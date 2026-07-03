# memtrack

An `LD_PRELOAD` library for tracking heap allocations per thread at runtime — no recompilation required.

The library is partly made with Claude Sonnet 4.6.

## Features

- Intercepts `malloc`, `calloc`, `realloc`, `free`, `strdup`, `strndup`, `mmap` (anonymous), `munmap`, `mremap`, `operator new`, `operator delete` (including sized-delete C++14 overloads)
- Every allocation and free is logged with its **size**, **thread ID**, **thread name**, and **timestamp** (µs since program start)
- Per-thread **running total** of bytes allocated
- On thread exit: **cumulative total** and a **leak report** listing every pointer that was never freed
- **Cross-thread free after exit** handled correctly — if thread B frees a pointer after the allocating thread has already exited and logged it as a leak, the free is logged and the leak entry is cancelled in memview
- Optional **stack traces** per allocation/free with automatic C++ symbol demangling
- Optional **per-thread stack trace filter** (`MEMTRACK_STACK_THREADS`) to capture frames only for threads of interest
- Output to a **file** or stderr
- **Minimum size filter** to ignore small allocations
- Companion **ncurses TUI** (`memview`) for interactive browsing

## Build

### Requirements

- CMake ≥ 3.16
- A C++17 compiler
- ncurses development headers

### Native build (host x86/x64)

```sh
cmake -B build
cmake --build build
```

Produces `build/memtrack.so`, `build/memview`, and `build/test_app`.

### Cross-compile for ARM (or any other target)

Set the `CROSS_TOOLCHAIN` environment variable to the toolchain prefix and pass the provided toolchain file:

```sh
CROSS_TOOLCHAIN=arm-linux-gnueabihf \
  cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-linux-toolchain.cmake
cmake --build build
```

`memtrack.so`, `memview`, and `test_app` are all built for the target.

Copy `memtrack.so` and `memview` to the target device and run as usual.

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
| `MEMTRACK_STACK_DEPTH`   | `0`           | Number of call-stack frames to capture per allocation/free (0 = disabled). Compile with `-rdynamic` for resolved symbol names. |
| `MEMTRACK_STACK_THREADS` | *(all)*       | Comma-separated list of thread-name substrings. When set, only threads whose name contains one of the substrings capture stack frames. Example: `MEMTRACK_STACK_THREADS=Remote,Worker`. Requires `MEMTRACK_STACK_DEPTH > 0`. |
| `MEMTRACK_BUFFER_SIZE`   | `4096`        | Per-thread output buffer size. Accepts a plain byte count or a `K`/`M` suffix, e.g. `64K` or `1M`. Set to `0` to disable buffering (one `write()` per event). |


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
```

Leak detection is handled entirely by `memview`, which reconstructs the allocation lifecycle from the log by matching `ptr=` fields.

### Field reference

| Field  | Description |
|--------|-------------|
| `tid`  | Linux thread ID (`gettid`) |
| `name` | Thread name (up to 15 chars, set via `pthread_setname_np`) |
| `op`   | `malloc` / `calloc` / `realloc` / `new` / `new[]` / `free` / `delete` / `delete[]` / `strdup` / `strndup` / `mmap` / `munmap` / `mremap` |
| `ts`   | Microseconds since the library constructor ran |
| `size` | Bytes requested/freed |
| `total`| Cumulative bytes allocated by this thread |
| `ptr`  | Pointer address |

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
│ ▁▁▂▃▅▇▇▆▃▂▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁ 1.234s              │
├──────────────────────────────────────────────────────────────┬────────────────────────────────────────────┤
│ St. Pointer             Op        Time        Size      Thread│ Detail [↑↓ scroll]                         │
│ [L] 0x7f4860000880      realloc   0.267ms     512 B     w-1  │ ── Allocation ──────────────────────────── │
│ [A] 0x55b29f0a8160      malloc    0.376ms     128 B     w-2  │   Op      : realloc                        │
│                                                              │   Ptr     : 0x7f4860000880                  │
│                                                              │   Size    : 512 B                           │
├──────────────────────────────────────────────────────────────┴────────────────────────────────────────────┤
│ Thread       TID    Allocated    Freed     Net(live)  Leaks                                                │
│ worker-1     12346  640 B        0 B       640 B      1 (512 B)                                           │
└────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
```

**Row colours in the list:**
- **Green** — live allocation, recently created (last 30% of session)
- **Yellow** — live allocation, middle-aged (30–75% of session)
- **Red/bold** — live allocation, old (oldest 75%+) — most likely a leak
- **Yellow/dim** — freed allocation
- **Red/bold** — confirmed leak (`[L]`)

**Status column:** `[L]` = leak, `[A]` = active (not freed), `[F]` = freed

### Timeline row

Row 3 of the header shows a **sparkline timeline** of live memory over the session:

- Each cell = a time bucket; height represents cumulative live bytes at that point
- **Green** cells = net allocating period; **yellow** = net freeing period
- When a **thread filter** is active, only that thread's allocations are shown (same time axis, labelled with the thread name)
- Persistent **markers** (magenta digits `1`–`9`) are shown at their placed timestamps
- Range brackets `[` `]` mark the active range diff window
- Delta baseline `|` shown in yellow when delta mode is active
- Press **`Z`** to hide/show the timeline row

### Keys

| Key | Action |
|-----|--------|
| `↑` `↓` / `k` `j` | Navigate allocation list |
| `PgUp` `PgDn` / `Ctrl-b` `Ctrl-f` | Scroll by page |
| `Home` `End` / `g` `G` | Jump to top / bottom |
| `Tab` / `→` / `l` | Cycle focus forward: **list → detail → thread summary → list** |
| `←` / `h` | Cycle focus backward |
| `f` | Cycle filter: **All → Leaks → Active → Freed** |
| `a` | Minimum lifetime filter — prompts for seconds (`0` = off). Shown in header as `Age≥N`. |
| `/` | Symbol search — case-insensitive substring across frames, thread name, op, pointer. Empty clears. |
| `m` | **Delta mode** — marks the selected record's timestamp; shows only what was allocated after. Header shows `▶ DELTA since T`. Press again to clear. |
| `M` | **Add marker** — drops a numbered marker (M1–M9) at the selected record's timestamp. Shown as magenta digit on timeline. |
| `D` | **Delete marker** — removes the marker nearest to the selected record. |
| `[` | **Range start** — sets the range filter start at the selected record's time (snaps to nearest marker). |
| `]` | **Range end** — sets the range filter end. When both ends are set, only allocations in that window are shown. Header shows `▶◀ RANGE`. |
| `R` | **Clear range** — removes range filter (markers remain). |
| `\` | Toggle **group mode** — folds records with identical stack traces. |
| `Enter` | Normal mode: jump to the group containing the selected record. Group mode: jump back to the example record. |
| `t` / `T` | Cycle thread filter forward / backward |
| `s` | Cycle sort column. List: **Time → Size → Thread**. Group: **Total → Count → Rate**. Threads: **Name → TID → Allocated → Freed → Net → Leaks**. |
| `S` | Reverse current sort direction |
| `1` / `2` / `3` | Sort by Time / Size / Thread directly |
| `H` | Toggle **size histogram** — replaces list pane with a bar chart of allocations by size bucket |
| `W` | **Export** — prompts for a filename and writes visible records (or groups) to a text file |
| `Z` | Toggle **timeline** row in header |
| `L` | Toggle addr2line source-line resolution in detail pane |
| `F` | Toggle auto-follow in live mode |
| `q` / `Esc` | Quit |

### Group mode (`\`)

Press `\` to fold all visible records with identical stack traces into one row. Each row shows:

- **▲ ×N** — monotonic growth indicator + allocation count. **`▲` in red** means live bytes for this call site *never decrease* — a strong leak signal.
- **Total bytes** — combined size of all allocations at this call site
- **Rate/s** — bytes per second (total ÷ time span of the group's allocations). Non-zero when idle = almost certainly leaking.
- **First frame** (demangled) + thread name

Sort with `s` cycles **Total → Count → Rate**. `S` reverses.

Press **`Enter`** on a group to jump back to individual records. Press `\` again to leave group mode.

### Size histogram (`H`)

Press `H` to replace the list pane with a bar chart showing the distribution of visible allocations across 10 size buckets (≤16 B → >1 MB). Each bar shows count and total bytes. The chart is filter-aware — it reflects the current filter, search, thread, and range.

### Markers and range diff

Markers are named timestamps (M1–M9) you pin to specific moments in the log. They appear as magenta digits on the timeline sparkline.

**Typical workflow:**

```
1. Navigate to a baseline record → press M  (drop M1)
2. Navigate to a later record    → press M  (drop M2)
3. Navigate near M1 → press [   (set range start, snaps to M1)
4. Navigate near M2 → press ]   (set range end,   snaps to M2)
5. List now shows only allocations in that window
6. Press \ → group by call site, sort by Rate or Total
7. Press f → Leaks filter to see what didn't get freed
```

### Leak analysis workflow

1. Run the application under `LD_PRELOAD=./memtrack.so` with `MEMTRACK_OUTPUT=leak.log`
2. Open `memview leak.log` (or use live mode `-f` / TCP)
3. Press **`f`** → **Leaks** filter
4. Press **`\`** → group mode — identical call sites collapse, sorted by total bytes leaked
5. Look for rows with **`▲`** (monotonic growth) — these are the highest-priority suspects
6. Navigate to a group and check the **Rate/s** column — non-zero means bytes keep accumulating
7. Press **`Tab`** to move to the detail pane and inspect the full stack trace
8. Use **`/`** to search for a specific function, or **`a`** to hide short-lived allocations
9. Use markers (`M`) and range (`[`/`]`) to isolate what a specific operation leaks
10. Press **`W`** to export findings to a text file for sharing or diffing

---

## Test suite

A test application (`test_app`) exercises 24 different scenarios across multiple threads:

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
| T09  | Five-step realloc chain |
| T10  | `free(NULL)` — must be silent |
| T11  | `malloc(0)` — handles either NULL or non-NULL |
| T12  | 10 MB large allocation |
| T13  | 200× `malloc(13)`, all freed |
| T14  | Intentional leak — verified NOT freed |
| T15  | Cross-thread free while allocating thread is still alive |
| T16  | LEAK cancellation — thread exits; main frees afterwards |
| T17  | 4 worker threads, all alloc types, all freed |
| T18  | C++14 sized `::operator delete(ptr, n)` |
| T19  | Failed `realloc` safety — original pointer remains valid |
| T20  | 20-level deep call stack |
| T21  | 60-level template recursion — long symbol names |
| T22  | `MEMTRACK_STACK_THREADS` filter |
| T23  | `strdup` / `strndup` — own op names, freed cleanly |
| T24  | Anonymous `mmap` / `munmap` — tracked and balanced |

Run the suite:

```sh
cmake --build build
ctest --test-dir build
```

This runs the app under `LD_PRELOAD`, captures the log to `mt.log`, and runs `verify.sh` which checks **53 assertions** and reports per-test PASS/FAIL.

> **Note:** tests require a native build — ARM (cross-compiled) binaries cannot run on the host.

---

## Overhead

memtrack is a debugging tool; its overhead is significant and it is **not intended for production use**.

| Source | Cost |
|--------|------|
| `pthread_mutex_lock/unlock` | **High** — per-thread lock on allocation path; free path is lock-free |
| `write()` syscall | **High** — one per allocation and one per free |
| `clock_gettime(CLOCK_MONOTONIC)` | Medium — one call per event |
| `backtrace()` | **Very high** if enabled — 10–100 µs per call; disabled by default |

**Typical slowdown** (stack traces off): **1.5–3×**. With `MEMTRACK_STACK_DEPTH=5`: **50–100×**.

**Zero per-allocation heap overhead** — no in-memory map is kept. memtrack writes each event directly to the log and forgets it.

---

## Implementation notes

- **Bootstrap allocator** — `dlsym()` itself calls `calloc` during startup. A static 64 KB buffer handles those early allocations safely.
- **`in_hook` (thread-local bool)** — prevents recursive hook calls and ensures internal allocations (e.g. from `backtrace_symbols`) are never tracked.
- **Lock-free free path** — the alloc path takes a per-thread mutex (to serialise multi-line alloc+frames output). The free/munmap path is entirely lock-free.
- **`mmap` tracking** — only `MAP_ANONYMOUS` mappings are tracked; file-backed mmaps are skipped to avoid noise. `mremap` is logged as free(old) + alloc(new) to keep the tracker balanced.
- **`pthread_key` destructor** — `thread_exit_handler` is registered via `pthread_key_create`. On thread exit it emits an EXIT line; memview marks still-unfreed records as leaked.
- **Cross-thread LEAK cancellation** — handled in memview by log line numbers. When a free appears after an EXIT for the same pointer, memview clears the `is_leak` flag.

## Limitations

- `posix_memalign`, `aligned_alloc`, `memalign`, `valloc`, and `reallocarray` are not intercepted.
- The leak report only fires on *thread exit*. If the process is killed with `SIGKILL`, exit reports are not printed; memview handles this by marking all still-unfreed allocations as leaked at EOF.
- Not intended for production use — see [Overhead](#overhead) above.
