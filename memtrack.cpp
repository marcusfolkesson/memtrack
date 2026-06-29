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
#include <fcntl.h>
#include <execinfo.h>
#include <cxxabi.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <stdio.h>
#include <string.h>
#include <new>
#include <unordered_map>
#include <cstdlib>

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
static __thread char   thread_name[16] = {};   // cached on first use; max 15 chars + NUL

static const char* get_thread_name();  // forward declaration
static void print_frames(void**, int); // forward declaration
static void log_free(void*, const char*); // forward declaration

// ---------------------------------------------------------------------------
// Output fd and size filter  (MEMTRACK_OUTPUT / MEMTRACK_MIN_SIZE env vars)
// Defined here so thread_exit_handler (below) can reference them.
// ---------------------------------------------------------------------------
static int    g_outfd       = STDERR_FILENO;
static size_t g_min_size    = 0;
static int    g_stack_depth = 0;   // 0 = stack traces disabled

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
    int    frame_count;
    void*  frames[32];  // raw PCs captured at allocation time
};

static std::unordered_map<void*, AllocInfo>* g_map  = nullptr;
static pthread_mutex_t                        g_lock = PTHREAD_MUTEX_INITIALIZER;

// Record a user allocation.  Caller must have set in_hook = true first.
static void map_record(void* ptr, size_t size, const char* op,
                       void** frames, int frame_count)
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
    info.frame_count = frame_count;
    for (int i = 0; i < frame_count; i++) info.frames[i] = frames[i];
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
    char  buf[256];
    int   n;

    n = snprintf(buf, sizeof(buf),
                 "[memtrack] tid=%-6d (%-15s) EXIT       total=%-12zu bytes allocated\n",
                 tid, get_thread_name(), thread_total);
    if (n > 0) write(g_outfd, buf, (size_t)n);

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
                         "[memtrack] tid=%-6d (%-15s) LEAK       %-10s size=%-12zu  ptr=%p\n",
                         tid, get_thread_name(), info.op, info.size, ptr);
            if (n > 0) write(g_outfd, buf, (size_t)n);
            if (info.frame_count > 0)
                print_frames(info.frames, info.frame_count);
        }

        // Second pass: erase this thread's entries.
        // (in_hook==true prevents operator delete from trying to re-lock g_lock)
        for (auto it = g_map->begin(); it != g_map->end(); ) {
            it = (it->second.tid == tid) ? g_map->erase(it) : ++it;
        }

        if (leak_count > 0) {
            n = snprintf(buf, sizeof(buf),
                         "[memtrack] tid=%-6d (%-15s) SUMMARY    %zu unfreed allocation(s), %zu bytes leaked\n",
                         tid, get_thread_name(), leak_count, leak_bytes);
        } else {
            n = snprintf(buf, sizeof(buf),
                         "[memtrack] tid=%-6d (%-15s) SUMMARY    all allocations freed\n",
                         tid, get_thread_name());
        }
        if (n > 0) write(g_outfd, buf, (size_t)n);
    }

    pthread_mutex_unlock(&g_lock);
    in_hook = false;
}

static void __attribute__((constructor)) memtrack_ctor()
{
    pthread_key_create(&exit_key, thread_exit_handler);

    const char* out = getenv("MEMTRACK_OUTPUT");
    if (out) {
        int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0)
            g_outfd = fd;
        else {
            const char msg[] = "[memtrack] WARNING: could not open output file, falling back to stderr\n";
            write(STDERR_FILENO, msg, sizeof(msg) - 1);
        }
    }

    const char* min = getenv("MEMTRACK_MIN_SIZE");
    if (min) g_min_size = (size_t)strtoull(min, nullptr, 10);

    const char* depth = getenv("MEMTRACK_STACK_DEPTH");
    if (depth) g_stack_depth = atoi(depth);
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

static const char* get_thread_name()
{
    if (thread_name[0] == '\0')
        pthread_getname_np(pthread_self(), thread_name, sizeof(thread_name));
    return thread_name;
}

// Print symbolised stack frames.  Must be called with in_hook == true.
// backtrace_symbols() allocates internally; real_free() is used to release it
// so the freed buffer (which was never tracked) doesn't confuse map_remove.
//
// Each symbol string from backtrace_symbols looks like one of:
//   ./binary(mangled_name+0xNN) [0xADDR]   — Linux with -rdynamic
//   ./binary(+0xoffset) [0xADDR]            — no symbol
//   /lib/libc.so.6(func+0xNN) [0xADDR]
//
// We extract the mangled name (between '(' and '+'/')'), demangle it with
// __cxa_demangle, and reconstruct the string.
static void print_frames(void** frames, int count)
{
    char** syms = backtrace_symbols(frames, count);
    if (!syms) return;

    char buf[1024];
    for (int i = 0; i < count; i++) {
        const char* sym = syms[i];

        // Locate the mangled symbol between '(' and the first '+' or ')'
        const char* lparen = strchr(sym, '(');
        const char* plus   = lparen ? strpbrk(lparen, "+)") : nullptr;

        if (lparen && plus && plus > lparen + 1) {
            // Extract module (everything before '(')
            int mod_len = (int)(lparen - sym);

            // Extract mangled name
            int  name_len  = (int)(plus - lparen - 1);
            char mangled[512];
            if (name_len >= (int)sizeof(mangled)) name_len = (int)sizeof(mangled) - 1;
            memcpy(mangled, lparen + 1, (size_t)name_len);
            mangled[name_len] = '\0';

            // Demangle — __cxa_demangle allocates with malloc; release with real_free
            int   status   = -1;
            char* demangled = abi::__cxa_demangle(mangled, nullptr, nullptr, &status);
            const char* display = (status == 0 && demangled) ? demangled : mangled;

            // Remainder: "+0xNN) [0xADDR]"
            const char* rest = plus;

            int n = snprintf(buf, sizeof(buf), "[memtrack]   #%-2d %.*s(%s%s\n",
                             i, mod_len, sym, display, rest);
            if (n > 0) write(g_outfd, buf, (size_t)n);
            if (demangled) real_free(demangled);
        } else {
            // Fallback: print as-is
            int n = snprintf(buf, sizeof(buf), "[memtrack]   #%-2d %s\n", i, sym);
            if (n > 0) write(g_outfd, buf, (size_t)n);
        }
    }
    real_free(syms);
}

static void log_alloc(const char* op, size_t size, void* ptr)
{
    ensure_exit_hook();
    thread_total += size;

    if (size < g_min_size) return;

    // Capture stack frames before doing anything else so the trace is accurate.
    // Skip 2 internal frames: log_alloc itself + the hook (malloc/calloc/etc.).
    void* frames[32];
    int   frame_count = 0;
    if (g_stack_depth > 0) {
        const int skip  = 2;
        const int total = g_stack_depth + skip;
        int raw = backtrace(frames, total < 32 ? total : 32);
        frame_count = (raw > skip) ? raw - skip : 0;
        // Shift: drop the first `skip` internal frames
        for (int i = 0; i < frame_count; i++) frames[i] = frames[i + skip];
    }

    map_record(ptr, size, op, frames, frame_count);

    char buf[256];
    int n = snprintf(buf, sizeof(buf),
                     "[memtrack] tid=%-6d (%-15s) %-10s size=%-12zu  total=%-12zu  ptr=%p\n",
                     get_tid(), get_thread_name(), op, size, thread_total, ptr);
    if (n > 0)
        write(g_outfd, buf, (size_t)n);

    if (frame_count > 0)
        print_frames(frames, frame_count);
}

// Log a free/delete.  Only logs if the pointer is tracked in g_map (meaning
// the allocation passed the size filter); untracked pointers are silently ignored.
static void log_free(void* ptr, const char* op)
{
    if (!ptr || is_bootstrap_ptr(ptr) || in_hook) return;

    // Capture stack trace before touching the map.
    void* frames[32];
    int   frame_count = 0;
    if (g_stack_depth > 0) {
        const int skip  = 2;
        const int total = g_stack_depth + skip;
        int raw = backtrace(frames, total < 32 ? total : 32);
        frame_count = (raw > skip) ? raw - skip : 0;
        for (int i = 0; i < frame_count; i++) frames[i] = frames[i + skip];
    }

    // Look up the allocation size.  If the pointer is not in the map it was
    // either filtered out at allocation time or allocated before memtrack loaded.
    size_t size    = 0;
    bool   tracked = false;
    in_hook = true;
    pthread_mutex_lock(&g_lock);
    if (g_map) {
        auto it = g_map->find(ptr);
        if (it != g_map->end()) { size = it->second.size; tracked = true; }
    }
    pthread_mutex_unlock(&g_lock);
    in_hook = false;

    if (!tracked) return;

    char buf[256];
    int n = snprintf(buf, sizeof(buf),
                     "[memtrack] tid=%-6d (%-15s) %-10s size=%-12zu  ptr=%p\n",
                     get_tid(), get_thread_name(), op, size, ptr);
    if (n > 0) write(g_outfd, buf, (size_t)n);

    if (frame_count > 0)
        print_frames(frames, frame_count);
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
    log_free(ptr, "free");
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

static void cpp_free(void* p, const char* op) noexcept
{
    if (!p || is_bootstrap_ptr(p) || !real_free) return;
    log_free(p, op);
    // map_remove is a no-op when in_hook==true, preventing re-entry into the
    // map while we are already inside map_record/map_remove/thread_exit_handler.
    map_remove(p);
    real_free(p);
}

void* operator new  (size_t s)            { return cpp_alloc(s, "new");   }
void* operator new[](size_t s)            { return cpp_alloc(s, "new[]"); }

void  operator delete  (void* p) noexcept { cpp_free(p, "delete");    }
void  operator delete[](void* p) noexcept { cpp_free(p, "delete[]");  }

// Sized-delete overloads (C++14)
void  operator delete  (void* p, size_t) noexcept { cpp_free(p, "delete");   }
void  operator delete[](void* p, size_t) noexcept { cpp_free(p, "delete[]"); }
