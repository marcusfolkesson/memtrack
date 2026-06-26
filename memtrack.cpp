/*
 * memtrack.cpp – LD_PRELOAD memory allocation tracker
 *
 * Overrides malloc/calloc/realloc/free and operator new/delete.
 * Per allocation: prints size, thread ID, and cumulative thread total.
 * On thread exit: reports any pointers allocated by that thread that were
 *                 never freed (memory leaks).
 *
 * Build:  g++ -O2 -std=c++17 -fPIC -shared -o memtrack.so memtrack.cpp -ldl -lpthread
 * Usage:  LD_PRELOAD=./memtrack.so ./your_app
 */

#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <stdio.h>
#include <string.h>
#include <new>
#include <unordered_map>

// ---------------------------------------------------------------------------
// Bootstrap allocator
//
// dlsym() itself may call calloc() before we have resolved real_calloc.
// We service those early allocations from a static buffer so we never
// recurse into an unresolved function pointer.
// ---------------------------------------------------------------------------
static char   bootstrap_buf[65536];
static size_t bootstrap_used = 0;

static void* bootstrap_alloc(size_t size)
{
    size = (size + 15u) & ~15u;
    if (bootstrap_used + size > sizeof(bootstrap_buf)) return nullptr;
    void* p = bootstrap_buf + bootstrap_used;
    bootstrap_used += size;
    memset(p, 0, size);
    return p;
}

static bool is_bootstrap_ptr(void* p)
{
    return p >= (void*)bootstrap_buf &&
           p <  (void*)(bootstrap_buf + sizeof(bootstrap_buf));
}

// ---------------------------------------------------------------------------
// Real function pointers
// ---------------------------------------------------------------------------
static void* (*real_malloc )(size_t)        = nullptr;
static void* (*real_calloc )(size_t,size_t) = nullptr;
static void* (*real_realloc)(void*,size_t)  = nullptr;
static void  (*real_free   )(void*)         = nullptr;

static void resolve()
{
    real_malloc  = (void*(*)(size_t))       dlsym(RTLD_NEXT, "malloc");
    real_calloc  = (void*(*)(size_t,size_t))dlsym(RTLD_NEXT, "calloc");
    real_realloc = (void*(*)(void*,size_t)) dlsym(RTLD_NEXT, "realloc");
    real_free    = (void(*)(void*))         dlsym(RTLD_NEXT, "free");
}

// ---------------------------------------------------------------------------
// Per-thread state
//
// in_hook: prevents recursive hook calls and — crucially — signals that any
//   deallocation happening right now is an internal one (e.g. the map freeing
//   its own nodes).  When in_hook is true we must NOT try to re-enter the map.
// ---------------------------------------------------------------------------
static __thread bool   in_hook      = false;
static __thread size_t thread_total = 0;
static __thread bool   key_set      = false;

// ---------------------------------------------------------------------------
// Global allocation map  (ptr -> {tid, size, op})
//
// Tracks every live user allocation so we can report un-freed ones at exit.
// The map's own internal nodes are allocated while in_hook==true and are
// therefore never inserted into the map themselves.
// ---------------------------------------------------------------------------
struct AllocInfo {
    pid_t  tid;
    size_t size;
    char   op[12];
};

static std::unordered_map<void*, AllocInfo>* g_map  = nullptr;
static pthread_mutex_t                        g_lock = PTHREAD_MUTEX_INITIALIZER;

// Record a user allocation.  Caller must have set in_hook = true first.
static void map_record(void* ptr, size_t size, const char* op)
{
    if (!ptr || is_bootstrap_ptr(ptr)) return;
    pthread_mutex_lock(&g_lock);
    if (!g_map)
        g_map = new std::unordered_map<void*, AllocInfo>();
    AllocInfo info;
    info.tid  = (pid_t)syscall(SYS_gettid);
    info.size = size;
    strncpy(info.op, op, sizeof(info.op) - 1);
    info.op[sizeof(info.op) - 1] = '\0';
    (*g_map)[ptr] = info;
    pthread_mutex_unlock(&g_lock);
}

// Remove a freed pointer.  Only acts when in_hook is false (i.e. this is a
// genuine user free, not the map deallocating its own internal nodes).
static void map_remove(void* ptr)
{
    if (!ptr || is_bootstrap_ptr(ptr) || in_hook) return;
    in_hook = true;
    pthread_mutex_lock(&g_lock);
    if (g_map) g_map->erase(ptr);
    pthread_mutex_unlock(&g_lock);
    in_hook = false;
}

// ---------------------------------------------------------------------------
// Thread-exit handler – prints total and any unfreed allocations
// ---------------------------------------------------------------------------
static pthread_key_t exit_key;

static void thread_exit_handler(void*)
{
    pid_t tid = (pid_t)syscall(SYS_gettid);
    char  buf[200];
    int   n;

    n = snprintf(buf, sizeof(buf),
                 "[memtrack] tid=%-6d EXIT       total=%-12zu bytes allocated\n",
                 tid, thread_total);
    if (n > 0) write(STDERR_FILENO, buf, (size_t)n);

    // Set in_hook so that g_map->erase() below does not try to re-enter the map.
    in_hook = true;
    pthread_mutex_lock(&g_lock);

    if (g_map) {
        size_t leak_count = 0;
        size_t leak_bytes = 0;

        // First pass: report leaks belonging to this thread.
        for (auto& [ptr, info] : *g_map) {
            if (info.tid != tid) continue;
            ++leak_count;
            leak_bytes += info.size;
            n = snprintf(buf, sizeof(buf),
                         "[memtrack] tid=%-6d LEAK       %-10s size=%-12zu  ptr=%p\n",
                         tid, info.op, info.size, ptr);
            if (n > 0) write(STDERR_FILENO, buf, (size_t)n);
        }

        // Second pass: erase this thread's entries.
        // (in_hook==true prevents operator delete from trying to re-lock g_lock)
        for (auto it = g_map->begin(); it != g_map->end(); ) {
            it = (it->second.tid == tid) ? g_map->erase(it) : ++it;
        }

        if (leak_count > 0) {
            n = snprintf(buf, sizeof(buf),
                         "[memtrack] tid=%-6d SUMMARY    %zu unfreed allocation(s), %zu bytes leaked\n",
                         tid, leak_count, leak_bytes);
        } else {
            n = snprintf(buf, sizeof(buf),
                         "[memtrack] tid=%-6d SUMMARY    all allocations freed\n", tid);
        }
        if (n > 0) write(STDERR_FILENO, buf, (size_t)n);
    }

    pthread_mutex_unlock(&g_lock);
    in_hook = false;
}

static void __attribute__((constructor)) memtrack_ctor()
{
    pthread_key_create(&exit_key, thread_exit_handler);
}

static inline void ensure_exit_hook()
{
    if (!key_set) {
        pthread_setspecific(exit_key, (void*)1);
        key_set = true;
    }
}

// ---------------------------------------------------------------------------
// Logging  (must be called with in_hook == true)
// ---------------------------------------------------------------------------
static pid_t get_tid()
{
    return (pid_t)syscall(SYS_gettid);
}

static void log_alloc(const char* op, size_t size, void* ptr)
{
    ensure_exit_hook();
    thread_total += size;
    map_record(ptr, size, op);

    char buf[200];
    int n = snprintf(buf, sizeof(buf),
                     "[memtrack] tid=%-6d %-10s size=%-12zu  total=%-12zu  ptr=%p\n",
                     get_tid(), op, size, thread_total, ptr);
    if (n > 0)
        write(STDERR_FILENO, buf, (size_t)n);
}

// ---------------------------------------------------------------------------
// C API overrides
// ---------------------------------------------------------------------------
extern "C" {

void* malloc(size_t size)
{
    if (!real_malloc) resolve();
    if (in_hook) return real_malloc(size);

    in_hook = true;
    void* p = real_malloc(size);
    log_alloc("malloc", size, p);
    in_hook = false;
    return p;
}

void* calloc(size_t nmemb, size_t size)
{
    if (!real_calloc)
        return bootstrap_alloc(nmemb * size);

    if (in_hook) return real_calloc(nmemb, size);

    in_hook = true;
    void* p = real_calloc(nmemb, size);
    log_alloc("calloc", nmemb * size, p);
    in_hook = false;
    return p;
}

void* realloc(void* old_ptr, size_t size)
{
    if (!real_realloc) resolve();
    if (in_hook) return real_realloc(old_ptr, size);

    // Remove the old pointer before calling real_realloc: the old address
    // becomes invalid regardless of whether the block is moved.
    map_remove(old_ptr);

    in_hook = true;
    void* p = real_realloc(old_ptr, size);
    log_alloc("realloc", size, p);
    in_hook = false;
    return p;
}

void free(void* ptr)
{
    if (!ptr || is_bootstrap_ptr(ptr)) return;
    if (!real_free) resolve();
    // map_remove is a no-op when in_hook==true, so internal frees are safe.
    map_remove(ptr);
    real_free(ptr);
}

} // extern "C"

// ---------------------------------------------------------------------------
// C++ operator new / delete
// ---------------------------------------------------------------------------
static void* cpp_alloc(size_t size, const char* op)
{
    if (!real_malloc) resolve();

    if (in_hook) {
        void* p = real_malloc(size);
        if (!p) throw std::bad_alloc();
        return p;
    }

    in_hook = true;
    void* p = real_malloc(size);
    if (!p) {
        in_hook = false;
        throw std::bad_alloc();
    }
    log_alloc(op, size, p);
    in_hook = false;
    return p;
}

static void cpp_free(void* p) noexcept
{
    if (!p || is_bootstrap_ptr(p) || !real_free) return;
    // map_remove is a no-op when in_hook==true, preventing re-entry into the
    // map while we are already inside map_record/map_remove/thread_exit_handler.
    map_remove(p);
    real_free(p);
}

void* operator new  (size_t s)            { return cpp_alloc(s, "new");   }
void* operator new[](size_t s)            { return cpp_alloc(s, "new[]"); }

void  operator delete  (void* p) noexcept { cpp_free(p); }
void  operator delete[](void* p) noexcept { cpp_free(p); }

// Sized-delete overloads (C++14)
void  operator delete  (void* p, size_t) noexcept { cpp_free(p); }
void  operator delete[](void* p, size_t) noexcept { cpp_free(p); }
