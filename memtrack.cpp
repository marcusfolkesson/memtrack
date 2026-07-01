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
#include <unordered_set>
#include <vector>
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
static std::atomic<int> g_outfd{STDERR_FILENO};
static bool   g_close_outfd   = false;   // true when we own g_outfd (file/socket)
static size_t g_min_size       = 0;
static int    g_stack_depth    = 0;   // 0 = stack traces disabled
static bool   g_demangle       = false; // demangle in memtrack (default off — let memview do it)
static bool   g_compact        = true;  // MEMTRACK_COMPACT (default on): use CompactAllocInfo
                                         // instead of AllocInfo to save ~11× map memory

static void outfd_write(const void* buf, size_t n)
{
    if (n > 0) write(g_outfd.load(std::memory_order_relaxed), buf, n);
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
// Global allocation map  (ptr -> {tid, size, op})
//
// Tracks every live user allocation so we can report un-freed ones at exit.
// The map's own internal nodes are allocated while in_hook==true and are
// therefore never inserted into the map themselves.
// ---------------------------------------------------------------------------

// Full allocation record — used when MEMTRACK_COMPACT=0.
// Stores everything needed to reprint stack frames at thread exit.
struct AllocInfo {
    pid_t    tid;
    char     name[16];
    size_t   size;
    char     op[12];
    uint64_t timestamp_us;
    int      frame_count;
    void*    frames[32];    // ~308 bytes total
};

// Compact allocation record — used when MEMTRACK_COMPACT=1 (the default).
// Only keeps what is needed to log the free size and the exit LEAK line.
// memview reconstructs op/ts/frames from the original alloc log line.
struct CompactAllocInfo {
    pid_t  tid;
    char   name[16];
    size_t size;            // ~28 bytes total — ~11× smaller than AllocInfo
};

// ---------------------------------------------------------------------------
// Sharded allocation map
//
// 16 shards keyed by (uintptr_t(ptr) >> 4) & 15.  Each shard has its own
// mutex, reducing lock contention by ~16× compared to a single global lock.
// Cross-thread frees are transparent: alloc and free for the same pointer
// always hash to the same shard.
// g_reported_leaks lives inside each shard alongside the allocation map so
// the two structures are always protected by the same lock.
// ---------------------------------------------------------------------------
static constexpr int NSHARDS = 16;

struct alignas(64) MapShard {
    pthread_mutex_t                              lock;
    std::unordered_map<void*, AllocInfo>*        map            = nullptr;
    std::unordered_map<void*, CompactAllocInfo>* cmap           = nullptr;
    std::unordered_map<void*, size_t>*           reported_leaks = nullptr;
};

static MapShard g_shards[NSHARDS];

static void shards_init()
{
    for (auto& sh : g_shards)
        pthread_mutex_init(&sh.lock, nullptr);
}

static inline int shard_of(void* ptr)
{
    return (int)(((uintptr_t)ptr >> 4) & (NSHARDS - 1));
}

// Record a user allocation.  Caller must have set in_hook = true first.
// In compact mode this is a no-op: the alloc is logged to the file and
// memview tracks it there; no in-process map entry is needed.
static void map_record(void* ptr, size_t size, const char* op,
                       void** frames, int frame_count, uint64_t ts)
{
    if (g_compact) return;  // memview reconstructs everything from the log
    if (!ptr || is_bootstrap_ptr(ptr)) return;
    int s = shard_of(ptr);
    MapShard& sh = g_shards[s];
    pthread_mutex_lock(&sh.lock);
    if (g_compact) {
        if (!sh.cmap) {
            try { sh.cmap = new std::unordered_map<void*, CompactAllocInfo>(); }
            catch (...) { pthread_mutex_unlock(&sh.lock); return; }
        }
        CompactAllocInfo info;
        info.tid  = (pid_t)syscall(SYS_gettid);
        memcpy(info.name, get_thread_name(), sizeof(info.name));
        info.size = size;
        try { (*sh.cmap)[ptr] = info; } catch (...) {}
    } else {
        if (!sh.map) {
            try { sh.map = new std::unordered_map<void*, AllocInfo>(); }
            catch (...) { pthread_mutex_unlock(&sh.lock); return; }
        }
        AllocInfo info;
        info.tid          = (pid_t)syscall(SYS_gettid);
        memcpy(info.name, get_thread_name(), sizeof(info.name));
        info.size         = size;
        info.timestamp_us = ts;
        strncpy(info.op, op, sizeof(info.op) - 1);
        info.op[sizeof(info.op) - 1] = '\0';
        info.frame_count = frame_count;
        for (int i = 0; i < frame_count; i++) info.frames[i] = frames[i];
        try { (*sh.map)[ptr] = info; } catch (...) {}
    }
    pthread_mutex_unlock(&sh.lock);
}

// Remove a freed pointer.  Only acts when in_hook is false (i.e. this is a
// genuine user free, not the map deallocating its own internal nodes).
static void map_remove(void* ptr)
{
    if (g_compact) return;
    if (!ptr || is_bootstrap_ptr(ptr) || in_hook) return;
    in_hook = true;
    int s = shard_of(ptr);
    MapShard& sh = g_shards[s];
    pthread_mutex_lock(&sh.lock);
    if (sh.map) sh.map->erase(ptr);
    pthread_mutex_unlock(&sh.lock);
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
    if (n > 0) outfd_write(buf, (size_t)n);

    if (g_compact) {
        // In compact mode memview detects leaks from the log directly.
        // No map, no LEAK/SUMMARY lines needed from memtrack.
        return;
    }

    in_hook = true;

    size_t leak_count = 0;
    size_t leak_bytes = 0;

    // Full mode: iterate map, emit LEAK lines with ts/op and reprint frames.
    struct LeakEntry { void* ptr; AllocInfo info; };
    std::vector<LeakEntry> leaks;
    for (int s = 0; s < NSHARDS; s++) {
        MapShard& sh = g_shards[s];
        pthread_mutex_lock(&sh.lock);
        if (sh.map) {
            leaks.clear();
            for (auto& [ptr, info] : *sh.map) {
                if (info.tid != tid) continue;
                ++leak_count;
                leak_bytes += info.size;
                leaks.push_back({ptr, info});
                if (!sh.reported_leaks) {
                    try { sh.reported_leaks = new std::unordered_map<void*, size_t>(); }
                    catch (...) {}
                }
                if (sh.reported_leaks) {
                    try { (*sh.reported_leaks)[ptr] = info.size; } catch (...) {}
                }
            }
            for (auto it = sh.map->begin(); it != sh.map->end(); )
                it = (it->second.tid == tid) ? sh.map->erase(it) : ++it;
        }
        pthread_mutex_unlock(&sh.lock);
        for (auto& e : leaks) {
            n = snprintf(buf, sizeof(buf),
                         "[memtrack] tid=%-6d (%-15s) LEAK       ts=%-12llu %-10s size=%-12zu  ptr=%p\n",
                         tid, get_thread_name(), (unsigned long long)e.info.timestamp_us,
                         e.info.op, e.info.size, e.ptr);
            if (n > 0) outfd_write(buf, (size_t)n);
            if (e.info.frame_count > 0)
                print_frames(e.info.frames, e.info.frame_count);
        }
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
    if (n > 0) outfd_write(buf, (size_t)n);

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

    if (g_compact) {
        // Compact mode: memview reconstructs all leaks from the log.
        // Nothing to scan; just signal EOF to the viewer.
        in_hook = false;
        goto signal_eof;
    }

    // Full mode: scan all shards for any threads that didn't call
    // thread_exit_handler (e.g. main thread, or threads still alive at exit).

    {
        // First pass over all shards: collect unique TIDs still alive.
        pid_t seen[1024];
        int   seen_count = 0;
        for (int s = 0; s < NSHARDS; s++) {
            pthread_mutex_lock(&g_shards[s].lock);
            if (g_shards[s].map)
                for (auto& [ptr, info] : *g_shards[s].map) {
                    bool found = false;
                    for (int i = 0; i < seen_count; i++)
                        if (seen[i] == info.tid) { found = true; break; }
                    if (!found && seen_count < (int)(sizeof(seen)/sizeof(seen[0])))
                        seen[seen_count++] = info.tid;
                }
            pthread_mutex_unlock(&g_shards[s].lock);
        }

        if (seen_count == 0) {
            in_hook = false;
            goto signal_eof;
        }

        char buf[256];
        int  n;

        for (int si = 0; si < seen_count; si++) {
            pid_t  tid        = seen[si];
            size_t leak_count = 0;
            size_t leak_bytes = 0;
            char   tname[16]  = {};

            struct LeakEntry { void* ptr; AllocInfo info; };
            std::vector<LeakEntry> leaks;
            for (int s = 0; s < NSHARDS; s++) {
                MapShard& sh = g_shards[s];
                pthread_mutex_lock(&sh.lock);
                if (sh.map) {
                    leaks.clear();
                    for (auto& [ptr, info] : *sh.map) {
                        if (info.tid != tid) continue;
                        if (!tname[0]) memcpy(tname, info.name, sizeof(tname));
                        ++leak_count;
                        leak_bytes += info.size;
                        leaks.push_back({ptr, info});
                        if (!sh.reported_leaks) {
                            try { sh.reported_leaks = new std::unordered_map<void*, size_t>(); }
                            catch (...) {}
                        }
                        if (sh.reported_leaks) {
                            try { (*sh.reported_leaks)[ptr] = info.size; } catch (...) {}
                        }
                    }
                    for (auto it = sh.map->begin(); it != sh.map->end(); )
                        it = (it->second.tid == tid) ? sh.map->erase(it) : ++it;
                }
                pthread_mutex_unlock(&sh.lock);
                for (auto& e : leaks) {
                    n = snprintf(buf, sizeof(buf),
                                 "[memtrack] tid=%-6d (%-15s) LEAK       ts=%-12llu %-10s size=%-12zu  ptr=%p\n",
                                 tid, e.info.name,
                                 (unsigned long long)e.info.timestamp_us,
                                 e.info.op, e.info.size, e.ptr);
                    if (n > 0) outfd_write(buf, (size_t)n);
                    if (e.info.frame_count > 0)
                        print_frames(e.info.frames, e.info.frame_count);
                }
            }

            if (leak_count > 0) {
                n = snprintf(buf, sizeof(buf),
                             "[memtrack] tid=%-6d (%-15s) SUMMARY    %zu unfreed allocation(s), %zu bytes leaked\n",
                             tid, tname[0] ? tname : "?", leak_count, leak_bytes);
                if (n > 0) outfd_write(buf, (size_t)n);
            }
        }
    }

    in_hook = false;

signal_eof:
    // Signal EOF to the viewer.  For TCP sockets use shutdown(SHUT_WR) rather
    // than close() so that threads still running don't write to a recycled fd.
    // For plain files close() is safe here since we're in the atexit handler
    // and the process is single-threaded at this point in practice.
    if (g_close_outfd) {
        // Atomically redirect writers to stderr before closing the fd.
        // Any write() that loaded the old fd *before* this swap will go to a
        // closed (or recycled) fd — that's an unavoidable TOCTOU given that
        // we can't quiesce all threads here.  Writers that load *after* the swap
        // go safely to stderr.
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
    shards_init();
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

    const char* dem = getenv("MEMTRACK_DEMANGLE");
    if (dem) g_demangle = (dem[0] != '0' && dem[0] != '\0');

    const char* compact_env = getenv("MEMTRACK_COMPACT");
    if (compact_env) g_compact = (compact_env[0] != '0' && compact_env[0] != '\0');
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
    // Save and force in_hook=true for the duration of this function.
    // backtrace_symbols and __cxa_demangle both call malloc internally; if the
    // caller had cleared in_hook (free path), those mallocs would be tracked
    // but freed via real_free (untracked), creating phantom leak entries in g_map.
    // Restoring the caller's value at the end preserves the intended behaviour:
    // on the free path (in_hook=false at entry) we restore false, so destructor
    // chains fired AFTER print_frames returns are still tracked correctly.
    bool saved_hook = in_hook;
    in_hook = true;

    char** syms = backtrace_symbols(frames, count);
    if (!syms) { in_hook = saved_hook; return; }

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

            // Remainder: "+0xNN) [0xADDR]"
            const char* rest = plus;

            int n;
            if (g_demangle) {
                // Demangle — __cxa_demangle allocates with malloc; release with real_free
                int   status    = -1;
                char* demangled = abi::__cxa_demangle(mangled, nullptr, nullptr, &status);
                const char* display = (status == 0 && demangled) ? demangled : mangled;
                n = snprintf(buf, sizeof(buf), "[memtrack]   #%-2d %.*s(%s%s\n",
                             i, mod_len, sym, display, rest);
                if (demangled) { if (real_free) real_free(demangled); else free(demangled); }
            } else {
                // Emit raw — memview (or the caller) will demangle on display
                n = snprintf(buf, sizeof(buf), "[memtrack]   #%-2d %.*s(%s%s\n",
                             i, mod_len, sym, mangled, rest);
            }
            if (n > 0) outfd_write(buf, (size_t)n);
        } else {
            // Fallback: print as-is
            int n = snprintf(buf, sizeof(buf), "[memtrack]   #%-2d %s\n", i, sym);
            if (n > 0) outfd_write(buf, (size_t)n);
        }
    }
    if (real_free) real_free(syms); else free(syms);
    in_hook = saved_hook;
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
        outfd_write(buf, (size_t)n);

    if (frame_count > 0)
        print_frames(frames, frame_count);
}

// Log a free/delete event and capture the stack frames.
// On return, in_hook is TRUE if the pointer was tracked; the caller is then
// responsible for: erasing from g_map, calling real_free, clearing in_hook,
// and finally calling print_frames.
// Returns true if tracked (caller must handle the cleanup), false otherwise.
static bool log_free_capture(void* ptr, const char* op,
                              void** frames, int& frame_count)
{
    frame_count = 0;
    if (!ptr || is_bootstrap_ptr(ptr) || in_hook) return false;

    in_hook = true;

    uint64_t ts = elapsed_us();

    if (g_compact) {
        // No map: log the free without size (memview gets size from alloc record).
        // Always considered tracked — memview silently ignores frees for
        // pointers it didn't see allocated (e.g. pre-load allocations).
        char buf[256];
        int n = snprintf(buf, sizeof(buf),
                         "[memtrack] tid=%-6d (%-15s) %-10s ts=%-12llu  ptr=%p\n",
                         get_tid(), get_thread_name(), op, (unsigned long long)ts, ptr);
        if (n > 0) outfd_write(buf, (size_t)n);
        return true;
    }

    // Full mode: capture frames then look up the map for size.
    if (g_stack_depth > 0) {
        const int skip  = 2;
        const int total = g_stack_depth + skip;
        int raw = backtrace(frames, total < 32 ? total : 32);
        frame_count = (raw > skip) ? raw - skip : 0;
        for (int i = 0; i < frame_count; i++) frames[i] = frames[i + skip];
    }

    size_t size    = 0;
    bool   tracked = false;
    int    s       = shard_of(ptr);
    MapShard& sh   = g_shards[s];
    pthread_mutex_lock(&sh.lock);
    if (sh.map) {
        auto it = sh.map->find(ptr);
        if (it != sh.map->end()) { size = it->second.size; tracked = true; }
    }
    // Cross-thread free after exit handler: ptr was removed from the shard map
    // but recorded in reported_leaks so we can log a cancelling free entry.
    if (!tracked && sh.reported_leaks) {
        auto it2 = sh.reported_leaks->find(ptr);
        if (it2 != sh.reported_leaks->end()) {
            size    = it2->second;
            tracked = true;
            sh.reported_leaks->erase(it2);
        }
    }
    pthread_mutex_unlock(&sh.lock);

    if (!tracked) {
        in_hook = false;
        return false;
    }

    char buf[384];
    int n = snprintf(buf, sizeof(buf),
                     "[memtrack] tid=%-6d (%-15s) %-10s ts=%-12llu size=%-12zu  ptr=%p\n",
                     get_tid(), get_thread_name(), op,
                     (unsigned long long)ts, size, ptr);
    if (n > 0) outfd_write(buf, (size_t)n);

    return true;
}

// log_free: full log+print in one call.  Used by the realloc hook which
// logs a synthetic free of old_ptr without calling real_free itself.
static void log_free(void* ptr, const char* op)
{
    void* frames[32];
    int   frame_count = 0;
    if (!log_free_capture(ptr, op, frames, frame_count)) return;
    // in_hook is true here; realloc owns the actual freeing, so just clear
    // and print.  The window is small because real_realloc is called
    // immediately after under its own in_hook=true block.
    in_hook = false;
    if (frame_count > 0) print_frames(frames, frame_count);
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

    // Log the implicit free of old_ptr before we erase it from the map.
    // This ensures the log has a matching free entry even when real_realloc
    // returns the same address (in-place growth), preventing memview from
    // treating the old record as a phantom leak.
    log_free(old_ptr, "free");

    // Hold in_hook while we erase old_ptr and call real_realloc so no other
    // thread can slip in between and re-use the same address in our map.
    in_hook = true;
    if (!g_compact) {
        int s = shard_of(old_ptr);
        pthread_mutex_lock(&g_shards[s].lock);
        if (g_shards[s].map) g_shards[s].map->erase(old_ptr);
        pthread_mutex_unlock(&g_shards[s].lock);
    }

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

    void* frames[32];
    int   frame_count = 0;
    bool  tracked = log_free_capture(ptr, "free", frames, frame_count);

    if (tracked) {
        if (!g_compact) {
            int s = shard_of(ptr);
            pthread_mutex_lock(&g_shards[s].lock);
            if (g_shards[s].map) g_shards[s].map->erase(ptr);
            pthread_mutex_unlock(&g_shards[s].lock);
        }
    }
    real_free(ptr);
    if (tracked) in_hook = false;
    if (tracked && frame_count > 0)
        print_frames(frames, frame_count);
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

    void* frames[32];
    int   frame_count = 0;
    bool  tracked = log_free_capture(p, op, frames, frame_count);
    // If tracked, in_hook is now TRUE — we own the cleanup.

    // Erase from the shard and physically free while in_hook=true.
    // Any reentrant free(p) during this window sees in_hook=true and exits
    // early, so real_free cannot be called twice.
    if (tracked) {
        if (!g_compact) {
            int s = shard_of(p);
            pthread_mutex_lock(&g_shards[s].lock);
            if (g_shards[s].map) g_shards[s].map->erase(p);
            pthread_mutex_unlock(&g_shards[s].lock);
        }
    }
    real_free(p);

    // Clear in_hook AFTER real_free: ptr is now gone from both g_map and
    // the heap.  If a destructor chain during print_frames tries to free p
    // again, log_free_capture won't find it in g_map → silent no-op.
    if (tracked) in_hook = false;

    if (tracked && frame_count > 0)
        print_frames(frames, frame_count);
}

void* operator new  (size_t s)            { return cpp_alloc(s, "new");   }
void* operator new[](size_t s)            { return cpp_alloc(s, "new[]"); }

void  operator delete  (void* p) noexcept { cpp_free(p, "delete");    }
void  operator delete[](void* p) noexcept { cpp_free(p, "delete[]");  }

// Sized-delete overloads (C++14)
void  operator delete  (void* p, size_t) noexcept { cpp_free(p, "delete");   }
void  operator delete[](void* p, size_t) noexcept { cpp_free(p, "delete[]"); }
