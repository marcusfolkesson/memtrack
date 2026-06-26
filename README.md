# memtrack

An `LD_PRELOAD` library for tracking heap allocations per thread at runtime — no recompilation required.

The library is partly made with Claude Sonnet 4.6.

## Features

- Intercepts `malloc`, `calloc`, `realloc`, `free`, `operator new`, and `operator delete`
- Prints each allocation with its **size**, **thread ID**, and **running total** for that thread
- On thread exit, prints the **cumulative bytes allocated** and a **leak report** listing every pointer that was never freed

## Build

```sh
make
```

This produces `memtrack.so`.

## Usage

```sh
LD_PRELOAD=./memtrack.so ./your_app
```

All output goes to **stderr** so it does not interfere with your application's stdout.

### Environment variables

| Variable              | Default | Description |
|-----------------------|---------|-------------|
| `MEMTRACK_MIN_SIZE`   | `0`     | Suppress logging and leak tracking for allocations smaller than this many bytes. The per-thread `total` counter is unaffected and always reflects all allocations. |
| `MEMTRACK_OUTPUT`     | _(stderr)_ | Write all output to this file instead of stderr. The file is created (or truncated) at startup. If the file cannot be opened, a warning is printed to stderr and output falls back to stderr. |
| `MEMTRACK_STACK_DEPTH`| `0`     | Number of call stack frames to capture and print per allocation (0 = disabled). Frames are also stored and replayed in LEAK reports. Compile your application with `-rdynamic` for resolved symbol names. |

## Output format

### Per allocation

```
[memtrack] tid=<tid> (<name>          ) <op>       size=<bytes>      total=<bytes>      ptr=<address>
```

| Field   | Description                                      |
|---------|--------------------------------------------------|
| `tid`   | Linux thread ID (`gettid`)                       |
| `name`  | Thread name (set via `pthread_setname_np`; up to 15 chars) |
| `op`    | Operation: `malloc`, `calloc`, `realloc`, `new`, `new[]` |
| `size`  | Bytes requested in this call                     |
| `total` | Cumulative bytes allocated by this thread so far |
| `ptr`   | Returned pointer                                 |

### On thread exit

```
[memtrack] tid=<tid> (<name>          ) EXIT       total=<bytes>   bytes allocated
[memtrack] tid=<tid> (<name>          ) LEAK       <op>       size=<bytes>   ptr=<address>
[memtrack] tid=<tid> (<name>          ) SUMMARY    <N> unfreed allocation(s), <bytes> bytes leaked
```

If all allocations were freed:

```
[memtrack] tid=<tid> (<name>          ) SUMMARY    all allocations freed
```

### Example

```
[memtrack] tid=12345  (main           ) malloc     size=1024         total=1024          ptr=0x...
[memtrack] tid=12345  (main           ) realloc    size=2048         total=3072          ptr=0x...
[memtrack] tid=12346  (worker-1       ) malloc     size=128          total=128           ptr=0x...
[memtrack] tid=12346  (worker-1       ) EXIT       total=128         bytes allocated
[memtrack] tid=12346  (worker-1       ) LEAK       malloc     size=128          ptr=0x...
[memtrack] tid=12346  (worker-1       ) SUMMARY    1 unfreed allocation(s), 128 bytes leaked
```

## memview – interactive log viewer

`memview` is an ncurses TUI that reads a memtrack log and lets you interactively browse allocations, inspect stack traces, and find leaks.

### Build

```sh
make          # builds both memtrack.so and memview
```

### Usage

```sh
# Capture a run to a log file
MEMTRACK_OUTPUT=run.log MEMTRACK_STACK_DEPTH=8 LD_PRELOAD=./memtrack.so ./your_app

# Open the viewer
./memview run.log
```

Or pipe directly:

```sh
LD_PRELOAD=./memtrack.so ./your_app 2>&1 | tee run.log | ./memview -
```

### Layout

```
┌─ memtrack viewer  run.log  │  42 allocs  │  3 shown ─────────────────┐
│ Filter: Leaks    │  Thread: worker-1  │  Leaks: 3  (1.5 KB)          │
├────────────────────────────┬───────────────────────────────────────────┤
│ St. Pointer          Op    │ Detail [↑↓ scroll]                        │
│ [L] 0x7f4860000880  reall… │ ── Allocation ─────────────────────────── │
│ [A] 0x55b29f0a8160  malloc │   Op     : realloc                        │
│                            │   Ptr    : 0x7f4860000880                 │
│                            │   Size   : 512 B                          │
│                            │   tid    : 602060                         │
│                            │   Thread : worker-1                       │
│                            │                                           │
│                            │   Stack:                                  │
│                            │     #0  ./app(thread_fn+0x91)             │
│                            │     #1  /usr/lib/libc.so.6                │
│                            │                                           │
│                            │ ── Free ───────────────────────────────── │
│                            │   *** NOT FREED — MEMORY LEAK ***         │
├────────────────────────────┴───────────────────────────────────────────┤
│ Threads: main[82 KB]  worker-1[640 B total, LEAK 512 B]  worker-2[…]  │
│ q:quit  f:filter  t:thread  Tab/←→:pane  ↑↓:nav  PgUp/Dn  Home/End  │
└────────────────────────────────────────────────────────────────────────┘
```

**Status column:** `[L]` = leak, `[A]` = active (unfreed), `[F]` = freed

### Keys

| Key | Action |
|-----|--------|
| `↑` `↓` | Navigate allocation list (or scroll detail pane when focused) |
| `PgUp` `PgDn` `Home` `End` | Scroll list or detail |
| `Tab` / `←` `→` | Switch focus between list and detail pane |
| `f` | Cycle filter: **All → Leaks → Active → Freed** |
| `t` | Cycle thread filter |
| `q` / `Esc` | Quit |



A test application is included that exercises allocations across multiple threads and deliberately leaks one allocation in thread 1:

```sh
make test
```

## Implementation notes

- **Bootstrap allocator** — `dlsym()` itself calls `calloc` during startup before the real function pointers are resolved. A static 64 KB buffer handles those early allocations safely.
- **`in_hook` flag (thread-local)** — prevents recursive hook calls and signals that a deallocation is internal (e.g. the tracking map freeing its own nodes). `map_remove` is a no-op when `in_hook` is `true`, which also prevents a deadlock that would otherwise occur when the map's internal nodes are freed while the map lock is held.
- **`pthread_key` destructor** — `thread_exit_handler` is registered via `pthread_key_create` and fires automatically when each thread exits, even threads that were not created by your code.
- **Global allocation map** — a `std::unordered_map<void*, AllocInfo>` protected by a mutex tracks all live user allocations across threads. The map's own nodes are allocated while `in_hook == true` and are therefore never tracked themselves.
- **`write()` for output** — uses `write(2)` directly instead of `printf`/`fprintf` to avoid stdio buffering and potential recursive allocations.

## Limitations

- `posix_memalign`, `aligned_alloc`, `memalign`, and `valloc` are not intercepted.
- Allocations made before the library constructor runs (very early runtime init) are not tracked.
- The leak report only covers memory allocated by the exiting thread. Memory allocated by one thread and freed by another is handled correctly during normal operation, but if the allocating thread exits before the freeing thread, it will appear as a leak in the exit report.
- Not intended for production use; the added locking overhead affects performance.
