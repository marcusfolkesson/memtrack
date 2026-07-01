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
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <new>
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
// Per-thread frame-filter result: -1=not yet checked, 0=no frames, 1=capture frames
static __thread int    frames_enabled = -1;

static const char* get_thread_name();  // forward declaration
static void log_free(void*, const char*); // forward declaration

// ---------------------------------------------------------------------------
// Output fd and size filter  (MEMTRACK_OUTPUT / MEMTRACK_MIN_SIZE env vars)
// Defined here so thread_exit_handler (below) can reference them.
// ---------------------------------------------------------------------------
static std::atomic<int> g_outfd{STDERR_FILENO};
static bool   g_close_outfd   = false;   // true when we own g_outfd (file/socket)
static size_t g_min_size       = 0;
static int    g_stack_depth    = 0;       // 0 = stack traces disabled
static size_t g_buffer_size    = 4096;    // per-thread write buffer (PIPE_BUF); 0 = unbuffered

// Thread-name filter for stack traces.  If non-empty, only threads whose name
// contains one of the substrings (case-sensitive) capture frames.
// Parsed from MEMTRACK_STACK_THREADS=name1,name2,...
static char   g_stack_threads[512]   = {};     // raw copy of the env value
static char*  g_stack_tnames[64]     = {};     // pointers into g_stack_threads
static int    g_stack_tname_count    = 0;      // 0 = no filter (all threads)

// Per-thread output buffer.  Used when g_buffer_size > 0.
// Allocated lazily with real_malloc to avoid tracking the buffer itself.
struct TLBuf {
    char*  data = nullptr;
    size_t len  = 0;

    void init() {
        if (data || !real_malloc) return;
        data = static_cast<char*>(real_malloc(g_buffer_size));
        // On allocation failure, data stays null — outfd_write falls back to
        // direct writes.
    }

    void flush() {
        if (!data || len == 0) return;
        write(g_outfd.load(std::memory_order_relaxed), data, len);
        len = 0;
    }

    // Append bytes to the buffer, flushing first if there is not enough room.
    void append(const char* p, size_t n) {
        if (!data) init();
        if (!data) {
            // Buffer allocation failed — fall back to a direct write.
            write(g_outfd.load(std::memory_order_relaxed), p, n);
            return;
        }
        if (len + n > g_buffer_size) flush();
        if (n > g_buffer_size) {
            // Single event larger than the whole buffer — write directly.
            write(g_outfd.load(std::memory_order_relaxed), p, n);
            return;
        }
        memcpy(data + len, p, n);
        len += n;
    }
};
static thread_local TLBuf tl_buf;

static void outfd_write(const void* buf, size_t n)
{
    if (n == 0) return;
    if (g_buffer_size > 0)
        tl_buf.append(static_cast<const char*>(buf), n);
    else
        write(g_outfd.load(std::memory_order_relaxed), buf, n);
}

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
// Thread-exit handler – logs EXIT so memview can detect leaked allocations
// ---------------------------------------------------------------------------
static pthread_key_t exit_key;

static void thread_exit_handler(void*)
{
    pid_t tid = (pid_t)syscall(SYS_gettid);
    char  buf[256];

    // Flush any buffered events before emitting EXIT so the log is ordered.
    tl_buf.flush();

    int n = snprintf(buf, sizeof(buf),
                     "[memtrack] tid=%-6d (%-15s) EXIT       ts=%-12llu total=%-12zu bytes allocated\n",
                     tid, get_thread_name(), (unsigned long long)elapsed_us(), thread_total);
    if (n > 0) outfd_write(buf, (size_t)n);
    // EXIT itself is small enough to flush immediately in buffered mode too.
    tl_buf.flush();
    // Leak detection is handled entirely by memview: it matches alloc/free
    // ptr fields in the log, using EXIT as the "thread is done" marker.
}

// ---------------------------------------------------------------------------
// atexit handler – signals EOF to the viewer once the process finishes
// ---------------------------------------------------------------------------
//
// pthread_key destructors fire when a *thread* exits, but glibc does not
// reliably call them for the main thread when the process exits via exit()
// or main() returning.  This atexit handler closes the output fd, which
// signals EOF to a connected memview instance so it can finalize leak
// detection from the log.

static void atexit_handler()
{
    // Flush the main thread's buffer before closing — worker threads flushed
    // in their own thread_exit_handler calls.
    tl_buf.flush();

    // Signal EOF to the viewer.  For TCP sockets use shutdown(SHUT_WR) rather
    // than close() so that threads still running don't write to a recycled fd.
    if (g_close_outfd) {
        int old_fd = g_outfd.exchange(STDERR_FILENO, std::memory_order_acq_rel);
        if (old_fd != STDERR_FILENO) {
            int type = 0; socklen_t tlen = sizeof(type);
            if (getsockopt(old_fd, SOL_SOCKET, SO_TYPE, &type, &tlen) == 0)
                shutdown(old_fd, SHUT_WR);
            else
                close(old_fd);
        }
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
                        g_outfd.store(client, std::memory_order_relaxed);
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
                g_outfd.store(fd, std::memory_order_relaxed);
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

    const char* bufsz = getenv("MEMTRACK_BUFFER_SIZE");
    if (bufsz) {
        char* end = nullptr;
        unsigned long long v = strtoull(bufsz, &end, 10);
        if (end && *end) {
            if (*end == 'k' || *end == 'K') v *= 1024ULL;
            else if (*end == 'm' || *end == 'M') v *= 1024ULL * 1024ULL;
        }
        g_buffer_size = (size_t)v;
    }

    const char* sthr = getenv("MEMTRACK_STACK_THREADS");
    if (sthr && *sthr) {
        // Copy into g_stack_threads and split on commas.
        strncpy(g_stack_threads, sthr, sizeof(g_stack_threads) - 1);
        char* p = g_stack_threads;
        while (*p && g_stack_tname_count < 64) {
            g_stack_tnames[g_stack_tname_count++] = p;
            char* comma = strchr(p, ',');
            if (!comma) break;
            *comma = '\0';
            p = comma + 1;
        }
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

// Returns true if the current thread should capture stack frames.
// Result is cached per-thread after the name is established.
// Re-evaluates if the name was not yet set when last checked (empty string).
static bool thread_wants_frames()
{
    if (g_stack_depth == 0) return false;

    // Re-check if not yet resolved or if name was empty last time.
    if (frames_enabled < 0 || (frames_enabled == 0 && !name_cached)) {
        if (g_stack_tname_count == 0) {
            // No filter: all threads capture frames.
            frames_enabled = 1;
        } else {
            const char* name = get_thread_name();
            frames_enabled = 0;
            if (name[0] != '\0') {
                for (int i = 0; i < g_stack_tname_count; i++) {
                    if (strstr(name, g_stack_tnames[i])) {
                        frames_enabled = 1;
                        break;
                    }
                }
            }
            // If name is still empty we leave frames_enabled=0 and
            // allow re-evaluation next call (name not established yet).
            if (name[0] == '\0') frames_enabled = -1;
        }
    }
    return frames_enabled == 1;
}

// Write an event line + optional stack frames as a single atomic write().
// For large events (deep template names), a heap buffer is allocated via
// real_malloc so the output is never truncated and always ends with '\n'.
static void write_event(const char* event_line, int event_len,
                        void** frames, int frame_count)
{
    // Fast path: no frames — write the event line directly.
    if (frame_count == 0 || !frames) {
        outfd_write(event_line, (size_t)event_len);
        return;
    }

    // Resolve symbols first so we know the exact size needed.
    // backtrace_symbols calls malloc internally; in_hook stays true.
    char** syms = backtrace_symbols(frames, frame_count);

    // Calculate exact buffer size: event line + one line per frame.
    int needed = event_len;
    if (syms) {
        for (int i = 0; i < frame_count; i++)
            needed += 20 + (int)strlen(syms[i]) + 2; // "#NN  " + sym + "\n"
    }
    needed += 1; // null terminator

    // Use a stack buffer for small events, heap for anything larger.
    char  stack_buf[8192];
    char* buf  = (needed <= (int)sizeof(stack_buf))
                 ? stack_buf
                 : (real_malloc ? (char*)real_malloc((size_t)needed) : nullptr);

    if (!buf) {
        // Allocation failed: emit event line alone, discard frames.
        outfd_write(event_line, (size_t)event_len);
        if (syms) { if (real_free) real_free(syms); else free(syms); }
        return;
    }

    memcpy(buf, event_line, (size_t)event_len);
    int off = event_len;
    if (syms) {
        for (int i = 0; i < frame_count; i++) {
            int n = snprintf(buf + off, (size_t)(needed - off),
                             "[memtrack]   #%-2d %s\n", i, syms[i]);
            if (n > 0 && n < needed - off) off += n;
        }
        if (real_free) real_free(syms); else free(syms);
    }

    outfd_write(buf, (size_t)off);

    if (buf != stack_buf) { if (real_free) real_free(buf); else free(buf); }
}

static void log_alloc(const char* op, size_t size, void* ptr)
{
    ensure_exit_hook();
    if (ptr) thread_total += size;

    if (!ptr || size < g_min_size) return;

    uint64_t ts = elapsed_us();

    void* frames[68]; // g_stack_depth max=64 + up to 3 skip frames + 1 spare
    int   frame_count = 0;
    if (thread_wants_frames()) {
        // Skip 2: log_alloc + the hook (malloc/calloc/new/etc.).
        // cpp_alloc is always_inline so it adds no frame.
        const int skip  = 2;
        const int total = g_stack_depth + skip;
        int raw = backtrace(frames, total < 68 ? total : 68);
        frame_count = (raw > skip) ? raw - skip : 0;
        for (int i = 0; i < frame_count; i++) frames[i] = frames[i + skip];
    }

    char event[256];
    int n = snprintf(event, sizeof(event),
                     "[memtrack] tid=%-6d (%-15s) %-10s ts=%-12llu size=%-12zu  total=%-12zu  ptr=%p\n",
                     get_tid(), get_thread_name(), op,
                     (unsigned long long)ts, size, thread_total, ptr);
    if (n > 0) write_event(event, n, frames, frame_count);
}

// Log a free/delete event + optional frames in a single write().
// skip: frames to discard so #0 lands on user code.
//   2 = called directly from free hook or cpp_free (always_inline)
//   3 = called via log_free wrapper (one extra non-inlined frame for realloc)
// On return, in_hook is TRUE; the caller must call real_free then clear in_hook.
static bool log_free_capture(void* ptr, const char* op,
                              int skip = 2)
{
    if (!ptr || is_bootstrap_ptr(ptr) || in_hook) return false;

    in_hook = true;

    uint64_t ts = elapsed_us();

    void* frames[68]; // g_stack_depth max=64 + up to 3 skip frames + 1 spare
    int   frame_count = 0;
    if (thread_wants_frames()) {
        const int total = g_stack_depth + skip;
        int raw = backtrace(frames, total < 68 ? total : 68);
        frame_count = (raw > skip) ? raw - skip : 0;
        for (int i = 0; i < frame_count; i++) frames[i] = frames[i + skip];
    }

    char event[256];
    int n = snprintf(event, sizeof(event),
                     "[memtrack] tid=%-6d (%-15s) %-10s ts=%-12llu  ptr=%p\n",
                     get_tid(), get_thread_name(), op, (unsigned long long)ts, ptr);
    if (n > 0) write_event(event, n, frames, frame_count);

    return true;
}

// log_free: convenience wrapper for realloc — logs a synthetic free without
// calling real_free (realloc does that itself via real_realloc).
// Uses skip=3: log_free_capture + log_free + realloc are all on the stack.
static void log_free(void* ptr, const char* op)
{
    if (!log_free_capture(ptr, op, 3)) return;
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
        real_realloc(old_ptr, 0);
        return nullptr;
    }

    // Log the implicit free of old_ptr before calling real_realloc.
    // This ensures the log has a matching free entry even when real_realloc
    // returns the same address (in-place growth).
    log_free(old_ptr, "free");

    in_hook = true;

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

    bool tracked = log_free_capture(ptr, "free");
    real_free(ptr);
    if (tracked) in_hook = false;
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

    bool tracked = log_free_capture(p, op);
    real_free(p);
    if (tracked) in_hook = false;
}

void* operator new  (size_t s)            { return cpp_alloc(s, "new");   }
void* operator new[](size_t s)            { return cpp_alloc(s, "new[]"); }

void  operator delete  (void* p) noexcept { cpp_free(p, "delete");    }
void  operator delete[](void* p) noexcept { cpp_free(p, "delete[]");  }

// Sized-delete overloads (C++14)
void  operator delete  (void* p, size_t) noexcept { cpp_free(p, "delete");   }
void  operator delete[](void* p, size_t) noexcept { cpp_free(p, "delete[]"); }
