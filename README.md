# memtrack

An `LD_PRELOAD` library for tracking heap allocations per thread at runtime — no recompilation required.

The library is partly made with Claude Sonnet 4.6.

## Features

- Intercepts `malloc`, `calloc`, `realloc`, `free`, `operator new`, `operator delete` (including sized-delete C++14 overloads)
- Every allocation and free is logged with its **size**, **thread ID**, **thread name**, and **timestamp** (µs since program start)
- Per-thread **running total** of bytes allocated
- On thread exit: **cumulative total** and a **leak report** listing every pointer that was never freed
- Optional **stack traces** per allocation/free with automatic C++ symbol demangling
- Output to a **file** or stderr
- **Minimum size filter** to ignore small allocations
- Companion **ncurses TUI** (`memview`) for interactive browsing — including a **live follow mode** for monitoring running applications

## Build

```sh
make
```

Produces `memtrack.so` and `memview`.

## Usage

```sh
LD_PRELOAD=./memtrack.so ./your_app
```

All output goes to **stderr** by default so it does not interfere with stdout.

### Environment variables

| Variable               | Default       | Description |
|------------------------|---------------|-------------|
| `MEMTRACK_OUTPUT`      | _(stderr)_    | Write all output to this file instead of stderr. The file is created (or truncated) at startup. |
| `MEMTRACK_MIN_SIZE`    | `0`           | Suppress logging and leak tracking for allocations smaller than this many bytes. The per-thread `total` counter still counts every allocation. |
| `MEMTRACK_STACK_DEPTH` | `0`           | Number of call-stack frames to capture per allocation/free (0 = disabled). Compile the application with `-rdynamic` for resolved symbol names. C++ symbols are automatically demangled. |

## Output format

### Per allocation

```
[memtrack] tid=<tid> (<name>          ) <op>       ts=<µs>       size=<bytes>      total=<bytes>      ptr=<address>
```

### Per free

```
[memtrack] tid=<tid> (<name>          ) <op>       ts=<µs>       size=<bytes>      ptr=<address>
```

### Optional stack frame (printed immediately after the allocation/free line)

```
[memtrack]   #0  ./binary(MyClass::method()+0x2a) [0x...]
[memtrack]   #1  ./binary(main+0x28) [0x...]
```

### On thread exit

```
[memtrack] tid=<tid> (<name>          ) EXIT       ts=<µs>       total=<bytes>   bytes allocated
[memtrack] tid=<tid> (<name>          ) LEAK       ts=<alloc-µs> <op>       size=<bytes>   ptr=<address>
[memtrack] tid=<tid> (<name>          ) SUMMARY    <N> unfreed allocation(s), <bytes> bytes leaked
```

If all allocations were freed:

```
[memtrack] tid=<tid> (<name>          ) SUMMARY    all allocations freed
```

### Field reference

| Field  | Description |
|--------|-------------|
| `tid`  | Linux thread ID (`gettid`) |
| `name` | Thread name (up to 15 chars, set via `pthread_setname_np`) |
| `op`   | `malloc` / `calloc` / `realloc` / `new` / `new[]` / `free` / `delete` / `delete[]` |
| `ts`   | Microseconds since the library constructor ran (0 for pre-constructor allocations) |
| `size` | Bytes requested/freed |
| `total`| Cumulative bytes allocated by this thread |
| `ptr`  | Pointer address |

### Example

```
[memtrack] tid=12345  (main           ) malloc     ts=11           size=1024         total=1024          ptr=0x55a1b...
[memtrack] tid=12345  (main           ) realloc    ts=22           size=2048         total=3072          ptr=0x55a1b...
[memtrack] tid=12345  (main           ) free       ts=29           size=2048         ptr=0x55a1b...
[memtrack] tid=12346  (worker-1       ) malloc     ts=267          size=512          total=512           ptr=0x7f48b...
[memtrack]   #0  ./app(thread_fn()+0x91) [0x55a1b...]
[memtrack]   #1  /usr/lib/libc.so.6(__libc_start_main+0x89) [0x7f48a...]
[memtrack] tid=12346  (worker-1       ) EXIT       ts=290          total=512         bytes allocated
[memtrack] tid=12346  (worker-1       ) LEAK       ts=267          malloc     size=512          ptr=0x7f48b...
[memtrack] tid=12346  (worker-1       ) SUMMARY    1 unfreed allocation(s), 512 bytes leaked
```

---

## memview – interactive log viewer

`memview` is an ncurses TUI that reads a memtrack log and lets you browse allocations, inspect stack traces, filter, sort, and find leaks — or monitor a **live running application** with automatic refresh.

### Usage

```sh
# Capture to a log file, then open the viewer
MEMTRACK_OUTPUT=run.log MEMTRACK_STACK_DEPTH=8 LD_PRELOAD=./memtrack.so ./your_app
./memview run.log

# Live mode: start the viewer before or during a run — updates every 250 ms
MEMTRACK_OUTPUT=run.log LD_PRELOAD=./memtrack.so ./your_app &
./memview -f run.log

# Pipe directly (live via stdin)
MEMTRACK_OUTPUT=/dev/stdout LD_PRELOAD=./memtrack.so ./your_app | ./memview -f -
```

### Layout

```
┌─ ● LIVE  run.log  │  42 allocs  │  3 shown ────────────────────────────────────────────────────────────┐
│ Filter: Leaks    │  Thread: worker-1       │  Sort: Time ▼  │  Leaks: 1  (512 B)  │  Follow: ON [F]     │
├──────────────────────────────────────────────────────────────────┬────────────────────────────────────────┤
│ St. Pointer             Op        Time        Size      Thread   │ Detail [↑↓ scroll]                     │
│ [L] 0x7f4860000880      realloc   0.267ms     512 B     worker-1 │ ── Allocation ──────────────────────── │
│ [A] 0x55b29f0a8160      malloc    0.376ms     128 B     worker-2 │   Op      : realloc                    │
│                                                                  │   Ptr     : 0x7f4860000880              │
│                                                                  │   Size    : 512 B                       │
│                                                                  │   Total   : 640 B                       │
│                                                                  │   Time    : 0.267ms                     │
│                                                                  │   tid     : 12346                       │
│                                                                  │   Thread  : worker-1                    │
│                                                                  │                                         │
│                                                                  │   Stack:                                 │
│                                                                  │     #0  ./app(thread_fn()+0x91)          │
│                                                                  │     #1  /usr/lib/libc.so.6(...)          │
│                                                                  │                                         │
│                                                                  │ ── Free ───────────────────────────────  │
│                                                                  │   *** NOT FREED — MEMORY LEAK ***        │
├──────────────────────────────────────────────────────────────────┴────────────────────────────────────────┤
│ Threads: main[82 KB]  worker-1[640 B total, LEAK 512 B]  worker-2[839 B]                                  │
│ q:quit  f:filter  t:thread  s/S:sort(rev)  1/2/3:sort-by  Tab/h/l:pane  j/k:nav  ^f/^b:page  g/G:top/bot │
└────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
```

**Status column:** `[L]` = leak, `[A]` = active (not yet freed), `[F]` = freed

The **left pane is a fixed 72 columns** wide (exactly enough for all list columns) so the detail pane always receives the maximum available space.

The detail pane shows **Time** (when the allocation occurred) and, for freed allocations, **Lifetime** (how long the pointer was live).

### Keys

| Key | Action |
|-----|--------|
| `↑` `↓` / `k` `j` | Navigate allocation list (or scroll detail pane when focused) |
| `PgUp` `PgDn` / `Ctrl-b` `Ctrl-f` | Scroll by page |
| `Home` `End` / `g` `G` | Jump to top / bottom |
| `Tab` / `←` `→` / `h` `l` | Switch focus between list and detail pane |
| `f` | Cycle filter: **All → Leaks → Active → Freed** |
| `t` | Cycle thread filter |
| `s` | Cycle sort: **Time → Size → Thread** |
| `S` | Reverse current sort direction |
| `1` / `2` / `3` | Sort by Time / Size / Thread directly (press again to reverse) |
| `F` | Toggle auto-follow in live mode (re-enabled by pressing `G`) |
| `q` / `Esc` | Quit |

### Live mode (`-f`)

When started with `-f`, `memview` polls the log file every **250 ms** for new lines and updates the display incrementally without disturbing your scroll position. Auto-follow (`F`) keeps the list scrolled to the newest entry.

If the log file is **truncated** (application restarted), the display resets automatically.

Manual navigation (any movement key) **pauses** auto-follow. Pressing `G` jumps to the bottom and **re-enables** it.

The header shows `● LIVE` (green) when auto-follow is on, `◌ LIVE` (yellow/paused) when it is not.

---

## Test

A test application exercises allocations across multiple threads and deliberately leaks one allocation:

```sh
make test
```

---

## Implementation notes

- **Bootstrap allocator** — `dlsym()` itself calls `calloc` during startup before the real function pointers are resolved. A static 64 KB buffer handles those early allocations safely.

- **`in_hook` (thread-local bool)** — set to `true` for the entire duration of any hook or internal helper (`log_alloc`, `log_free`, `map_record`, `print_frames`, `thread_exit_handler`). This prevents recursive hook calls *and* ensures that internal allocations (map nodes, `backtrace_symbols` buffers, demangled strings) are never tracked. Critically, `log_free()` holds `in_hook = true` across the call to `print_frames()` so that `backtrace_symbols()`'s internal `malloc` is invisible to the tracker.

- **`map_remove` deadlock prevention** — `g_map->erase()` frees internal hash-map nodes via `operator delete`, which would re-enter the map lock. `map_remove` is a no-op when `in_hook == true`, breaking the cycle. All internal map operations happen while `in_hook` is already `true`.

- **`pthread_key` destructor** — `thread_exit_handler` is registered via `pthread_key_create`. It fires when any thread exits (including threads not created by your code), printing the EXIT/LEAK/SUMMARY lines and erasing that thread's entries from the global map.

- **Timestamps** — `clock_gettime(CLOCK_MONOTONIC)` is captured in `memtrack_ctor()` as the zero point. Each event records microseconds elapsed since that point. Allocations before the constructor fires (early runtime init) are recorded with `ts=0`.

- **Stack traces & demangling** — `backtrace()` is called skipping two internal frames (the hook + `log_alloc`/`log_free`) so `#0` is always user code. `backtrace_symbols()` output is parsed to extract the mangled symbol, which is passed to `abi::__cxa_demangle()`. The demangled buffer and the symbols array are both freed with `real_free()` to avoid tracking.

- **Memory overhead** — approximately **320 bytes per live tracked allocation** (one `std::unordered_map` node containing `AllocInfo`). Freed allocations are immediately removed from the map. Use `MEMTRACK_MIN_SIZE` to limit tracking to allocations you care about.

- **`write()` for output** — uses the `write(2)` syscall directly instead of `printf`/`fprintf` to avoid stdio buffering and re-entrant allocation inside the hook.

## Limitations

- `posix_memalign`, `aligned_alloc`, `memalign`, `valloc`, `strdup`, and `strndup` are not intercepted.
- The leak report only fires on *thread exit*. If the process is killed (SIGKILL), exit reports are not printed.
- Memory allocated by one thread and freed by another is handled correctly, but if the allocating thread exits first the pointer will appear as a leak in that thread's exit report.
- Not intended for production use; the per-allocation mutex lock affects throughput.
