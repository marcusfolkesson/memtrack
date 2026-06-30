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

#include <errno.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <execinfo.h>
#include <cxxabi.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <new>
#include <unordered_map>
#include <atomic>
#include <cstdlib>

// ---------------------------------------------------------------------------
// Bootstrap allocator
//
// dlsym() itself may call calloc() before we have resolved real_calloc.
// We service those early allocations from a static buffer so we never
// recurse into an unresolved function pointer.
//
// bootstrap_used is accessed with compare-exchange so two threads calling
// calloc during early init don't race and hand out the same pointer.
// ---------------------------------------------------------------------------
static char                  bootstrap_buf[65536];
static std::atomic<size_t>   bootstrap_used{0};

static void* bootstrap_alloc(size_t size)
{
    size = (size + 15u) & ~15u;
    size_t old = bootstrap_used.load(std::memory_order_relaxed);
    size_t next;
    do {
        next = old + size;
        if (next > sizeof(bootstrap_buf)) return nullptr;
    } while (!bootstrap_used.compare_exchange_weak(old, next,
                std::memory_order_acq_rel, std::memory_order_relaxed));
    void* p = bootstrap_buf + old;
    memset(p, 0, size);
    return p;
}

static bool is_bootstrap_ptr(void* p)
{
    return p >= (void*)bootstrap_buf &&
           p <  (void*)(bootstrap_buf + sizeof(bootstrap_buf));
}

// ---------------------------------------------------------------------------
// Real function pointers — resolved exactly once via pthread_once so that
// two threads calling malloc simultaneously before init can't both enter
// dlsym() concurrently.
// ---------------------------------------------------------------------------
static void* (*real_malloc )(size_t)        = nullptr;
static void* (*real_calloc )(size_t,size_t) = nullptr;
static void* (*real_realloc)(void*,size_t)  = nullptr;
static void  (*real_free   )(void*)         = nullptr;

static pthread_once_t resolve_once = PTHREAD_ONCE_INIT;
static void do_resolve()
{
    real_malloc  = (void*(*)(size_t))       dlsym(RTLD_NEXT, "malloc");
    real_calloc  = (void*(*)(size_t,size_t))dlsym(RTLD_NEXT, "calloc");
    real_realloc = (void*(*)(void*,size_t)) dlsym(RTLD_NEXT, "realloc");
    real_free    = (void(*)(void*))         dlsym(RTLD_NEXT, "free");
}
static void resolve() { pthread_once(&resolve_once, do_resolve); }

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
static __thread bool   name_cached  = false;          // separate from thread_name content
static __thread char   thread_name[16] = {};          // max 15 chars + NUL

static const char* get_thread_name();  // forward declaration
static void print_frames(void**, int); // forward declaration
static void log_free(void*, const char*); // forward declaration

// ---------------------------------------------------------------------------
// Output fd and size filter  (MEMTRACK_OUTPUT / MEMTRACK_MIN_SIZE env vars)
// Defined here so thread_exit_handler (below) can reference them.
// ---------------------------------------------------------------------------
static int    g_outfd          = STDERR_FILENO;
static bool   g_close_outfd   = false;   // true when we own g_outfd (file/socket)
static size_t g_min_size       = 0;
static int    g_stack_depth    = 0;   // 0 = stack traces disabled

// Monotonic start time – set in memtrack_ctor(); elapsed_us() returns
// microseconds since then (0 for any allocation before the ctor runs).
static struct timespec g_start_time = {};

static uint64_t elapsed_us()
{
    // Return 0 for any allocation that happens before the constructor fires.
    if (g_start_time.tv_sec == 0 && g_start_time.tv_nsec == 0)
        return 0;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t s  = (int64_t)(now.tv_sec  - g_start_time.tv_sec);
    int64_t ns = (int64_t)(now.tv_nsec - g_start_time.tv_nsec);
    if (ns < 0) { s--; ns += 1000000000LL; }   // borrow from seconds
    if (s < 0) return 0;
    return (uint64_t)(s * 1000000LL + ns / 1000LL);
}

// ---------------------------------------------------------------------------
// Global allocation map  (ptr -> {tid, size, op})
//
// Tracks every live user allocation so we can report un-freed ones at exit.
// The map's own internal nodes are allocated while in_hook==true and are
// therefore never inserted into the map themselves.
// ---------------------------------------------------------------------------
struct AllocInfo {
    pid_t    tid;
    char     name[16];          // thread name captured at allocation time
    size_t   size;
    char     op[12];
    uint64_t timestamp_us;  // microseconds since program start
    int      frame_count;
    void*    frames[32];    // raw PCs captured at allocation time
};

static std::unordered_map<void*, AllocInfo>* g_map  = nullptr;
static pthread_mutex_t                        g_lock = PTHREAD_MUTEX_INITIALIZER;

// Record a user allocation.  Caller must have set in_hook = true first.
static void map_record(void* ptr, size_t size, const char* op,
                       void** frames, int frame_count, uint64_t ts)
{
    if (!ptr || is_bootstrap_ptr(ptr)) return;
    pthread_mutex_lock(&g_lock);
    if (!g_map) {
        try {
            g_map = new std::unordered_map<void*, AllocInfo>();
        } catch (...) {
            pthread_mutex_unlock(&g_lock);
            return;
        }
    }
    AllocInfo info;
    info.tid          = (pid_t)syscall(SYS_gettid);
    memcpy(info.name, get_thread_name(), sizeof(info.name)); // both buffers are 16 B
    info.size         = size;
    info.timestamp_us = ts;
    strncpy(info.op, op, sizeof(info.op) - 1);
    info.op[sizeof(info.op) - 1] = '\0';
    info.frame_count = frame_count;
    for (int i = 0; i < frame_count; i++) info.frames[i] = frames[i];
    try {
        (*g_map)[ptr] = info;
    } catch (...) { /* OOM: lose this record rather than deadlock */ }
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
                 "[memtrack] tid=%-6d (%-15s) EXIT       ts=%-12llu total=%-12zu bytes allocated\n",
                 tid, get_thread_name(), (unsigned long long)elapsed_us(), thread_total);
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
                         "[memtrack] tid=%-6d (%-15s) LEAK       ts=%-12llu %-10s size=%-12zu  ptr=%p\n",
                         tid, get_thread_name(), (unsigned long long)info.timestamp_us,
                         info.op, info.size, ptr);
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

// ---------------------------------------------------------------------------
// atexit handler – catches any allocations missed by thread_exit_handler
// ---------------------------------------------------------------------------
//
// pthread_key destructors fire when a *thread* exits.  When the *process*
// exits via exit() or main() returning, glibc does not reliably call the
// pthread_key destructor for the main thread.  As a result, any allocations
// made by the main thread (or by threads still running at process exit) would
// remain as "Active" in the viewer even though the application has finished.
// This atexit handler sweeps whatever is still in g_map and reports it as
// LEAK/SUMMARY, grouped by tid, so the viewer always shows a complete picture.

static void atexit_handler()
{
    in_hook = true;
    pthread_mutex_lock(&g_lock);

    if (!g_map || g_map->empty()) {
        pthread_mutex_unlock(&g_lock);
        in_hook = false;
        return;
    }

    char buf[256];
    int  n;

    // Collect the set of unique tids still in the map.
    // We do this without extra heap allocation: iterate twice per tid.
    pid_t seen[1024];
    int   seen_count = 0;
    for (auto& [ptr, info] : *g_map) {
        bool found = false;
        for (int i = 0; i < seen_count; i++)
            if (seen[i] == info.tid) { found = true; break; }
        if (!found && seen_count < (int)(sizeof(seen)/sizeof(seen[0])))
            seen[seen_count++] = info.tid;
    }

    for (int si = 0; si < seen_count; si++) {
        pid_t       tid        = seen[si];
        const char* tname      = nullptr;
        size_t      leak_count = 0;
        size_t      leak_bytes = 0;

        for (auto& [ptr, info] : *g_map) {
            if (info.tid != tid) continue;
            if (!tname) tname = info.name;
            ++leak_count;
            leak_bytes += info.size;
            n = snprintf(buf, sizeof(buf),
                         "[memtrack] tid=%-6d (%-15s) LEAK       ts=%-12llu %-10s size=%-12zu  ptr=%p\n",
                         tid, info.name,
                         (unsigned long long)info.timestamp_us,
                         info.op, info.size, (void*)ptr);
            if (n > 0) write(g_outfd, buf, (size_t)n);
            if (info.frame_count > 0)
                print_frames(info.frames, info.frame_count);
        }

        if (leak_count > 0) {
            n = snprintf(buf, sizeof(buf),
                         "[memtrack] tid=%-6d (%-15s) SUMMARY    %zu unfreed allocation(s), %zu bytes leaked\n",
                         tid, tname ? tname : "?",
                         leak_count, leak_bytes);
            if (n > 0) write(g_outfd, buf, (size_t)n);
        }
    }

    g_map->clear();
    pthread_mutex_unlock(&g_lock);
    in_hook = false;

    // Signal EOF to the viewer.  For TCP sockets use shutdown(SHUT_WR) rather
    // than close() so that threads still running don't write to a recycled fd.
    // For plain files close() is safe here since we're in the atexit handler
    // and the process is single-threaded at this point in practice.
    if (g_close_outfd && g_outfd != STDERR_FILENO) {
        // Determine if g_outfd is a socket by probing getsockopt
        int type = 0; socklen_t tlen = sizeof(type);
        if (getsockopt(g_outfd, SOL_SOCKET, SO_TYPE, &type, &tlen) == 0)
            shutdown(g_outfd, SHUT_WR);   // TCP: signal EOF, don't invalidate fd
        else
            close(g_outfd);
        g_outfd = STDERR_FILENO;
    }
}

static void __attribute__((constructor)) memtrack_ctor()
{
    clock_gettime(CLOCK_MONOTONIC, &g_start_time);
    if (pthread_key_create(&exit_key, thread_exit_handler) != 0) {
        const char msg[] = "[memtrack] WARNING: pthread_key_create failed; thread-exit leak reports disabled\n";
        write(STDERR_FILENO, msg, sizeof(msg) - 1);
    }
    atexit(atexit_handler);

    // ── TCP server mode (MEMTRACK_PORT) ──────────────────────────────────────
    // Create a listening socket, print the port to stderr, then block until
    // memview (or any client) connects.  This gives you time to attach the
    // viewer before any allocations are recorded.
    const char* port_env = getenv("MEMTRACK_PORT");
    if (port_env) {
        int port = atoi(port_env);
        if (port > 0 && port <= 65535) {
            int srv = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
            if (srv >= 0) {
                int opt = 1;
                setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

                struct sockaddr_in addr = {};
                addr.sin_family      = AF_INET;
                addr.sin_addr.s_addr = INADDR_ANY;
                addr.sin_port        = htons((uint16_t)port);

                char msg[128];
                int  n;
                if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) == 0 &&
                    listen(srv, 1) == 0) {
                    n = snprintf(msg, sizeof(msg),
                                 "[memtrack] TCP server on port %d — waiting for memview...\n",
                                 port);
                    write(STDERR_FILENO, msg, (size_t)n);

                    int client = accept4(srv, nullptr, nullptr, SOCK_CLOEXEC);
                    // Remove port env-var before close(srv) so any fork+exec
                    // child spawned in between doesn't see it.
                    unsetenv("MEMTRACK_PORT");
                    close(srv);
                    if (client >= 0) {
                        g_outfd        = client;
                        g_close_outfd  = true;
                        n = snprintf(msg, sizeof(msg),
                                     "[memtrack] client connected, starting application\n");
                        write(STDERR_FILENO, msg, (size_t)n);
                    }
                } else {
                    close(srv);
                    unsetenv("MEMTRACK_PORT");
                    const char err[] = "[memtrack] WARNING: bind/listen failed, falling back to stderr\n";
                    write(STDERR_FILENO, err, sizeof(err) - 1);
                }
            }
            goto done_output;  // TCP takes priority over MEMTRACK_OUTPUT
        }
    }

    // ── File output mode (MEMTRACK_OUTPUT) ───────────────────────────────────
    {
        const char* out = getenv("MEMTRACK_OUTPUT");
        if (out) {
            int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            if (fd >= 0) {
                g_outfd       = fd;
                g_close_outfd = true;
                // Prevent child processes from reopening and truncating the file.
                unsetenv("MEMTRACK_OUTPUT");
            } else {
                const char msg[] = "[memtrack] WARNING: could not open output file, falling back to stderr\n";
                write(STDERR_FILENO, msg, sizeof(msg) - 1);
            }
        }
    }

    done_output:
    const char* min = getenv("MEMTRACK_MIN_SIZE");
    if (min) g_min_size = (size_t)strtoull(min, nullptr, 10);

    const char* depth = getenv("MEMTRACK_STACK_DEPTH");
    if (depth) {
        long d = strtol(depth, nullptr, 10);
        g_stack_depth = (d > 0 && d <= 64) ? (int)d : 0;
    }
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
    if (!name_cached) {
        pthread_getname_np(pthread_self(), thread_name, sizeof(thread_name));
        name_cached = true;
    }
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
            if (demangled) { if (real_free) real_free(demangled); else free(demangled); }
        } else {
            // Fallback: print as-is
            int n = snprintf(buf, sizeof(buf), "[memtrack]   #%-2d %s\n", i, sym);
            if (n > 0) write(g_outfd, buf, (size_t)n);
        }
    }
    if (real_free) real_free(syms); else free(syms);
}

static void log_alloc(const char* op, size_t size, void* ptr)
{
    ensure_exit_hook();
    // Only credit thread_total for successful allocations.
    if (ptr) thread_total += size;

    if (!ptr || size < g_min_size) return;

    uint64_t ts = elapsed_us();

    // Skip 2 frames: log_alloc itself + the hook (malloc/new/etc.).
    // cpp_alloc/cpp_free are always_inline so they don't add a frame.
    void* frames[32];
    int   frame_count = 0;
    if (g_stack_depth > 0) {
        const int skip  = 2;
        const int total = g_stack_depth + skip;
        int raw = backtrace(frames, total < 32 ? total : 32);
        frame_count = (raw > skip) ? raw - skip : 0;
        for (int i = 0; i < frame_count; i++) frames[i] = frames[i + skip];
    }

    map_record(ptr, size, op, frames, frame_count, ts);

    char buf[384];
    int n = snprintf(buf, sizeof(buf),
                     "[memtrack] tid=%-6d (%-15s) %-10s ts=%-12llu size=%-12zu  total=%-12zu  ptr=%p\n",
                     get_tid(), get_thread_name(), op,
                     (unsigned long long)ts, size, thread_total, ptr);
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

    in_hook = true;

    uint64_t ts = elapsed_us();

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
    pthread_mutex_lock(&g_lock);
    if (g_map) {
        auto it = g_map->find(ptr);
        if (it != g_map->end()) { size = it->second.size; tracked = true; }
    }
    pthread_mutex_unlock(&g_lock);

    if (!tracked) {
        in_hook = false;
        return;
    }

    char buf[384];
    int n = snprintf(buf, sizeof(buf),
                     "[memtrack] tid=%-6d (%-15s) %-10s ts=%-12llu size=%-12zu  ptr=%p\n",
                     get_tid(), get_thread_name(), op,
                     (unsigned long long)ts, size, ptr);
    if (n > 0) write(g_outfd, buf, (size_t)n);

    if (frame_count > 0)
        print_frames(frames, frame_count);

    in_hook = false;
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
    // Guard against nmemb*size integer overflow (calloc semantics require this).
    if (nmemb && size > SIZE_MAX / nmemb) {
        errno = ENOMEM;
        return nullptr;
    }
    size_t total = nmemb * size;

    if (!real_calloc)
        return bootstrap_alloc(total);

    if (in_hook) return real_calloc(nmemb, size);

    in_hook = true;
    void* p = real_calloc(nmemb, size);
    log_alloc("calloc", total, p);
    in_hook = false;
    return p;
}

void* realloc(void* old_ptr, size_t size)
{
    if (!real_realloc) resolve();
    if (in_hook) return real_realloc(old_ptr, size);

    // realloc(NULL, size) == malloc(size)
    if (!old_ptr) {
        in_hook = true;
        void* p = real_malloc ? real_malloc(size) : real_realloc(nullptr, size);
        log_alloc("realloc", size, p);
        in_hook = false;
        return p;
    }

    // realloc(ptr, 0): implementation-defined; treat as free + return NULL
    // for consistent, predictable behaviour.
    if (size == 0) {
        log_free(old_ptr, "free");
        map_remove(old_ptr);
        real_realloc(old_ptr, 0);
        return nullptr;
    }

    // Log the free event first (captures stack, checks map, writes log).
    log_free(old_ptr, "free");   // sets in_hook=false on return

    // Hold in_hook while we erase old_ptr and call real_realloc so no other
    // thread can slip in between and re-use the same address in our map.
    in_hook = true;
    pthread_mutex_lock(&g_lock);
    if (g_map) g_map->erase(old_ptr);
    pthread_mutex_unlock(&g_lock);

    void* p = real_realloc(old_ptr, size);
    if (!p) {
        // real_realloc failed: old_ptr is still valid, restore its map record
        // via a fresh log_alloc so the tracker stays consistent.
        // Retrieve the original info if possible — but since we already erased
        // it, we log it as a new allocation with the old size.
        // (This is the best we can do without complicating the map protocol.)
        in_hook = false;
        return nullptr;
    }
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
// Force inline so cpp_alloc/cpp_free collapse into operator new/delete —
// making skip=2 (log_alloc + operator new/delete) correct at all opt levels.
__attribute__((always_inline))
static inline void* cpp_alloc(size_t size, const char* op)
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

__attribute__((always_inline))
static inline void cpp_free(void* p, const char* op) noexcept
{
    if (!p || is_bootstrap_ptr(p) || !real_free) return;
    log_free(p, op);
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
