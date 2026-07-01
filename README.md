# memtrack

An `LD_PRELOAD` library for tracking heap allocations per thread at runtime — no recompilation required.

The library is partly made with Claude Sonnet 4.6.

## Features

- Intercepts `malloc`, `calloc`, `realloc`, `free`, `operator new`, `operator delete` (including sized-delete C++14 overloads)
- Every allocation and free is logged with its **size**, **thread ID**, **thread name**, and **timestamp** (µs since program start)
- Per-thread **running total** of bytes allocated
- On thread exit: **cumulative total** and a **leak report** listing every pointer that was never freed
- **Cross-thread free after exit** handled correctly — if thread B frees a pointer after the allocating thread has already exited and logged it as a leak, the free is logged and the leak entry is cancelled in memview
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
| `MEMTRACK_PORT`        | _(none)_      | Start a TCP server on this port. The application **pauses at startup** until a client connects (e.g., `memview :PORT`). Output goes to the connected client instead of a file. Takes priority over `MEMTRACK_OUTPUT`. |
| `MEMTRACK_OUTPUT`      | _(stderr)_    | Write all output to this file instead of stderr. Created/truncated at startup. |
| `MEMTRACK_MIN_SIZE`    | `0`           | Suppress logging for allocations smaller than this many bytes. |
| `MEMTRACK_STACK_DEPTH` | `0`           | Number of call-stack frames to capture per allocation/free (0 = disabled). Compile with `-rdynamic` for resolved symbol names. |
| `MEMTRACK_DEMANGLE`    | `0`           | Set to `1` to demangle C++ symbols in memtrack's log output. By default symbols are left mangled and `memview` demangled them on display. |
| `MEMTRACK_COMPACT`     | `1`           | When `1` (default), memtrack keeps **no in-memory map** of live allocations. Free hooks log `ptr=` only (no `size=`), and no LEAK/SUMMARY lines are emitted at thread exit — memview detects leaks by matching alloc/free ptrs in the log. This eliminates all lock contention on the free path and removes ~300–400 bytes per live allocation. Set to `0` for standalone log analysis without memview (LEAK lines will include op, timestamp, and reprinted stack frames). |

### TCP server mode

```sh
# Terminal 1 — start the target; it blocks until memview connects
MEMTRACK_PORT=4242 MEMTRACK_STACK_DEPTH=8 LD_PRELOAD=./memtrack.so ./your_app

# Terminal 2 — open the viewer; connects to the server and starts the app
./memview :4242
# or with an explicit host:
./memview myhost:4242
```

The connection is closed automatically when the application exits, at which point memview shows `✕ DONE` in the header and continues displaying the collected data.

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
[memtrack] tid=12345  (main           ) free       ts=310          size=512          ptr=0x7f48b...
```

The last line above shows how a cross-thread free is logged after the allocating thread has exited. memview will cancel the corresponding LEAK entry when it encounters this free.

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

# TCP mode: connect directly to a running process (saves stream to file too)
./memview :4242 -o run.log
./memview myhost:4242 -o run.log
```

**`-o` / `--save <file>`** — when viewing a TCP stream, tee every received line to a file.
The saved file is a valid memtrack log that can be replayed later with `./memview run.log`.

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
│ Thread              TID      Allocated       Freed    Net(live)  Leaks           │                         │
│ main                12345    1.2 MB          1.1 MB   82.0 KB    -               │                         │
│ worker-1            12346    640 B           0 B      640 B      1 (512 B)       │                         │
│ worker-2            12347    839 B           839 B    0 B        -               │                         │
├──────────────────────────────────────────────────────────────────────────────────┴─────────────────────────┤
│ Top functions  1-5/12 ─ ...                                                                                 │
│  Function                        Allocated      Freed       Net (live)                                      │
│  thread_fn(void*)                512 B          0 B         512 B                                           │
│ q:quit  f:filter  t:thread  s/S:sort  1/2/3:sort-by  Tab/h/l:pane  j/k:nav  ^f/^b:page  g/G:top/bot       │
└────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
```

**Status column:** `[L]` = leak, `[A]` = active (not yet freed), `[F]` = freed

The **left pane is a fixed 72 columns** wide (exactly enough for all list columns) so the detail pane always receives the maximum available space.

The detail pane shows **Time** (when the allocation occurred) and, for freed allocations, **Lifetime** (how long the pointer was live).

### Threads pane

The threads pane (below the main list) shows one row per thread with these columns:

| Column      | Description |
|-------------|-------------|
| `Thread`    | Thread name (up to 15 chars) |
| `TID`       | Linux thread ID |
| `Allocated` | Total bytes ever allocated by this thread |
| `Freed`     | Total bytes freed (including cross-thread frees) |
| `Net(live)` | `Allocated − Freed` — bytes currently live |
| `Leaks`     | Number and total size of allocations logged as LEAK at exit (cancelled when later freed by another thread) |

Rows are sorted by `Net(live)` descending. The currently selected thread (active filter) is highlighted bold. Press `t` to cycle the thread filter, which affects both the main list and the hot-functions pane.

### Keys

| Key | Action |
|-----|--------|
| `↑` `↓` / `k` `j` | Navigate allocation list (or scroll active pane when focused) |
| `PgUp` `PgDn` / `Ctrl-b` `Ctrl-f` | Scroll by page |
| `Home` `End` / `g` `G` | Jump to top / bottom |
| `Tab` / `→` / `l` | Cycle focus forward: **list → detail → hot-fn → list** |
| `←` / `h` | Cycle focus backward |
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

### Hot functions pane

At the bottom of the screen, a **7-row pane** (2 header rows + 5 data rows) lists **all unique call sites ranked by net unfreed bytes** — bytes allocated minus bytes freed for each caller. The 5 visible rows scroll independently from the rest of the UI.

```
┌─ Top functions  3-7/12 ────────────────────────────────────────────────────────────────────────────────────┐
│  Function                                               Allocated      Freed        Net (live)              │
│  main                                                   3,072 B        2,048 B      1,024 B                 │
│  thread_fn(void*)                                       512 B          0 B          512 B                   │
│  malloc                                                 256 B          256 B        0 B                     │
│  calloc                                                 128 B          128 B        0 B                     │
│  _IO_file_doallocate                                    4,096 B        4,096 B      0 B                     │
└────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
```

- Press **Tab** (or `l` / `→`) to focus the hot-functions pane; its title changes to show navigation hints.  When focused, use `j`/`k`, `Ctrl-f`/`Ctrl-b`, `g`/`G` to scroll.
- A **scroll indicator** (`N-M/total`) in the title shows the visible range.
- Function names are extracted from `frame #0` of each allocation's stack trace (the most immediate caller of `malloc`/`new`/etc.).
- When no stack trace is available (e.g., `MEMTRACK_STACK_DEPTH=0`), the grouping key falls back to the raw operation name (`malloc`, `calloc`, …).
- The pane respects the current **thread filter** (`t` key).
- The top offender (net > 0) is highlighted **bold red**; others with unfreed bytes are **red**; fully freed functions are **green**.
- For functions to appear by name, the binary must be compiled with `-rdynamic` and the relevant functions must not be `static` (static symbols are invisible to `backtrace_symbols()`). Inlined functions will appear under the caller's name; use `__attribute__((noinline))` on hot functions to get accurate attribution.

---

## Test suite

A comprehensive test application (`test_app`) exercises 19 different scenarios across multiple threads:

| Test | Scenario |
|------|----------|
| T01  | `malloc` / `free` |
| T02  | `calloc` / `free` (verifies zeroing) |
| T03  | `new` / `delete` |
| T04  | `new[]` / `delete[]` |
| T05  | `realloc(NULL, n)` acts as `malloc` |
| T06  | `realloc` grow (may move pointer) |
| T07  | `realloc` shrink (likely in-place) |
| T08  | `realloc(ptr, 0)` acts as `free` |
| T09  | Five-step realloc chain (100 → 200 → 400 → 800 → final size) |
| T10  | `free(NULL)` — must be silent, no log entry |
| T11  | `malloc(0)` — glibc returns non-NULL; handles either behaviour |
| T12  | 10 MB large allocation |
| T13  | 200× `malloc(13)`, all freed |
| T14  | Intentional leak — verified present in SUMMARY and absent from free log |
| T15  | Cross-thread free while allocating thread is still alive |
| T16  | LEAK cancellation — thread exits with LEAK logged; main frees it afterwards |
| T17  | 4 worker threads each exercising all alloc types; all freed |
| T18  | C++14 sized `::operator delete(ptr, n)` |
| T19  | Failed `realloc` safety — original pointer remains valid and is freed |

Each test uses a unique size encoding (`T×1000+N`) so any allocation can be located by a simple `grep` in the log.

Run the suite and verify automatically:

```sh
make test
```

This builds `memtrack.so` and `test_app`, runs the app under `LD_PRELOAD`, captures the log to `mt.log`, and runs `verify.sh` which checks **40 assertions** and reports per-test PASS/FAIL.

You can also run the verification manually:

```sh
LD_PRELOAD=./memtrack.so ./test_app 2>mt.log
./verify.sh mt.log
```

---

## Overhead

memtrack is a debugging tool; its overhead is significant and it is **not intended for production use**.

### Per-operation CPU cost

| Source | Cost | Notes |
|--------|------|-------|
| `pthread_mutex_lock/unlock` | **High** | Global lock — all threads contend on every malloc/free |
| `write()` syscall | **High** | One per allocation and one per free to write the log line |
| `clock_gettime(CLOCK_MONOTONIC)` | Medium | One syscall per event for the timestamp |
| `syscall(SYS_gettid)` | Medium | Called on every allocation (not cached) |
| `unordered_map` insert/find/erase | Medium | O(1) amortised; only applies in full mode (`MEMTRACK_COMPACT=0`) |
| `backtrace()` | **Very high** (if enabled) | 10–100 µs per call depending on stack depth; disabled by default |
| `backtrace_symbols()` + `__cxa_demangle` | **Very high** (if enabled) | Demangling is especially expensive; only called when printing |

**Typical slowdown** (stack traces off):

| Mode | Slowdown | Bottleneck |
|------|----------|------------|
| Compact (default) | **1.5–3×** | Two `write()` syscalls + two `clock_gettime` calls per alloc/free pair |
| Full (`MEMTRACK_COMPACT=0`) | **3–10×** | Mutex contention + map insert/erase + same syscalls |

With `MEMTRACK_STACK_DEPTH=5` the slowdown can reach **50–100×** in either mode.

In compact mode, the **free hook is completely lock-free** — it does one `clock_gettime`, one snprintf into a stack buffer, and one `write()`. All mutex contention is eliminated.

### Memory overhead

In **compact mode** (default): **zero per-allocation heap overhead** — no map is kept. memtrack writes each event directly to the log file and forgets it. Resident memory cost is limited to the file write buffer (~4 KB, kernel-managed) plus a few hundred bytes of per-thread state.

In **full mode** (`MEMTRACK_COMPACT=0`): each live tracked allocation occupies one `std::unordered_map` node containing an `AllocInfo` struct:

| Field | Size |
|-------|------|
| `tid` + `name` + `op` | 32 bytes |
| `size` + `timestamp_us` | 16 bytes |
| `frame_count` + `frames[32]` | 260 bytes (always reserved, even when stack traces are off) |
| `unordered_map` node overhead | ~50–80 bytes |
| **Total per live allocation** | **~360–400 bytes** |

Freed allocations are removed immediately, so only *live* allocations incur this cost. Use `MEMTRACK_MIN_SIZE` to limit tracking to the allocations you care about.

---

## Implementation notes

- **Bootstrap allocator** — `dlsym()` itself calls `calloc` during startup before the real function pointers are resolved. A static 64 KB buffer handles those early allocations safely.

- **`in_hook` (thread-local bool)** — set to `true` for the entire duration of any hook or internal helper (`log_alloc`, `log_free_capture`, `print_frames`, `thread_exit_handler`). This prevents recursive hook calls *and* ensures that internal allocations (demangled strings in full mode) are never tracked.

- **Compact mode lock-freedom** — in compact mode (`MEMTRACK_COMPACT=1`, the default), no in-memory map is maintained. Free hooks log the pointer and return immediately, without acquiring any mutex. Leak detection is delegated entirely to memview, which reconstructs the alloc/free lifecycle from the log by matching `ptr=` fields with line-number awareness to handle address reuse.

- **Sharded map (full mode)** — the map is split into 16 independently-locked shards (`g_shards[16]`, each `alignas(64)` to avoid false sharing). The shard for a pointer is selected by `(uintptr_t(ptr) >> 4) & 15`. This reduces lock contention by up to 16× compared to a single global lock, at the cost of 16 small mutex initialisation calls at startup.

- **`pthread_key` destructor** — `thread_exit_handler` is registered via `pthread_key_create`. In compact mode it emits only an EXIT line and returns. In full mode it also scans the thread's shard entries for leaked allocations and emits LEAK/SUMMARY lines.

- **Cross-thread LEAK cancellation** — in full mode, when `thread_exit_handler` logs a LEAK for a pointer, it also records it in `sh.reported_leaks`. If another thread later frees that pointer, the free hook finds it there, logs the free, and removes the entry. memview recognises this pattern (free line appears after EXIT) and clears `is_leak`. In compact mode, memview handles this entirely by comparing line numbers of EXIT and free events.

- **Timestamps** — `clock_gettime(CLOCK_MONOTONIC)` is captured in `memtrack_ctor()` as the zero point. Each event records microseconds elapsed since that point. Allocations before the constructor fires (early runtime init) are recorded with `ts=0`.

- **Stack traces & demangling** — `backtrace()` is called skipping two internal frames (the hook + `log_alloc`/`log_free`) so `#0` is always user code. By default memtrack emits raw mangled names; memview demandles on display using `abi::__cxa_demangle`. Set `MEMTRACK_DEMANGLE=1` to demangle in the log instead.

- **`write()` for output** — uses the `write(2)` syscall directly instead of `printf`/`fprintf` to avoid stdio buffering and re-entrant allocation inside the hook.

- **GCC dead-allocation elimination** — at `-O2`, GCC may prove that an allocation result escapes only into `free()` and eliminate the alloc/free pair entirely ("as-if" rule + `__attribute__((malloc))`). `test_app` is compiled at `-O0` to prevent this.

## Limitations

- `posix_memalign`, `aligned_alloc`, `memalign`, `valloc`, `strdup`, and `strndup` are not intercepted.
- The leak report only fires on *thread exit* (via `pthread_key` destructor). If the process is killed with `SIGKILL`, exit reports are not printed. memview handles this by marking all still-unfreed allocations as leaked at EOF.
- Not intended for production use — see [Overhead](#overhead) above.
