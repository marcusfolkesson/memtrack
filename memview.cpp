/*
 * memview.cpp – ncurses viewer for memtrack log output
 *
 * Build:  g++ -O2 -std=c++17 -o memview memview.cpp -lncurses
 * Usage:  ./memview <log>
 *         ./memview -              (read from stdin)
 *         ./memview -f <log>       (live follow mode)
 *         ./memview :4242          (connect to memtrack TCP server on localhost)
 *         ./memview host:4242      (connect to memtrack TCP server on host)
 *
 * Keys:
 *   ↑ ↓  PgUp PgDn  Home End   Navigate allocation list
 *   Tab / ← →                  Switch focus: list ↔ detail ↔ thread summary pane
 *   f                           Cycle filter: All → Leaks → Active → Freed
 *   t / T                       Cycle thread filter forward / backward (Thread Summary order)
 *   F                           Toggle auto-follow (live mode)
 *   q / Esc                     Quit
 */

#include <ncurses.h>
#include <locale.h>
#include <vector>
#include <string>
#include <unordered_map>
#include <algorithm>
#include <fstream>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cinttypes>
#include <climits>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <cxxabi.h>

using std::vector;
using std::string;
using std::unordered_map;
using std::pair;
using std::min;
using std::max;

static constexpr int ctrl(char c) { return c & 0x1f; }

// ─── Data model ──────────────────────────────────────────────────────────────

struct AllocRecord {
    uintptr_t ptr         = 0;
    int       tid         = 0;
    string    thread_name;
    string    op;               // malloc / calloc / realloc / new / new[]
    size_t    size        = 0;
    size_t    total       = 0;
    uint64_t  timestamp_us = 0; // µs since program start
    vector<string> frames;

    bool     freed             = false;
    int      free_tid          = 0;
    string   free_thread_name;
    string   free_op;             // free / delete / delete[]
    uint64_t free_timestamp_us = 0;
    vector<string> free_frames;

    bool   is_leak        = false;  // flagged in a LEAK line at thread exit
};

struct ThreadInfo {
    int    tid            = 0;
    string name;
    size_t allocated      = 0;   // total bytes ever allocated by this thread
    size_t freed          = 0;   // total bytes freed by this thread (from its own allocs)
    size_t total_bytes    = 0;   // running total counter (from EXIT line)
    size_t leak_count     = 0;
    size_t leak_bytes     = 0;
    size_t net() const { return allocated >= freed ? allocated - freed : 0; }
};

// ─── Parser ──────────────────────────────────────────────────────────────────

struct ParseState {
    vector<AllocRecord>          records;
    unordered_map<uintptr_t, size_t> ptr_idx;   // live ptr → records index

    vector<ThreadInfo>           threads;
    unordered_map<int, size_t>   tid_idx;        // tid → threads index

    enum class Last { NONE, ALLOC, FREE } last = Last::NONE;
    size_t last_rec = 0;

    ThreadInfo& thread(int tid, const string& name) {
        auto it = tid_idx.find(tid);
        if (it != tid_idx.end()) return threads[it->second];
        threads.push_back({tid, name, 0, 0, 0});
        tid_idx[tid] = threads.size() - 1;
        return threads.back();
    }
};

static string rtrim(string s)
{
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

static bool is_alloc_op(const char* op)
{
    return !strcmp(op,"malloc") || !strcmp(op,"calloc") ||
           !strcmp(op,"realloc") || !strcmp(op,"new") || !strcmp(op,"new[]") ||
           !strcmp(op,"strdup") || !strcmp(op,"strndup") ||
           !strcmp(op,"mmap") || !strcmp(op,"mremap");
}

static bool is_free_op(const char* op)
{
    return !strcmp(op,"free") || !strcmp(op,"delete") || !strcmp(op,"delete[]") ||
           !strcmp(op,"munmap");
}

static void parse_line(const char* line, ParseState& st)
{
    // Stack frame:  "[memtrack]   #N  <symbol>"
    if (strncmp(line, "[memtrack]   #", 14) == 0) {
        // find symbol after the "#N  " part
        const char* p = line + 14;
        while (*p && *p != ' ') p++;  // skip number
        while (*p == ' ') p++;
        string sym = rtrim(p);
        if (st.last == ParseState::Last::ALLOC && st.last_rec < st.records.size())
            st.records[st.last_rec].frames.push_back(sym);
        else if (st.last == ParseState::Last::FREE && st.last_rec < st.records.size())
            st.records[st.last_rec].free_frames.push_back(sym);
        return;
    }

    if (strncmp(line, "[memtrack] tid=", 15) != 0) return;

    int  tid = 0;
    char name[64] = {};
    if (sscanf(line, "[memtrack] tid=%d (%63[^)])", &tid, name) != 2) return;
    string tname = rtrim(name);

    const char* after = strstr(line, ") ");
    if (!after) return;
    after += 2;

    char op[32] = {};
    if (sscanf(after, "%31s", op) != 1) return;

    st.last = ParseState::Last::NONE;

    // ── EXIT ──────────────────────────────────────────────────────────────
    if (!strcmp(op, "EXIT")) {
        size_t total = 0;
        sscanf(after, "EXIT total=%zu", &total);
        auto& th = st.thread(tid, tname);
        th.total_bytes = total;
        // memtrack emits no LEAK/SUMMARY lines — memview detects leaks by
        // marking every still-unfreed record for this thread at exit time.
        for (auto& r : st.records) {
            if (r.tid == tid && !r.freed && !r.is_leak) {
                r.is_leak = true;
                th.leak_count++;
                th.leak_bytes += r.size;
            }
        }
        return;
    }

    // ── Allocation ────────────────────────────────────────────────────────
    if (is_alloc_op(op)) {
        size_t sz = 0, total = 0;
        void*  ptr_raw = nullptr;
        unsigned long long ts_ull = 0;
        sscanf(after, "%*s ts=%llu size=%zu total=%zu ptr=%p", &ts_ull, &sz, &total, &ptr_raw);
        uintptr_t ptr = (uintptr_t)ptr_raw;

        // ptr=0 means sscanf failed to parse the pointer field — the line was
        // truncated or two events were concatenated (old newline bug in memtrack).
        // memtrack never logs a successful alloc with a null return value,
        // so discard this record rather than inserting a bogus ptr=0 entry.
        if (ptr == 0) return;

        // Register thread and track bytes allocated.
        st.thread(tid, tname).allocated += sz;

        AllocRecord rec;
        rec.ptr          = ptr;
        rec.tid          = tid;
        rec.thread_name  = tname;
        rec.op           = op;
        rec.size         = sz;
        rec.total        = total;
        rec.timestamp_us = (uint64_t)ts_ull;

        size_t idx = st.records.size();

        // If this pointer is already tracked as live, a matching free was written
        // out-of-order in the log due to thread-local buffer flushing (the free
        // happened chronologically before this alloc but its write was delayed).
        // The allocator guarantees the same address cannot be live-allocated
        // twice, so silently retire the old record as freed-by-reuse.
        auto existing = st.ptr_idx.find(ptr);
        if (existing != st.ptr_idx.end()) {
            auto& old_rec = st.records[existing->second];
            if (!old_rec.freed) {
                old_rec.freed = true;
                // Don't penalise the thread's freed counter — the real free
                // event will arrive later and would double-credit otherwise.
                // Just ensure the record isn't flagged as a leak.
            }
        }

        st.ptr_idx[ptr] = idx;
        st.records.push_back(std::move(rec));
        st.last     = ParseState::Last::ALLOC;
        st.last_rec = idx;
        return;
    }

    // ── Free ──────────────────────────────────────────────────────────────
    if (is_free_op(op)) {
        void*  ptr_raw = nullptr;
        unsigned long long ts_ull = 0;
        // Use strstr-based extraction to handle both:
        //   full mode:    "free ts=N size=N ptr=0x..."
        //   compact mode: "free ts=N ptr=0x..."  (no size= field)
        const char* pts  = strstr(after, "ts=");
        const char* pptr = strstr(after, "ptr=");
        if (pts)  sscanf(pts,  "ts=%llu",  &ts_ull);
        if (pptr) sscanf(pptr, "ptr=%p",   &ptr_raw);
        uintptr_t ptr = (uintptr_t)ptr_raw;

        // Register the freeing thread (so 't' can filter by it) but apply the
        // freed bytes to the ALLOCATING thread so Net(live) = memory that thread
        // still owns, regardless of which thread happens to call free().
        st.thread(tid, tname);

        auto it = st.ptr_idx.find(ptr);
        if (it != st.ptr_idx.end()) {
            auto& rec              = st.records[it->second];
            // Credit the freed bytes back to whoever allocated the memory.
            auto& alloc_th = st.thread(rec.tid, rec.thread_name);
            alloc_th.freed += rec.size;
            // If this was previously reported as a leak (cross-thread free
            // after exit handler), cancel the leak entry in thread stats.
            if (rec.is_leak) {
                rec.is_leak = false;
                if (alloc_th.leak_count > 0) alloc_th.leak_count--;
                if (alloc_th.leak_bytes >= rec.size) alloc_th.leak_bytes -= rec.size;
                else alloc_th.leak_bytes = 0;
            }
            rec.freed              = true;
            rec.free_tid           = tid;
            rec.free_thread_name   = tname;
            rec.free_op            = op;
            rec.free_timestamp_us  = (uint64_t)ts_ull;
            st.last     = ParseState::Last::FREE;
            st.last_rec = it->second;
            st.ptr_idx.erase(it);
        }
        return;
    }
}

// Mark every remaining unfreed allocation as a leak and update thread stats.
// Called when the log stream is complete (file EOF or TCP disconnect).
// memtrack emits no LEAK/SUMMARY lines, so this is the final opportunity to
// catch allocations in threads that never received an EXIT line (e.g. SIGKILL).
static void finalize_leaks(ParseState& st)
{
    for (auto& r : st.records) {
        if (!r.freed && !r.is_leak) {
            r.is_leak = true;
            auto& th = st.thread(r.tid, r.thread_name);
            th.leak_count++;
            th.leak_bytes += r.size;
        }
    }
}

static ParseState parse_file(const string& path, long* out_pos = nullptr)
{    ParseState st;
    FILE* fp = fopen(path.c_str(), "r");
    if (!fp) {
        if (out_pos) *out_pos = 0;
        return st;
    }
    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        parse_line(line, st);
    }
    if (out_pos) *out_pos = ftell(fp);
    fclose(fp);
    return st;
}

// ─── Live reader ─────────────────────────────────────────────────────────────
//
// Opened once after the initial parse; poll_new() appends any newly written
// lines to the ParseState.  Detects file truncation (application restarted)
// and signals the UI to clear and rebuild from scratch.

struct LiveReader {
    string path;
    FILE*  fp           = nullptr;
    long   pos          = 0;
    bool   is_stdin     = false;
    bool   is_socket    = false;  // TCP connection — no seek, EOF = disconnected
    bool   disconnected = false;  // server closed the connection

    FILE*  save_fp      = nullptr; // optional file to tee all received lines into

    ~LiveReader() {
        if (save_fp) { fflush(save_fp); fclose(save_fp); save_fp = nullptr; }
    }

    bool open_save(const string& savepath) {
        save_fp = fopen(savepath.c_str(), "w");
        return save_fp != nullptr;
    }

    // `initial_pos` is the byte offset after the initial parse so we don't
    // re-process already-seen lines.
    void open(const string& p, long initial_pos = 0)
    {
        path     = p;
        is_stdin = (p == "-" || p == "/dev/stdin");

        if (is_stdin) {
            fp  = stdin;
            pos = 0;
            // Make stdin non-blocking so poll_new() can return quickly.
            int flags = fcntl(fileno(stdin), F_GETFL, 0);
            if (flags != -1)
                fcntl(fileno(stdin), F_SETFL, flags | O_NONBLOCK);
        } else {
            fp  = fopen(p.c_str(), "r");
            pos = initial_pos;
            if (fp) fseek(fp, pos, SEEK_SET);
        }
    }

    // Connect to a memtrack TCP server.  Returns true on success.
    // Always implies live mode; the UI header shows the connection address.
    bool connect_tcp(const string& host, int port)
    {
        struct addrinfo hints = {}, *res = nullptr;
        hints.ai_family   = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        string port_str   = std::to_string(port);

        if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0 || !res)
            return false;

        int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        bool ok = (sock >= 0) &&
                  (connect(sock, res->ai_addr, res->ai_addrlen) == 0);
        freeaddrinfo(res);

        if (!ok) { if (sock >= 0) close(sock); return false; }

        // Non-blocking so poll_new() can return between polls.
        int flags = fcntl(sock, F_GETFL, 0);
        if (flags != -1)
            fcntl(sock, F_SETFL, flags | O_NONBLOCK);
        fp = fdopen(sock, "r");
        if (!fp) { close(sock); return false; }
        is_socket   = true;
        is_stdin    = false;
        disconnected = false;
        pos         = 0;
        return fp != nullptr;
    }

    // Read any newly appended lines into `st`.
    // Returns true if the file was truncated (application restarted) — the
    // caller should treat this as a full reset of the display.
    // `new_count` is set to the number of new lines parsed this call.
    bool poll_new(ParseState& st, int& new_count)
    {
        new_count = 0;
        if (disconnected) return false;

        if (!fp && !is_stdin && !is_socket) {
            // File may not have appeared yet — try again next poll.
            fp = fopen(path.c_str(), "r");
            if (!fp) return false;
            pos = 0;
        }

        if (is_socket || is_stdin) {
            // Non-blocking stream: check readability before attempting fgets.
            int raw_fd = fp ? fileno(fp) : (is_stdin ? fileno(stdin) : -1);
            struct pollfd pfd = { raw_fd, POLLIN, 0 };
            if (::poll(&pfd, 1, 0) <= 0) return false;
        } else {
            // Detect file truncation: current end is before our read position.
            long saved = ftell(fp);
            if (saved < 0) return false;  // seek error, skip truncation check
            fseek(fp, 0, SEEK_END);
            long cur_end = ftell(fp);
            fseek(fp, saved, SEEK_SET);
            if (cur_end < 0) return false;  // seek error

            if (cur_end < pos) {
                // File was truncated — clear state and restart from top.
                st  = ParseState{};
                pos = 0;
                fseek(fp, 0, SEEK_SET);
                clearerr(fp);
                return true;   // signal: UI should reset
            }
            if (cur_end == pos) return false;  // nothing new
        }

        char line_buf[4096];
        // Limit lines per poll so fast streams don't starve keyboard input.
        // The caller will immediately call poll_new again on the next tick.
        static constexpr int MAX_LINES_PER_POLL = 500;
        int limit = MAX_LINES_PER_POLL;
        while (limit-- > 0 && fgets(line_buf, sizeof(line_buf), fp)) {
            // Tee raw line (including newline) to the save file before parsing.
            if (save_fp) fputs(line_buf, save_fp);
            size_t len = strlen(line_buf);
            if (len > 0 && line_buf[len - 1] == '\n') line_buf[len - 1] = '\0';
            parse_line(line_buf, st);
            new_count++;
        }

        if (is_socket) {
            if (feof(fp)) { finalize_leaks(st); disconnected = true; return false; }
            clearerr(fp);
        } else if (is_stdin) {
            if (feof(fp)) { finalize_leaks(st); disconnected = true; }
            else clearerr(fp);
        } else {
            long p = ftell(fp);
            if (p >= 0) pos = p;
            clearerr(fp);   // clear EOF so next fgets works
        }

        return false;
    }
};

// ─── Colour pairs ────────────────────────────────────────────────────────────

enum {
    C_NORMAL    = 1,  // white / black
    C_HEADER    = 2,  // white / blue   (panel headers, unfocused)
    C_ALLOC     = 3,  // green / black  (allocations)
    C_FREE      = 4,  // yellow / black (freed)
    C_LEAK      = 5,  // red / black    (leaks)
    C_SEL       = 6,  // black / white  (selected row)
    C_DIM       = 7,  // dim white      (stack frames, hints)
    C_THREAD    = 8,  // cyan / black   (thread info)
    C_FOCUS_HDR = 9,  // black / cyan   (focused pane title bar)
    C_GROUP     = 10, // magenta / black (grouped entries)
    C_TIMELINE  = 11, // white / black   (timeline bar background)
};

static void init_colors()
{
    start_color();
    use_default_colors();
    init_pair(C_NORMAL,    COLOR_WHITE,  COLOR_BLACK);
    init_pair(C_HEADER,    COLOR_WHITE,  COLOR_BLUE);
    init_pair(C_ALLOC,     COLOR_GREEN,  COLOR_BLACK);
    init_pair(C_FREE,      COLOR_YELLOW, COLOR_BLACK);
    init_pair(C_LEAK,      COLOR_RED,    COLOR_BLACK);
    init_pair(C_SEL,       COLOR_BLACK,  COLOR_WHITE);
    init_pair(C_DIM,       COLOR_WHITE,  COLOR_BLACK);
    init_pair(C_THREAD,    COLOR_CYAN,   COLOR_BLACK);
    init_pair(C_FOCUS_HDR, COLOR_BLACK,   COLOR_CYAN);
    init_pair(C_GROUP,     COLOR_MAGENTA, COLOR_BLACK);
    init_pair(C_TIMELINE,  COLOR_WHITE,   COLOR_BLACK);
}

// ─── UI state ────────────────────────────────────────────────────────────────

// ─── Leak grouping ───────────────────────────────────────────────────────────

struct LeakGroup {
    string         key;          // grouping key (joined frames or "op:X")
    vector<string> frames;       // frames from the example record
    string         op;           // op from example record
    string         thread_name;  // set if all records share the same thread
    int            tid       = -1;
    size_t         count     = 0;
    size_t         total_size = 0;
    size_t         example_idx = 0;  // index into ps->records (for detail pane)
    // Growth analysis (computed in rebuild_groups)
    bool           monotonic_growth = false; // live bytes only ever increase
    uint64_t       min_ts = UINT64_MAX;      // earliest alloc in group
    uint64_t       max_ts = 0;               // latest alloc in group
    double         bytes_per_sec = 0.0;      // allocation rate
};

enum FilterMode { F_ALL, F_LEAKS, F_ACTIVE, F_FREED, F_COUNT };
static const char* filter_label[] = { "All", "Leaks", "Active", "Freed" };

enum SortMode  { S_TIME, S_SIZE, S_THREAD, S_COUNT };
static const char* sort_label[]   = { "Time", "Size↕", "Thread" };


enum ThreadSortMode { TS_NET, TS_NAME, TS_TID, TS_ALLOC, TS_FREED, TS_LEAKS };
enum GroupSortMode  { GS_TOTAL, GS_COUNT, GS_RATE };  // sort modes for group view

struct UI {
    ParseState*    ps           = nullptr;
    vector<size_t> visible;        // indices into ps->records
    FilterMode     filter        = F_ALL;
    int            tid_filter    = -1;   // -1 = all threads
    SortMode       sort          = S_TIME;
    bool           sort_rev      = false;
    int            selected      = 0;    // index into visible[]
    int            list_top      = 0;    // scroll offset in list pane
    int            detail_top    = 0;    // scroll offset in detail pane
    bool           focus_detail  = false;
    bool           focus_threads = false;
    int            threads_top   = 0;    // scroll offset in thread summary pane
    ThreadSortMode thread_sort     = TS_NET;  // Thread Summary sort column
    bool           thread_sort_rev = false;   // reverse Thread Summary sort
    bool           live          = false;
    bool           auto_follow   = true; // scroll to newest in live mode
    bool           resolve_lines = false; // show addr2line source locations
    bool           group_mode      = false;  // fold records by stack trace
    GroupSortMode  group_sort      = GS_TOTAL;
    bool           group_sort_rev  = false;
    vector<LeakGroup> groups;                // populated when group_mode==true
    uint64_t       lifetime_min_us = 0;      // 0 = disabled; hide allocations younger than this
    string         search_str;               // empty = disabled; case-insensitive substring filter
    bool           delta_mode  = false;      // show only allocations after mark_ts
    uint64_t       mark_ts     = 0;          // timestamp of mark point (µs)
    bool           show_timeline = true;     // show sparkline timeline in header
    bool           hist_mode    = false;     // show size-distribution histogram

    // Persistent timeline markers (M key)
    struct Marker { uint64_t ts; int id; };  // id = 1..9
    vector<Marker> markers;                  // kept sorted by ts
    int            next_marker_id = 1;

    // Range filter: show only records allocated in [range_start, range_end]
    bool     range_active = false;
    uint64_t range_start  = 0;
    uint64_t range_end    = 0;

    // Whether a record is currently selected (valid index).
    bool has_selection() const {
        if (selected < 0) return false;
        if (group_mode) return selected < (int)groups.size();
        return !visible.empty() && selected < (int)visible.size();
    }

    // Timestamp of currently selected record (0 if nothing selected).
    uint64_t selected_ts() const {
        if (!has_selection()) return 0;
        if (group_mode) {
            if (selected >= (int)groups.size()) return 0;
            // Use example record from the group
            size_t ex = groups[selected].example_idx;
            return ex < ps->records.size() ? ps->records[ex].timestamp_us : 0;
        }
        if (visible.empty() || selected >= (int)visible.size()) return 0;
        return ps->records[visible[selected]].timestamp_us;
    }

    // Add a marker at ts; keeps markers sorted; returns its id (0 if full).
    int add_marker(uint64_t ts) {
        if (markers.size() >= 9) return 0;
        Marker m{ts, next_marker_id++};
        int id = m.id;
        markers.insert(
            std::lower_bound(markers.begin(), markers.end(), m,
                             [](const Marker& a, const Marker& b){ return a.ts < b.ts; }),
            m);
        return id;
    }

    // Remove the marker whose timestamp is closest to ts.
    void remove_nearest_marker(uint64_t ts) {
        if (markers.empty()) return;
        auto it = std::min_element(markers.begin(), markers.end(),
                                   [ts](const Marker& a, const Marker& b){
                                       uint64_t da = (a.ts > ts) ? a.ts - ts : ts - a.ts;
                                       uint64_t db = (b.ts > ts) ? b.ts - ts : ts - b.ts;
                                       return da < db;
                                   });
        markers.erase(it);
    }

    // Snap to nearest marker at or before ts (for range start), or ts itself.
    uint64_t snap_left(uint64_t ts) const {
        uint64_t best = ts;
        for (const auto& m : markers) if (m.ts <= ts) best = m.ts;
        return best;
    }

    // Snap to nearest marker at or after ts (for range end), or ts itself.
    uint64_t snap_right(uint64_t ts) const {
        for (const auto& m : markers) if (m.ts >= ts) return m.ts;
        return ts;
    }

    // reset_scroll=true: reset selection and scroll (filter changes, full reset).
    // reset_scroll=false: preserve selection and scroll (incremental data update).
    void rebuild(bool reset_scroll = true)
    {
        // Compute max timestamp across all records (used as "now" for lifetime filter)
        uint64_t max_ts = 0;
        for (const auto& r : ps->records) {
            max_ts = max(max_ts, r.timestamp_us);
            if (r.freed) max_ts = max(max_ts, r.free_timestamp_us);
        }

        visible.clear();
        for (size_t i = 0; i < ps->records.size(); i++) {
            const auto& r = ps->records[i];
            if (tid_filter != -1 && r.tid != tid_filter) continue;
            if (filter == F_LEAKS  && r.freed && !r.is_leak) continue;
            if (filter == F_ACTIVE && r.freed)               continue;
            if (filter == F_FREED  && !r.freed)              continue;
            // Lifetime filter: age = time from alloc to "now" (max_ts)
            if (lifetime_min_us > 0) {
                uint64_t age = (max_ts >= r.timestamp_us) ? (max_ts - r.timestamp_us) : 0;
                if (age < lifetime_min_us) continue;
            }
            // Delta filter: only show allocations that appeared after the mark point
            if (delta_mode && r.timestamp_us <= mark_ts) continue;
            // Range filter: only show allocations within [range_start, range_end]
            if (range_active && (r.timestamp_us < range_start || r.timestamp_us > range_end)) continue;
            // Symbol search: case-insensitive substring across frames, thread, op, ptr
            if (!search_str.empty()) {
                auto ci_contains = [&](const string& haystack) {
                    if (haystack.size() < search_str.size()) return false;
                    auto it = std::search(haystack.begin(), haystack.end(),
                                         search_str.begin(), search_str.end(),
                                         [](char a, char b) {
                                             return tolower((unsigned char)a) ==
                                                    tolower((unsigned char)b);
                                         });
                    return it != haystack.end();
                };
                bool match = ci_contains(r.thread_name) || ci_contains(r.op);
                if (!match) for (const auto& f : r.frames)
                    if (ci_contains(f)) { match = true; break; }
                if (!match) for (const auto& f : r.free_frames)
                    if (ci_contains(f)) { match = true; break; }
                // Also match hex pointer string
                if (!match) {
                    char ptr_hex[20];
                    snprintf(ptr_hex, sizeof(ptr_hex), "0x%014" PRIxPTR, r.ptr);
                    match = ci_contains(ptr_hex);
                }
                if (!match) continue;
            }
            visible.push_back(i);
        }

        // Sort (stable so equal keys preserve insertion order)
        const auto& recs = ps->records;
        auto cmp = [&](size_t a, size_t b) {
            int c = 0;
            switch (sort) {
                case S_TIME:
                    if (recs[a].timestamp_us != recs[b].timestamp_us)
                        c = (recs[a].timestamp_us < recs[b].timestamp_us) ? -1 : 1;
                    break;
                case S_SIZE:
                    if (recs[a].size != recs[b].size)
                        c = (recs[a].size > recs[b].size) ? -1 : 1;
                    break;
                case S_THREAD:
                    c = recs[a].thread_name.compare(recs[b].thread_name);
                    if (c == 0 && recs[a].tid != recs[b].tid)
                        c = recs[a].tid < recs[b].tid ? -1 : 1;
                    break;
                default: break;
            }
            if (c == 0) c = (a < b) ? -1 : (a > b ? 1 : 0);
            return sort_rev ? c > 0 : c < 0;
        };
        std::stable_sort(visible.begin(), visible.end(), cmp);

        if (reset_scroll) {
            selected   = 0;
            list_top   = 0;
            detail_top = 0;
            threads_top  = 0;
        } else {
            selected = min(selected, (int)(group_mode ? groups.size() : visible.size()) - 1);
            if (selected < 0) selected = 0;
        }
        if (group_mode) rebuild_groups();
    }

    // Compute the grouping key for a single record (same logic as rebuild_groups).
    static string group_key(const AllocRecord& r) {
        if (!r.frames.empty()) {
            string k;
            for (const auto& f : r.frames) { k += f; k += '\n'; }
            return k;
        }
        return "op:" + r.op;
    }

    // Switch to group mode and select the group that contains the given record.
    void jump_to_group(const AllocRecord& r) {
        group_mode = true;
        rebuild_groups();   // re-sorts, may reorder
        string key = group_key(r);
        for (int i = 0; i < (int)groups.size(); i++) {
            if (groups[i].key == key) {
                selected   = i;
                list_top   = max(0, i - 3); // a few rows of context above
                detail_top = 0;
                return;
            }
        }
        selected = 0; list_top = 0; detail_top = 0;
    }

    void rebuild_groups()
    {
        groups.clear();
        std::unordered_map<string, size_t> key_idx;
        // Collect timestamped net-byte events per group in one pass.
        std::unordered_map<string, vector<pair<uint64_t,int64_t>>> events_map;

        for (size_t vi : visible) {
            const auto& r = ps->records[vi];
            string key;
            if (!r.frames.empty()) {
                for (const auto& f : r.frames) { key += f; key += '\n'; }
            } else {
                key = "op:" + r.op;
            }

            auto it = key_idx.find(key);
            if (it == key_idx.end()) {
                key_idx[key] = groups.size();
                LeakGroup g;
                g.key         = key;
                g.frames      = r.frames;
                g.op          = r.op;
                g.thread_name = r.thread_name;
                g.tid         = r.tid;
                g.count       = 1;
                g.total_size  = r.size;
                g.example_idx = vi;
                g.min_ts      = r.timestamp_us;
                g.max_ts      = r.timestamp_us;
                groups.push_back(std::move(g));
            } else {
                auto& g = groups[it->second];
                g.count++;
                g.total_size += r.size;
                if (g.tid != r.tid) { g.tid = -1; g.thread_name = ""; }
                g.min_ts = min(g.min_ts, r.timestamp_us);
                g.max_ts = max(g.max_ts, r.timestamp_us);
            }

            // Accumulate events for monotonic growth check (alloc + optional free)
            auto& evs = events_map[key];
            evs.push_back({r.timestamp_us, (int64_t)r.size});
            if (r.freed)
                evs.push_back({r.free_timestamp_us, -(int64_t)r.size});
        }

        // Post-process: monotonic growth and rate — O(n log n) total
        for (auto& g : groups) {
            auto& evs = events_map[g.key];
            std::sort(evs.begin(), evs.end());
            int64_t net = 0, prev_net = 0;
            bool monotonic = true;
            for (auto& [ts, delta] : evs) {
                net += delta;
                if (net < prev_net) { monotonic = false; break; }
                prev_net = net;
            }
            g.monotonic_growth = monotonic && g.count >= 2 && net > 0;

            uint64_t span_us = (g.max_ts > g.min_ts) ? (g.max_ts - g.min_ts) : 0;
            g.bytes_per_sec  = (span_us > 0)
                               ? (double)g.total_size / ((double)span_us / 1e6)
                               : 0.0;
        }

        // Sort
        std::stable_sort(groups.begin(), groups.end(),
            [this](const LeakGroup& a, const LeakGroup& b) {
                bool less;
                if (group_sort == GS_COUNT)
                    less = a.count > b.count;
                else if (group_sort == GS_RATE)
                    less = a.bytes_per_sec > b.bytes_per_sec;
                else
                    less = a.total_size > b.total_size;
                return group_sort_rev ? !less : less;
            });

        selected = min(selected, max(0, (int)groups.size() - 1));
    }

    const AllocRecord* current() const {
        if (group_mode) {
            if (groups.empty()) return nullptr;
            int idx = min(selected, (int)groups.size() - 1);
            return &ps->records[groups[idx].example_idx];
        }
        if (visible.empty()) return nullptr;
        return &ps->records[visible[selected]];
    }

    const LeakGroup* current_group() const {
        if (!group_mode || groups.empty()) return nullptr;
        int idx = min(selected, (int)groups.size() - 1);
        return &groups[idx];
    }
};

static string fmt_size(size_t sz)
{
    char buf[32];
    if      (sz >= 1024*1024) snprintf(buf, sizeof(buf), "%.1f MB", sz/1048576.0);
    else if (sz >= 1024)      snprintf(buf, sizeof(buf), "%.1f KB", sz/1024.0);
    else                      snprintf(buf, sizeof(buf), "%zu B",   sz);
    return buf;
}

// Format elapsed microseconds as human-readable time
static string fmt_time_ms(uint64_t us)
{
    char buf[32];
    double ms = us / 1000.0;
    if (ms >= 60000.0) {
        int mins = (int)(ms / 60000.0);
        snprintf(buf, sizeof(buf), "%dm%.3fs", mins, (ms - mins * 60000.0) / 1000.0);
    } else if (ms >= 1000.0)
        snprintf(buf, sizeof(buf), "%.3fs",  ms / 1000.0);
    else
        snprintf(buf, sizeof(buf), "%.3fms", ms);
    return buf;
}

static void hline_to_eol(WINDOW* w, int y, int col_pair)
{
    int h, width; getmaxyx(w, h, width); (void)h;
    // When the previous write exactly filled the row, the cursor wraps to the
    // next row (getcury > y).  In that case the row is already fully painted —
    // overwriting it with spaces would erase the content we just wrote.
    if (getcury(w) != y) return;
    int x = getcurx(w);
    wattron(w, COLOR_PAIR(col_pair));
    while (x < width) { mvwaddch(w, y, x++, ' '); }
    wattroff(w, COLOR_PAIR(col_pair));
}

// ─── Draw: header ────────────────────────────────────────────────────────────

static void draw_header(WINDOW* w, const UI& ui, const string& filename,
                        const LiveReader* reader = nullptr)
{
    int h, width; getmaxyx(w, h, width); (void)h;
    size_t leaks = 0, leak_bytes = 0;
    for (const auto& r : ui.ps->records)
        if (!r.freed || r.is_leak) { leaks++; leak_bytes += r.size; }

    wattron(w, COLOR_PAIR(C_HEADER) | A_BOLD);

    // Row 0: title + live/TCP indicator
    if (ui.live) {
        wattroff(w, COLOR_PAIR(C_HEADER) | A_BOLD);
        bool disc = reader && reader->disconnected;
        if (disc) {
            // Grey/dim when disconnected (application has exited)
            wattron(w, COLOR_PAIR(C_DIM) | A_DIM);
            mvwaddstr(w, 0, 0, " ✕ DONE ");
            wattroff(w, COLOR_PAIR(C_DIM) | A_DIM);
        } else if (ui.auto_follow) {
            wattron(w, COLOR_PAIR(C_ALLOC) | A_BOLD);
            mvwaddstr(w, 0, 0, reader && reader->is_socket ? " ● TCP  " : " ● LIVE ");
            wattroff(w, COLOR_PAIR(C_ALLOC) | A_BOLD);
        } else {
            wattron(w, COLOR_PAIR(C_FREE)  | A_BOLD);
            mvwaddstr(w, 0, 0, reader && reader->is_socket ? " ◌ TCP  " : " ◌ LIVE ");
            wattroff(w, COLOR_PAIR(C_FREE)  | A_BOLD);
        }
        wattron(w, COLOR_PAIR(C_HEADER) | A_BOLD);
        wprintw(w, " %s  │  %zu allocs  │  %zu shown",
                filename.c_str(), ui.ps->records.size(), ui.visible.size());
    } else {
        mvwprintw(w, 0, 0, " memtrack viewer  %s  │  %zu allocs  │  %zu shown",
                  filename.c_str(), ui.ps->records.size(), ui.visible.size());
    }
    hline_to_eol(w, 0, C_HEADER);

    // Row 1: filter/sort/leak summary
    mvwprintw(w, 1, 0, " Filter: %-8s │  Thread: %-15s │  Sort: %s%s │  Leaks: %zu  (%s)",
              filter_label[ui.filter],
              ui.tid_filter == -1 ? "all" : [&]() -> string {
                  for (auto& t : ui.ps->threads)
                      if (t.tid == ui.tid_filter) return t.name;
                  return "?";
              }().c_str(),
              sort_label[ui.sort],
              ui.sort_rev ? " ▲" : " ▼",
              leaks, fmt_size(leak_bytes).c_str());
    if (ui.range_active) {
        wattroff(w, COLOR_PAIR(C_HEADER) | A_BOLD);
        wattron(w, COLOR_PAIR(C_GROUP) | A_BOLD);
        wprintw(w, "  │  ▶◀ RANGE %s – %s",
                fmt_time_ms(ui.range_start).c_str(),
                fmt_time_ms(ui.range_end).c_str());
        wattroff(w, COLOR_PAIR(C_GROUP) | A_BOLD);
        wattron(w, COLOR_PAIR(C_HEADER) | A_BOLD);
    }
    if (ui.delta_mode) {
        // Highlight the delta marker prominently so it's impossible to miss
        wattroff(w, COLOR_PAIR(C_HEADER) | A_BOLD);
        wattron(w, COLOR_PAIR(C_FREE) | A_BOLD);
        wprintw(w, "  │  ▶ DELTA since %s", fmt_time_ms(ui.mark_ts).c_str());
        wattroff(w, COLOR_PAIR(C_FREE) | A_BOLD);
        wattron(w, COLOR_PAIR(C_HEADER) | A_BOLD);
    }
    if (!ui.search_str.empty()) {
        wprintw(w, "  │  Search: %s", ui.search_str.c_str());
    }
    if (ui.lifetime_min_us > 0) {
        wprintw(w, "  │  Age≥%s", fmt_time_ms(ui.lifetime_min_us).c_str());
    }
    if (ui.live) {
        wprintw(w, "  │  Follow: %s", ui.auto_follow ? "ON [F]" : "OFF[F]");
    }
    hline_to_eol(w, 1, C_HEADER);
    wattroff(w, COLOR_PAIR(C_HEADER) | A_BOLD);

    // Row 2: timeline sparkline (toggle with 'Z')
    if (ui.show_timeline) {
        static const wchar_t bars[] = L" ▁▂▃▄▅▆▇█";
        int height, cols_; getmaxyx(w, height, cols_); (void)height;

        const auto& recs = ui.ps->records;
        int tid_f = ui.tid_filter;
        bool any = false;
        for (const auto& r : recs) if (tid_f == -1 || r.tid == tid_f) { any = true; break; }

        // Resolve thread label (shown left of sparkline when thread is filtered)
        string tname;
        int bar_x = 1;   // x-position where the sparkline starts
        if (tid_f != -1) {
            for (const auto& t : ui.ps->threads)
                if (t.tid == tid_f) { tname = t.name; break; }
            bar_x = 2 + (int)tname.size() + 3;  // " [name] "
        }
        int span_lbl_w = 10;  // reserve space for the right-side duration label
        int tw = max(1, cols_ - bar_x - span_lbl_w);  // sparkline cell count

        if (tw > 4 && any) {
            // Always use GLOBAL time range so all thread views share the same x-axis.
            // Only bar heights differ when a thread filter is active.
            uint64_t t_min = UINT64_MAX, t_max = 0;
            for (const auto& r : recs) {
                t_min = min(t_min, r.timestamp_us);
                t_max = max(t_max, r.timestamp_us);
                if (r.freed) t_max = max(t_max, r.free_timestamp_us);
            }
            uint64_t span = max<uint64_t>(1, t_max - t_min);

            // Bucket: net live bytes per column cell (filtered by thread if active)
            vector<int64_t> net(tw, 0);
            for (const auto& r : recs) {
                if (tid_f != -1 && r.tid != tid_f) continue;
                int col = (int)(((r.timestamp_us - t_min) * (uint64_t)tw) / (span + 1));
                col = max(0, min(tw - 1, col));
                net[col] += (int64_t)r.size;
                if (r.freed) {
                    int fc = (int)(((r.free_timestamp_us - t_min) * (uint64_t)tw) / (span + 1));
                    fc = max(0, min(tw - 1, fc));
                    net[fc] -= (int64_t)r.size;
                }
            }

            // Running live bytes and peak
            int64_t peak = 1;
            vector<int64_t> live(tw, 0);
            int64_t running = 0;
            for (int i = 0; i < tw; i++) { running += net[i]; live[i] = running; peak = max(peak, running); }

            // Draw left label
            if (tid_f != -1) {
                wattron(w, COLOR_PAIR(C_THREAD));
                mvwprintw(w, 2, 0, " [%s] ", tname.empty() ? "?" : tname.c_str());
                wattroff(w, COLOR_PAIR(C_THREAD));
            } else {
                mvwaddstr(w, 2, 0, " ");
            }
            wmove(w, 2, bar_x);

            for (int i = 0; i < tw; i++) {
                int64_t v = max<int64_t>(0, live[i]);
                int lvl = (int)(v * 8 / peak);
                lvl = max(0, min(8, lvl));
                bool growing = net[i] >= 0;

                // Compute this column's timestamp for range checks
                uint64_t col_ts = t_min + (uint64_t)i * (uint64_t)span / (uint64_t)max(1, tw - 1);

                // Delta mark column
                if (ui.delta_mode && ui.mark_ts > t_min) {
                    int mark_col = (int)(((ui.mark_ts - t_min) * (uint64_t)tw) / (span + 1));
                    if (i == min(tw - 1, mark_col)) {
                        wattron(w, COLOR_PAIR(C_FREE) | A_BOLD);
                        waddch(w, '|');
                        wattroff(w, COLOR_PAIR(C_FREE) | A_BOLD);
                        continue;
                    }
                }

                // Range shading: dim columns outside range, bold inside
                bool in_range = !ui.range_active ||
                                (col_ts >= ui.range_start && col_ts <= ui.range_end);

                // Range boundary columns
                if (ui.range_active) {
                    int sc = (int)(((ui.range_start - t_min) * (uint64_t)tw) / (span + 1));
                    int ec = (int)(((ui.range_end   - t_min) * (uint64_t)tw) / (span + 1));
                    sc = max(0, min(tw - 1, sc));
                    ec = max(0, min(tw - 1, ec));
                    if (i == sc || i == ec) {
                        wattron(w, COLOR_PAIR(C_GROUP) | A_BOLD);
                        waddch(w, i == sc ? '[' : ']');
                        wattroff(w, COLOR_PAIR(C_GROUP) | A_BOLD);
                        continue;
                    }
                }

                int cp = growing ? C_ALLOC : C_FREE;
                attr_t extra = in_range ? A_BOLD : A_DIM;
                cchar_t cc;
                wchar_t wch[2] = { bars[lvl], 0 };
                setcchar(&cc, wch, extra, (short)cp, nullptr);
                wadd_wch(w, &cc);
            }

            // Overlay persistent markers AFTER sparkline (always visible).
            for (const auto& mk : ui.markers) {
                if (mk.ts < t_min || mk.ts > t_min + span) continue;
                int mc = (int)(((mk.ts - t_min) * (uint64_t)tw) / (span + 1));
                mc = max(0, min(tw - 1, mc));
                wattron(w, COLOR_PAIR(C_GROUP) | A_BOLD);
                mvwaddch(w, 2, bar_x + mc, '0' + mk.id);
                wattroff(w, COLOR_PAIR(C_GROUP) | A_BOLD);
            }

            // Right label: total span duration
            char span_label[32];
            snprintf(span_label, sizeof(span_label), " %s", fmt_time_ms(span).c_str());
            mvwaddstr(w, 2, cols_ - (int)strlen(span_label), span_label);
        } else {
            mvwaddstr(w, 2, 0, " (no data)");
        }
        hline_to_eol(w, 2, C_NORMAL);
    }

    wrefresh(w);
}

// ─── Draw: size histogram ────────────────────────────────────────────────────

static void draw_histogram(WINDOW* w, const UI& ui)
{
    int rows, cols; getmaxyx(w, rows, cols);
    werase(w);

    bool focused = !ui.focus_detail && !ui.focus_threads;
    int hdr_cp = focused ? C_FOCUS_HDR : C_HEADER;
    wattron(w, COLOR_PAIR(hdr_cp) | A_BOLD);
    mvwaddstr(w, 0, 0, " [H] Size distribution  (visible records)");
    hline_to_eol(w, 0, hdr_cp);
    wattroff(w, COLOR_PAIR(hdr_cp) | A_BOLD);

    // Size buckets: upper bound (inclusive), label
    struct Bucket { size_t limit; const char* label; };
    static const Bucket buckets[] = {
        {        16, "     ≤16 B " },
        {        64, "     ≤64 B " },
        {       256, "    ≤256 B " },
        {      1024, "      ≤1 KB" },
        {   4*1024,  "      ≤4 KB" },
        {  16*1024,  "     ≤16 KB" },
        {  64*1024,  "     ≤64 KB" },
        { 256*1024,  "    ≤256 KB" },
        {1024*1024,  "      ≤1 MB" },
        { SIZE_MAX,  "      >1 MB" },
    };
    constexpr int NBUCKETS = 10;

    size_t count[NBUCKETS] = {};
    size_t bytes[NBUCKETS] = {};

    for (size_t vi : ui.visible) {
        const auto& r = ui.ps->records[vi];
        for (int b = 0; b < NBUCKETS; b++) {
            if (r.size <= buckets[b].limit) {
                count[b]++;
                bytes[b] += r.size;
                break;
            }
        }
    }

    size_t max_count = 1;
    for (int b = 0; b < NBUCKETS; b++) max_count = max(max_count, count[b]);

    // Bar area: cols - label(11) - count(8) - bytes(12) - 4 padding
    int bar_w = max(4, cols - 11 - 8 - 12 - 4);

    int body_rows = rows - 2;  // header + footer
    for (int b = 0; b < NBUCKETS && b < body_rows; b++) {
        int bar_len = (int)((uint64_t)count[b] * bar_w / max_count);

        // Colour by size class: small=green, medium=yellow, large=red
        int cp = (b <= 3) ? C_ALLOC : (b <= 6) ? C_FREE : C_LEAK;

        wattron(w, COLOR_PAIR(C_DIM));
        mvwprintw(w, b + 1, 0, "%s", buckets[b].label);
        wattroff(w, COLOR_PAIR(C_DIM));

        wattron(w, COLOR_PAIR(cp) | A_BOLD);
        for (int i = 0; i < bar_len; i++) waddch(w, ACS_CKBOARD);
        wattroff(w, COLOR_PAIR(cp) | A_BOLD);

        if (count[b] > 0) {
            wattron(w, COLOR_PAIR(C_NORMAL));
            wprintw(w, "  %6zu  %s", count[b], fmt_size(bytes[b]).c_str());
            wattroff(w, COLOR_PAIR(C_NORMAL));
        }
    }

    // Footer
    wattron(w, COLOR_PAIR(C_DIM) | A_DIM);
    mvwprintw(w, rows - 1, 0, " %zu records  H:close histogram",
              ui.visible.size());
    hline_to_eol(w, rows - 1, C_DIM);
    wattroff(w, COLOR_PAIR(C_DIM) | A_DIM);
    wrefresh(w);
}

// ─── Draw: allocation list (left pane) ───────────────────────────────────────

static void draw_list(WINDOW* w, const UI& ui)
{
    int rows, cols; getmaxyx(w, rows, cols);
    werase(w);

    bool list_focused = !ui.focus_detail && !ui.focus_threads;
    int  hdr_cp = list_focused ? C_FOCUS_HDR : C_HEADER;
    wattron(w, COLOR_PAIR(hdr_cp) | A_BOLD);

    if (ui.group_mode) {
        // ── Grouped header with sortable columns ────────────────────────
        mvwaddstr(w, 0, 0, " [G] ");

        // gcol: underline label+arrow for active sort column
        auto gcol = [&](const char* label, GroupSortMode m, int pad) {
            int label_len = (int)strlen(label);
            if (ui.group_sort == m) {
                wattron(w, A_UNDERLINE);
                waddstr(w, label);
                waddstr(w, ui.group_sort_rev ? "▲" : "▼");
                wattroff(w, A_UNDERLINE);
                for (int i = label_len + 1; i < pad; i++) waddch(w, ' ');
            } else {
                waddstr(w, label);
                for (int i = label_len; i < pad; i++) waddch(w, ' ');
            }
        };
        gcol("×Count", GS_COUNT, 9);
        waddstr(w, "  ");
        gcol("Total", GS_TOTAL, 10);
        waddstr(w, "  ");
        gcol("Rate/s", GS_RATE, 10);
        waddstr(w, "  Description");
        hline_to_eol(w, 0, hdr_cp);
        wattroff(w, COLOR_PAIR(hdr_cp) | A_BOLD);

        int list_rows = rows - 2;
        for (int row = 0; row < list_rows; row++) {
            int idx = ui.list_top + row;
            if (idx >= (int)ui.groups.size()) break;

            const LeakGroup& g = ui.groups[idx];
            bool sel = (idx == ui.selected);

            // Description: first frame (demangled) or op name
            string desc;
            if (!g.frames.empty()) {
                // demangle inline — just strip module(...) wrapper if present
                const string& f = g.frames[0];
                auto lp = f.find('(');
                auto rp = f.find(')', lp == string::npos ? 0 : lp);
                if (lp != string::npos && rp != string::npos && rp > lp + 1) {
                    string mangled = f.substr(lp + 1, rp - lp - 1);
                    auto plus = mangled.find('+');
                    if (plus != string::npos) mangled = mangled.substr(0, plus);
                    int   status = -1;
                    char* dem = abi::__cxa_demangle(mangled.c_str(), nullptr, nullptr, &status);
                    desc = (status == 0 && dem) ? dem : mangled;
                    free(dem);
                } else {
                    desc = f;
                }
            } else {
                desc = g.op;
            }
            // Append thread info
            string tinfo = g.tid != -1
                ? ("  [" + g.thread_name + "]")
                : "  [multiple threads]";

            int cp   = g.monotonic_growth ? C_LEAK : C_GROUP;
            attr_t attr = A_BOLD;
            if (sel && !ui.focus_detail && !ui.focus_threads) { cp = C_SEL; attr = A_BOLD; }

            wattron(w, COLOR_PAIR(cp) | attr);
            char count_buf[16], size_buf[12], rate_buf[12];
            snprintf(count_buf, sizeof(count_buf), "%s×%zu",
                     g.monotonic_growth ? "▲" : " ", g.count);
            snprintf(size_buf, sizeof(size_buf), "%s", fmt_size(g.total_size).c_str());
            if (g.bytes_per_sec >= 1.0)
                snprintf(rate_buf, sizeof(rate_buf), "%s/s", fmt_size((size_t)g.bytes_per_sec).c_str());
            else
                snprintf(rate_buf, sizeof(rate_buf), "-");
            mvwprintw(w, row + 1, 0, " [G] %9s  %-10s  %-10s  ", count_buf, size_buf, rate_buf);

            // Fill remaining width with truncated description + thread info
            int used = getcurx(w);
            int avail = max(0, cols - used);
            string full = desc + tinfo;
            if ((int)full.size() > avail) full = full.substr(0, avail);
            waddstr(w, full.c_str());

            if (getcury(w) == row + 1) {
                int x = getcurx(w);
                while (x < cols) { waddch(w, ' '); x++; }
            }
            wattroff(w, COLOR_PAIR(cp) | attr);
        }

        // Counter
        wattron(w, COLOR_PAIR(C_DIM) | A_DIM);
        mvwprintw(w, rows - 1, 0, " %d / %d groups  [Enter] expand  [\\] ungroup",
                  ui.groups.empty() ? 0 : ui.selected + 1,
                  (int)ui.groups.size());
        hline_to_eol(w, rows - 1, C_DIM);
        wattroff(w, COLOR_PAIR(C_DIM) | A_DIM);
        wrefresh(w);
        return;
    }

    // ── Normal (per-record) header ──────────────────────────────────────
    mvwaddstr(w, 0, 0, " St. ");
    waddstr(w, "Pointer            ");
    waddstr(w, "Op         ");

    // col: underline only the label text + arrow
    auto col = [&](const char* label, SortMode sm, int pad) {
        int label_len = (int)strlen(label);
        if (ui.sort == sm) {
            wattron(w, A_UNDERLINE);
            waddstr(w, label);
            waddstr(w, ui.sort_rev ? "▲" : "▼");
            wattroff(w, A_UNDERLINE);
            int used = label_len + 1;
            for (int i = used; i < pad; i++) waddch(w, ' ');
        } else {
            waddstr(w, label);
            for (int i = label_len; i < pad; i++) waddch(w, ' ');
        }
    };
    col("Time",   S_TIME,   12);
    col("Size",   S_SIZE,   11);
    col("Thread", S_THREAD,  6);

    hline_to_eol(w, 0, hdr_cp);
    wattroff(w, COLOR_PAIR(hdr_cp) | A_BOLD);

    int list_rows = rows - 2;   // -1 header -1 counter

    // Compute session time span for age colouring of live allocations
    uint64_t age_max_ts = 0, age_min_ts = UINT64_MAX;
    for (const auto& r : ui.ps->records) {
        age_min_ts = min(age_min_ts, r.timestamp_us);
        age_max_ts = max(age_max_ts, r.timestamp_us);
        if (r.freed) age_max_ts = max(age_max_ts, r.free_timestamp_us);
    }
    uint64_t age_span = (age_max_ts > age_min_ts) ? (age_max_ts - age_min_ts) : 1;

    for (int row = 0; row < list_rows; row++) {
        int idx = ui.list_top + row;
        if (idx >= (int)ui.visible.size()) break;

        const auto& r   = ui.ps->records[ui.visible[idx]];
        bool sel        = (idx == ui.selected);
        char status     = r.is_leak ? 'L' : (r.freed ? 'F' : 'A');

        char ptr_buf[24];
        snprintf(ptr_buf, sizeof(ptr_buf), "0x%014" PRIxPTR, r.ptr);

        string sz = fmt_size(r.size);

        int cp;
        attr_t attr = 0;
        if (sel && !ui.focus_detail && !ui.focus_threads) {
            cp = C_SEL; attr = A_BOLD;
        } else if (r.is_leak) {
            cp = C_LEAK; attr = A_BOLD;
        } else if (r.freed) {
            cp = C_FREE;
        } else {
            // Live allocation: colour by age (young→normal, middle→yellow, old→red)
            uint64_t age = (age_max_ts >= r.timestamp_us) ? (age_max_ts - r.timestamp_us) : 0;
            int age_pct = (int)(age * 100 / age_span);
            if      (age_pct >= 75) { cp = C_LEAK;   attr = A_BOLD; }  // old: red
            else if (age_pct >= 30) { cp = C_FREE;   attr = 0;      }  // mid: yellow
            else                    { cp = C_ALLOC;  attr = 0;      }  // young: green
        }

        wattron(w, COLOR_PAIR(cp) | attr);
        mvwprintw(w, row + 1, 0, " [%c] %-18s %-9s %-12s %-9s %-15s",
                  status, ptr_buf, r.op.c_str(),
                  fmt_time_ms(r.timestamp_us).c_str(),
                  sz.c_str(), r.thread_name.c_str());
        if (getcury(w) == row + 1) {
            int x = getcurx(w);
            while (x < cols) { waddch(w, ' '); x++; }
        }
        wattroff(w, COLOR_PAIR(cp) | attr);
    }

    // Counter
    wattron(w, COLOR_PAIR(C_DIM) | A_DIM);
    mvwprintw(w, rows - 1, 0, " %d / %d  [L]eak [A]ctive [F]reed  [\\] group  [Enter] jump to group",
              ui.visible.empty() ? 0 : ui.selected + 1,
              (int)ui.visible.size());
    hline_to_eol(w, rows - 1, C_DIM);
    wattroff(w, COLOR_PAIR(C_DIM) | A_DIM);

    wrefresh(w);
}

// ─── Draw: detail pane (right) ───────────────────────────────────────────────

// addr2line resolver — calls addr2line once per (module, addr) pair and caches
// the result.  Returns "filename:line" or "" if not resolvable.
// Handles both non-PIE (use raw addr) and PIE (resolve via nm symbol + offset).
static const string& resolve_addr2line(const string& frame)
{
    static unordered_map<string, string> cache;
    static unordered_map<string, unordered_map<string, uint64_t>> nm_cache; // module → sym → addr

    auto cached = cache.find(frame);
    if (cached != cache.end()) return cached->second;

    string& result = cache[frame];  // default-insert as ""

    // Parse "module(func+offset) [0xADDR]"
    auto lbr = frame.rfind('[');
    auto rbr = frame.rfind(']');
    auto lp  = frame.find('(');
    auto rp  = frame.find(')');
    if (lbr == string::npos || rbr == string::npos || lp == string::npos)
        return result;

    string module   = frame.substr(0, lp);
    string sym_part = (rp != string::npos && rp > lp) ? frame.substr(lp+1, rp-lp-1) : "";
    string rt_addr  = frame.substr(lbr + 1, rbr - lbr - 1);
    if (module.empty() || rt_addr.empty() || rt_addr == "??") return result;

    // Determine the address to pass addr2line.
    // For non-PIE: runtime addr == file addr — subtract 1 for return-address correction.
    // For PIE: runtime addr is ASLR-shifted.  Recover file addr via:
    //   sym_part = "funcname+0xOFFSET" → look up funcname in nm, add offset - 1.
    // The -1 is needed because backtrace() records the return address (instruction
    // after CALL), and we want to point inside the CALL for correct source mapping.
    //
    // Validate rt_addr is a pure hex number before using it in a shell command.
    // A crafted frame with rt_addr = "; evil_cmd" would otherwise be injected.
    if (rt_addr.size() < 3 || rt_addr.substr(0, 2) != "0x") return result;
    for (char c : rt_addr.substr(2))
        if (!isxdigit((unsigned char)c)) return result;

    string resolve_addr;
    {
        uint64_t raw = 0;
        try { raw = stoull(rt_addr, nullptr, 16); } catch (...) { return result; }
        if (raw > 0) {
            char buf[32];
            snprintf(buf, sizeof(buf), "0x%lx", raw - 1);
            resolve_addr = buf;
        } else {
            return result;  // null pointer — nothing useful to resolve
        }
    }

    auto plus = sym_part.rfind('+');
    if (plus != string::npos) {
        string sym    = sym_part.substr(0, plus);
        string offstr = sym_part.substr(plus + 1);  // "0xNN"
        uint64_t offset = 0;
        try { offset = stoull(offstr, nullptr, 16); } catch (...) {}

        if (!sym.empty() && !nm_cache.count(module)) {
            // Populate nm symbol table for this module.
            // Quote module path with single-quotes; escape any embedded single-quote.
            string qmod = module;
            {
                string escaped;
                for (char c : qmod) {
                    if (c == '\'') escaped += "'\\''";
                    else           escaped += c;
                }
                qmod = "'" + escaped + "'";
            }
            unordered_map<string, uint64_t> syms;
            string nm_cmd = "nm -D " + qmod + " 2>/dev/null; nm " + qmod + " 2>/dev/null";
            FILE* nfp = popen(nm_cmd.c_str(), "r");
            if (nfp) {
                char buf[512];
                while (fgets(buf, sizeof(buf), nfp)) {
                    uint64_t a = 0; char t = 0; char n[256] = {};
                    if (sscanf(buf, "%lx %c %255s", &a, &t, n) == 3 && a != 0)
                        syms.emplace(n, a);
                }
                pclose(nfp);
            }
            nm_cache[module] = std::move(syms);
        }

        // Only look up the symbol if we have a name AND the module was populated.
        // Using find() avoids operator[] which would default-insert an empty map
        // and permanently block future population for the same module.
        if (!sym.empty()) {
            auto mc = nm_cache.find(module);
            if (mc != nm_cache.end()) {
                auto sit = mc->second.find(sym);
                if (sit != mc->second.end()) {
                    char buf[32];
                    // Subtract 1: backtrace gives the return address (instruction after
                    // the call), so -1 puts us inside the CALL instruction itself,
                    // which addr2line maps to the correct calling source line.
                    snprintf(buf, sizeof(buf), "0x%lx", sit->second + offset - 1);
                    resolve_addr = buf;
                }
            }
        }
    }

    // addr2line -e <module> -f -s -C -i <addr>
    // Quote module path to handle spaces and special characters.
    string qmod_a2l = module;
    {
        string escaped;
        for (char c : qmod_a2l) {
            if (c == '\'') escaped += "'\\''";
            else           escaped += c;
        }
        qmod_a2l = "'" + escaped + "'";
    }
    string cmd = "addr2line -e " + qmod_a2l + " -f -s -C -i " + resolve_addr + " 2>/dev/null";
    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) return result;

    char buf[512];
    bool want_fileline = false;  // addr2line alternates: funcname, file:line, ...
    while (fgets(buf, sizeof(buf), fp)) {
        string line = buf;
        if (!line.empty() && line.back() == '\n') line.pop_back();
        if (want_fileline) {
            if (line != "??:0" && line != "??:?" && line != ":?") {
                if (!result.empty()) result += " → ";
                result += line;
            }
        }
        want_fileline = !want_fileline;
    }
    pclose(fp);
    return result;
}

struct DLine { string text; int cp; int attr; };

// Demangle the C++ symbol inside a backtrace_symbols() frame string.
// Frame format: "module(mangled+0xNN) [0xADDR]"
// Returns the input unchanged if no mangled name is found or demangling fails.
static string demangle_frame(const string& frame)
{
    auto lp = frame.find('(');
    if (lp == string::npos) return frame;
    auto plus = frame.find_first_of("+)", lp + 1);
    if (plus == string::npos || plus == lp + 1) return frame;  // empty or no name

    string mangled = frame.substr(lp + 1, plus - lp - 1);
    // Only attempt demangle for C++ mangled names (_Z prefix)
    if (mangled.size() < 2 || mangled[0] != '_' || mangled[1] != 'Z')
        return frame;

    int   status = -1;
    char* dem    = abi::__cxa_demangle(mangled.c_str(), nullptr, nullptr, &status);
    if (status != 0 || !dem) return frame;

    string result = frame.substr(0, lp + 1) + dem + frame.substr(plus);
    free(dem);
    return result;
}

static void add_frames(vector<DLine>& d, const vector<string>& frames, bool resolve)
{
    auto add = [&](string t, int cp = C_NORMAL, int attr = 0) {
        d.push_back({std::move(t), cp, attr});
    };
    for (int i = 0; i < (int)frames.size(); i++) {
        string display = demangle_frame(frames[i]);
        add("    #" + std::to_string(i) + "  " + display, C_DIM, A_DIM);
        if (resolve) {
            const string& loc = resolve_addr2line(frames[i]);  // use raw frame for addr lookup
            if (!loc.empty())
                add("         → " + loc, C_ALLOC, 0);
        }
    }
}

static vector<DLine> build_detail(const AllocRecord& r, bool resolve_lines,
                                   const LeakGroup* grp = nullptr)
{
    vector<DLine> d;
    auto add = [&](string t, int cp = C_NORMAL, int attr = 0) {
        d.push_back({std::move(t), cp, attr});
    };
    auto sep = [&](const char* title, int cp = C_HEADER) {
        add(string("── ") + title + " ", cp, A_BOLD);
    };

    // Group summary header (when in group mode)
    if (grp) {
        sep("Group summary");
        add("");
        add("  Count   : " + std::to_string(grp->count) + " allocations", C_LEAK, A_BOLD);
        add("  Total   : " + fmt_size(grp->total_size),                    C_LEAK, A_BOLD);
        if (grp->tid != -1)
            add("  Thread  : " + grp->thread_name + " (" + std::to_string(grp->tid) + ")",
                C_THREAD);
        else
            add("  Threads : multiple", C_THREAD);
        add("");
        sep("Representative allocation");
    }

    char ptr_buf[24];
    snprintf(ptr_buf, sizeof(ptr_buf), "0x%" PRIxPTR, r.ptr);

    sep("Allocation");
    add("");
    add("  Op      : " + r.op,                  C_ALLOC,  A_BOLD);
    add("  Ptr     : " + string(ptr_buf));
    add("  Size    : " + fmt_size(r.size));
    add("  Total   : " + fmt_size(r.total));
    add("  Time    : " + fmt_time_ms(r.timestamp_us), C_DIM);
    add("  tid     : " + std::to_string(r.tid), C_THREAD);
    add("  Thread  : " + r.thread_name,         C_THREAD);
    if (!r.frames.empty()) {
        add("");
        add("  Stack:", C_DIM, A_DIM);
        add_frames(d, r.frames, resolve_lines);
    }

    add("");
    sep("Free");
    add("");
    if (!r.freed || r.is_leak) {
        add("  *** NOT FREED — MEMORY LEAK ***", C_LEAK, A_BOLD);
    } else {
        add("  Op      : " + r.free_op,                   C_FREE,   A_BOLD);
        add("  Time    : " + fmt_time_ms(r.free_timestamp_us), C_DIM);
        if (r.free_timestamp_us >= r.timestamp_us)
            add("  Lifetime: " + fmt_time_ms(r.free_timestamp_us - r.timestamp_us), C_DIM);
        add("  tid     : " + std::to_string(r.free_tid),  C_THREAD);
        add("  Thread  : " + r.free_thread_name,          C_THREAD);
        if (!r.free_frames.empty()) {
            add("");
            add("  Stack:", C_DIM, A_DIM);
            add_frames(d, r.free_frames, resolve_lines);
        }
    }
    return d;
}

static void draw_detail(WINDOW* w, const UI& ui)
{
    int rows, cols; getmaxyx(w, rows, cols);
    werase(w);

    // Sub-header
    bool focused = ui.focus_detail && !ui.focus_threads;
    int  hdr_cp  = focused ? C_FOCUS_HDR : C_HEADER;
    wattron(w, COLOR_PAIR(hdr_cp) | A_BOLD);
    mvwprintw(w, 0, 0, " Detail%s%s",
              focused ? " [↑↓ scroll]" : "",
              ui.resolve_lines ? " [L:src ON]" : " [L:src off]");
    hline_to_eol(w, 0, hdr_cp);
    wattroff(w, COLOR_PAIR(hdr_cp) | A_BOLD);

    const AllocRecord* r = ui.current();
    if (!r) {
        wattron(w, COLOR_PAIR(C_DIM) | A_DIM);
        mvwprintw(w, 2, 2, "(no allocations)");
        wattroff(w, COLOR_PAIR(C_DIM) | A_DIM);
        wrefresh(w);
        return;
    }

    auto detail  = build_detail(*r, ui.resolve_lines, ui.current_group());
    int  visible = rows - 2;    // -1 header -1 scrollbar

    // Clamp detail_top (INT_MAX is used as "jump to bottom" sentinel).
    int max_top = max(0, (int)detail.size() - visible);
    int dtop    = min(ui.detail_top, max_top);

    for (int row = 0; row < visible; row++) {
        int di = dtop + row;
        if (di >= (int)detail.size()) break;
        const auto& dl = detail[di];
        wattron(w, COLOR_PAIR(dl.cp) | dl.attr);
        mvwprintw(w, row + 1, 0, "%-*s", cols, dl.text.c_str());
        wattroff(w, COLOR_PAIR(dl.cp) | dl.attr);
    }

    // Scroll indicator
    if ((int)detail.size() > visible) {
        wattron(w, COLOR_PAIR(C_DIM) | A_DIM);
        mvwprintw(w, rows - 1, 0, " line %d / %d ",
                  dtop + 1, (int)detail.size());
        hline_to_eol(w, rows - 1, C_DIM);
        wattroff(w, COLOR_PAIR(C_DIM) | A_DIM);
    }

    wrefresh(w);
}

// ─── Draw: status bar ────────────────────────────────────────────────────────

static void draw_status(WINDOW* w, const UI& ui)
{
    int rows, cols; getmaxyx(w, rows, cols); (void)rows; (void)cols;
    werase(w);

    // Key hints
    wattron(w, COLOR_PAIR(C_DIM) | A_DIM);
    if (ui.focus_threads)
        mvwprintw(w, 0, 0,
                  " s/S:sort(rev)  j/k:scroll  g/G:top/bot  ^f/^b:page  Tab:next-pane  q:quit");
    else
        mvwprintw(w, 0, 0,
                  " q:quit  f:filter  a:age  /:search  m:mark  M:marker  D:del-marker  [/]:range  R:clr-range  W:export  H:histogram  Z:timeline  t/T:thread  s/S:sort  Tab/h/l:pane  j/k:nav  L:src%s",
                  ui.live ? "  F:follow" : "");
    hline_to_eol(w, 0, C_DIM);
    wattroff(w, COLOR_PAIR(C_DIM) | A_DIM);

    wrefresh(w);
}

// ─── Thread Summary sort helper ──────────────────────────────────────────────

// Returns the ThreadInfo pointers sorted by the current thread sort mode.
// Used by both draw_threads and the t/T cycling code to guarantee consistency.
static vector<const ThreadInfo*> sorted_threads(const UI& ui)
{
    const auto& threads = ui.ps->threads;
    vector<const ThreadInfo*> sv;
    sv.reserve(threads.size());
    for (auto& t : threads) sv.push_back(&t);

    auto cmp = [&](const ThreadInfo* a, const ThreadInfo* b) -> bool {
        bool less = false;
        switch (ui.thread_sort) {
            case TS_NAME:  less = a->name < b->name; break;
            case TS_TID:   less = a->tid  < b->tid;  break;
            case TS_ALLOC: less = a->allocated > b->allocated; break;
            case TS_FREED: less = a->freed     > b->freed;     break;
            case TS_LEAKS: less = a->leak_bytes > b->leak_bytes; break;
            case TS_NET:   // fallthrough — default
            default:       less = a->net() > b->net(); break;
        }
        return ui.thread_sort_rev ? !less : less;
    };
    std::stable_sort(sv.begin(), sv.end(), cmp);
    return sv;
}

// ─── Draw: thread summary pane ───────────────────────────────────────────────

static void draw_threads(WINDOW* w, const UI& ui)
{
    int rows, cols; getmaxyx(w, rows, cols);
    werase(w);

    bool focused = ui.focus_threads;

    const auto& threads = ui.ps->threads;
    int  total = (int)threads.size();
    int  page  = rows - 2;

    int top = max(0, min(ui.threads_top, max(0, total - page)));

    // Title row
    int hdr_cp = focused ? C_FOCUS_HDR : C_HEADER;
    wattron(w, COLOR_PAIR(hdr_cp) | A_BOLD);
    mvwaddstr(w, 0, 0, focused ? " Threads [s/S:sort  j/k:scroll  Tab:unfocus]" :
                                 " Thread summary");
    if (ui.tid_filter != -1) {
        for (auto& t : threads)
            if (t.tid == ui.tid_filter)
                wprintw(w, "  [filter: %s]", t.name.c_str());
    }
    if (total > page) {
        char scroll_info[32];
        snprintf(scroll_info, sizeof(scroll_info), " %d-%d/%d ",
                 top + 1, min(top + page, total), total);
        int si_len = (int)strlen(scroll_info);
        mvwaddstr(w, 0, cols - si_len, scroll_info);
    }
    hline_to_eol(w, 0, hdr_cp);
    wattroff(w, COLOR_PAIR(hdr_cp) | A_BOLD);

    // Column header — active sort column: only label text + arrow underlined.
    // Fixed non-name portion: 2+7(TID)+2+10+2+10+2+10+2+16(leaks) = 63 chars.
    // Reserve 1 leading space → name_w = cols - 64.
    int name_w = max(8, cols - 64);

    // col_hdr: print a fixed-width column header.  Only the label text and the
    // sort arrow are underlined — trailing padding spaces are not.
    auto col_hdr = [&](ThreadSortMode m, const char* label, int w2) {
        bool active = (ui.thread_sort == m);
        if (active) {
            wattron(w, A_UNDERLINE);
            waddstr(w, label);
            waddstr(w, ui.thread_sort_rev ? "▲" : "▼");
            wattroff(w, A_UNDERLINE);
            int used = (int)strlen(label) + 1; // +1 for the arrow (single char)
            wprintw(w, "%-*s", max(0, w2 - used), "");
        } else {
            wprintw(w, "%-*s ", w2 - 1, label);
        }
    };

    // Name column (variable width)
    wattron(w, COLOR_PAIR(C_HEADER) | A_BOLD);
    mvwaddstr(w, 1, 0, " ");
    if (ui.thread_sort == TS_NAME) {
        wattron(w, A_UNDERLINE);
        waddstr(w, "Thread");
        waddstr(w, ui.thread_sort_rev ? "▲" : "▼");
        wattroff(w, A_UNDERLINE);
        int used = 7; // strlen("Thread")+1 for arrow
        wprintw(w, "%-*s", max(0, name_w - used), "");
    } else {
        wprintw(w, "%-*s ", name_w - 1, "Thread");
    }
    wprintw(w, "  ");
    col_hdr(TS_TID, "TID", 7);
    wprintw(w, "  ");
    col_hdr(TS_ALLOC, "Allocated", 10);
    wprintw(w, "  ");
    col_hdr(TS_FREED, "Freed", 10);
    wprintw(w, "  ");
    col_hdr(TS_NET, "Net(live)", 10);
    wprintw(w, "  ");
    col_hdr(TS_LEAKS, "Leaks", 16);
    hline_to_eol(w, 1, C_HEADER);
    wattroff(w, COLOR_PAIR(C_HEADER) | A_BOLD);

    // Data rows
    vector<const ThreadInfo*> sv = sorted_threads(ui);

    for (int i = 0; i < page && (top + i) < total; i++) {
        const ThreadInfo& t = *sv[top + i];
        size_t net = t.net();
        // "all threads" mode: highlight every row; otherwise only the filtered one.
        bool selected_thread = (ui.tid_filter == -1) || (ui.tid_filter == t.tid);
        int cp   = selected_thread ? C_SEL : (net > 0 ? C_LEAK : C_ALLOC);
        int attr = (selected_thread && ui.tid_filter != -1) ? (int)A_BOLD : 0;

        char leak_buf[32] = "-";
        if (t.leak_count > 0)
            snprintf(leak_buf, sizeof(leak_buf), "%zu (%s)",
                     t.leak_count, fmt_size(t.leak_bytes).c_str());

        wattron(w, COLOR_PAIR(cp) | attr);
        mvwprintw(w, i + 2, 0, " %-*.*s  %7d  %10s  %10s  %10s  %-16s",
                  name_w, name_w, t.name.c_str(),
                  t.tid,
                  fmt_size(t.allocated).c_str(),
                  fmt_size(t.freed).c_str(),
                  fmt_size(net).c_str(),
                  leak_buf);
        hline_to_eol(w, i + 2, cp);
        wattroff(w, COLOR_PAIR(cp) | attr);
    }

    wrefresh(w);
}

// ─── Window management ───────────────────────────────────────────────────────

struct Windows {
    WINDOW *header, *list, *detail, *threads, *status;
};

// Exact column width required for the list pane:
//   " [X] " (5) + ptr "%-18s " (19) + op "%-9s " (10) +
//   time "%-12s " (13) + size "%-9s " (10) + thread "%-15s" (15) = 72
static constexpr int LIST_W      = 72;
static constexpr int BODY_MIN    = 5;   // list pane always gets at least this many rows
static constexpr int THREADS_HDR = 2;   // title + column header rows

// Thread pane height: 2 header rows + one row per thread, capped at 1/3 of
// the total terminal height so the list/detail pane always gets enough space.
static int threads_pane_h(int n_threads)
{
    int max_h = max(THREADS_HDR + 1, LINES / 3);
    int want  = THREADS_HDR + max(1, n_threads);
    return min(want, max_h);
}

// Actual list pane width — shrinks to half on very narrow terminals so the
// detail pane always gets at least 30 columns.
static int list_pane_w() { return (COLS >= LIST_W + 30) ? LIST_W : COLS / 2; }
// Header height: 2 fixed rows + optional timeline row.
static int header_h(bool show_timeline) { return show_timeline ? 3 : 2; }
// Body height: total rows minus header, thread pane, and status(1).
static int body_height(int th_h, bool show_timeline) { return max(BODY_MIN, LINES - header_h(show_timeline) - th_h - 1); }

static Windows make_windows(int th_h, bool show_timeline)
{
    int rows = LINES, cols = COLS;
    int lw      = list_pane_w();
    int hh      = header_h(show_timeline);
    int body    = body_height(th_h, show_timeline);
    int th_y    = hh + body;
    return {
        newwin(hh,   cols,      0,    0),
        newwin(body, lw,        hh,   0),
        newwin(body, cols-lw-1, hh,   lw+1),
        newwin(th_h, cols,      th_y, 0),
        newwin(1,    cols,      rows-1, 0),
    };
}

static void free_windows(Windows& w)
{
    delwin(w.header); delwin(w.list);
    delwin(w.detail); delwin(w.threads); delwin(w.status);
}

// ─── Export to text file ─────────────────────────────────────────────────────
static bool export_to_file(const string& path, const UI& ui, const string& source)
{
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return false;

    // Header
    fprintf(f, "memtrack export — %s\n", source.c_str());
    fprintf(f, "Filter: %s  |  Thread: %s  |  Sort: %s%s",
            filter_label[ui.filter],
            ui.tid_filter == -1 ? "all" : [&]() -> string {
                for (auto& t : ui.ps->threads)
                    if (t.tid == ui.tid_filter) return t.name;
                return "?";
            }().c_str(),
            sort_label[ui.sort],
            ui.sort_rev ? " ▲" : " ▼");
    if (!ui.search_str.empty()) fprintf(f, "  |  Search: %s", ui.search_str.c_str());
    if (ui.lifetime_min_us > 0) fprintf(f, "  |  Age≥%s", fmt_time_ms(ui.lifetime_min_us).c_str());
    if (ui.delta_mode)          fprintf(f, "  |  DELTA since %s", fmt_time_ms(ui.mark_ts).c_str());
    fprintf(f, "\n");
    fprintf(f, "Records shown: %zu\n", ui.group_mode ? ui.groups.size() : ui.visible.size());
    fprintf(f, "%s\n", string(80, '-').c_str());

    if (ui.group_mode) {
        // Grouped export
        fprintf(f, "%-8s  %-12s  %s\n", "Count", "Total", "Call site");
        fprintf(f, "%s\n", string(80, '-').c_str());
        for (const auto& g : ui.groups) {
            string desc = g.frames.empty() ? g.op : demangle_frame(g.frames[0]);
            string tinfo = g.tid != -1 ? ("  [" + g.thread_name + "]") : "  [multiple threads]";
            fprintf(f, "×%-7zu  %-12s  %s%s\n",
                    g.count, fmt_size(g.total_size).c_str(),
                    desc.c_str(), tinfo.c_str());
            // All frames
            for (size_t i = 0; i < g.frames.size(); i++)
                fprintf(f, "    #%zu  %s\n", i, demangle_frame(g.frames[i]).c_str());
            fprintf(f, "\n");
        }
    } else {
        // Per-record export
        fprintf(f, "%-3s  %-18s  %-9s  %-12s  %-10s  %-15s\n",
                "St.", "Pointer", "Op", "Time", "Size", "Thread");
        fprintf(f, "%s\n", string(80, '-').c_str());
        for (size_t vi : ui.visible) {
            const auto& r = ui.ps->records[vi];
            char status = r.is_leak ? 'L' : (r.freed ? 'F' : 'A');
            char ptr_buf[24];
            snprintf(ptr_buf, sizeof(ptr_buf), "0x%014" PRIxPTR, r.ptr);
            fprintf(f, "[%c]  %-18s  %-9s  %-12s  %-10s  %s\n",
                    status, ptr_buf, r.op.c_str(),
                    fmt_time_ms(r.timestamp_us).c_str(),
                    fmt_size(r.size).c_str(),
                    r.thread_name.c_str());
            for (size_t i = 0; i < r.frames.size(); i++)
                fprintf(f, "    #%zu  %s\n", i, demangle_frame(r.frames[i]).c_str());
            if (!r.frames.empty()) fprintf(f, "\n");
        }
    }

    fclose(f);
    return true;
}

// ─── Input prompt helper ─────────────────────────────────────────────────────
// Shows a one-line prompt in the given window, reads a string from the user.
// Returns the entered string, or "" if the user pressed Esc/Ctrl-C.
static string prompt_input(WINDOW* status_w, const char* label)
{
    werase(status_w);
    wattron(status_w, COLOR_PAIR(C_FOCUS_HDR) | A_BOLD);
    mvwprintw(status_w, 0, 0, " %s", label);
    wattroff(status_w, COLOR_PAIR(C_FOCUS_HDR) | A_BOLD);
    wrefresh(status_w);

    curs_set(1);
    echo();

    string buf;
    int ch;
    while ((ch = wgetch(status_w)) != '\n' && ch != '\r' && ch != KEY_ENTER) {
        if (ch == 27 || ch == ctrl('c')) { buf.clear(); break; }
        if ((ch == KEY_BACKSPACE || ch == 127 || ch == '\b') && !buf.empty()) {
            buf.pop_back();
            // Redraw input line
            werase(status_w);
            wattron(status_w, COLOR_PAIR(C_FOCUS_HDR) | A_BOLD);
            mvwprintw(status_w, 0, 0, " %s%s", label, buf.c_str());
            wattroff(status_w, COLOR_PAIR(C_FOCUS_HDR) | A_BOLD);
            wrefresh(status_w);
        } else if (ch >= 32 && ch < 127) {
            buf += (char)ch;
        }
    }

    noecho();
    curs_set(0);
    return buf;
}

// ─── Main UI loop ────────────────────────────────────────────────────────────

static void run(ParseState& ps, const string& filename, bool live, LiveReader* reader)
{
    UI ui;
    ui.ps   = &ps;
    ui.live = live;
    ui.rebuild();

    setlocale(LC_ALL, "");  // enable UTF-8 so Unicode box/arrow chars render correctly
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    set_escdelay(50);
    init_colors();

    // In live mode use a 50 ms input timeout so we can poll for new data
    // without starving keyboard input.
    if (live) wtimeout(stdscr, 50);

    Windows win = make_windows(threads_pane_h((int)ps.threads.size()), ui.show_timeline);
    int win_thread_count = (int)ps.threads.size(); // track for dynamic resize

    auto rebuild_windows = [&]() {
        int th_h = threads_pane_h((int)ps.threads.size());
        free_windows(win);
        clear(); refresh();
        win = make_windows(th_h, ui.show_timeline);
        win_thread_count = (int)ps.threads.size();
    };

    auto redraw = [&]() {
        int th_h = threads_pane_h((int)ps.threads.size());
        int lw = list_pane_w();
        int hh = header_h(ui.show_timeline);
        attron(COLOR_PAIR(C_DIM) | A_DIM);
        mvvline(hh, lw, ACS_VLINE, body_height(th_h, ui.show_timeline));
        attroff(COLOR_PAIR(C_DIM) | A_DIM);
        refresh();
        draw_header(win.header, ui, filename, reader);
        if (ui.hist_mode)
            draw_histogram(win.list, ui);
        else
            draw_list(win.list, ui);
        draw_detail(win.detail, ui);
        draw_threads(win.threads, ui);
        draw_status(win.status, ui);
    };

    // Transient one-line message shown in place of status bar until next keypress.
    string status_msg;

    auto redraw_with_msg = [&](const string& msg) {
        redraw();
        werase(win.status);
        wattron(win.status, COLOR_PAIR(C_ALLOC) | A_BOLD);
        mvwprintw(win.status, 0, 0, " %s", msg.c_str());
        wattroff(win.status, COLOR_PAIR(C_ALLOC) | A_BOLD);
        wrefresh(win.status);
        status_msg = msg;
    };

    // Helper: scroll list so `selected` is visible.
    auto ensure_visible = [&]() {
        int h, w2; getmaxyx(win.list, h, w2); (void)w2;
        int page = h - 2;
        if (ui.selected < ui.list_top)
            ui.list_top = ui.selected;
        if (ui.selected >= ui.list_top + page)
            ui.list_top = ui.selected - page + 1;
        if (ui.list_top < 0) ui.list_top = 0;
    };

    redraw();

    while (true) {
        int ch = wgetch(stdscr);

        // ── Live polling ─────────────────────────────────────────────────
        if (ch == ERR && live && reader) {
            int   new_lines = 0;
            bool  reset     = reader->poll_new(ps, new_lines);

            if (reset) {
                // Application restarted — full UI reset.
                ui.rebuild(true);
                rebuild_windows();
                redraw();
            } else if (new_lines > 0) {
                int prev_vis = (int)ui.visible.size();
                ui.rebuild(false);   // keep scroll position

                if (ui.auto_follow && (int)ui.visible.size() > prev_vis) {
                    ui.selected = max(0, (int)ui.visible.size() - 1);
                    ensure_visible();
                }
                // Rebuild windows if a new thread appeared.
                if ((int)ps.threads.size() != win_thread_count)
                    rebuild_windows();
                redraw();
            }
            continue;
        }

        if (ch == 'q' || ch == 27) break;

        // Resize
        if (ch == KEY_RESIZE) {
            rebuild_windows();
            redraw();
            continue;
        }

        // Pane switch: Tab / h / l cycles  list → detail → threads → list
        //             h / left  : backward;  l / right / Tab : forward
        if (ch == '\t' || ch == KEY_LEFT || ch == KEY_RIGHT ||
            ch == 'h'  || ch == 'l') {
            bool fwd = (ch == '\t' || ch == KEY_RIGHT || ch == 'l');
            if (!ui.focus_detail && !ui.focus_threads) {
                // list → detail (fwd) or list → threads (bwd)
                if (fwd) { ui.focus_detail = true;  ui.focus_threads = false; }
                else     { ui.focus_detail = false; ui.focus_threads = true;  }
            } else if (ui.focus_detail) {
                // detail → threads (fwd) or detail → list (bwd)
                if (fwd) { ui.focus_detail = false; ui.focus_threads = true;  }
                else     { ui.focus_detail = false; ui.focus_threads = false; }
            } else {
                // threads → list (fwd) or threads → detail (bwd)
                if (fwd) { ui.focus_detail = false; ui.focus_threads = false; }
                else     { ui.focus_detail = true;  ui.focus_threads = false; }
            }
            redraw();
            continue;
        }

        // Toggle auto-follow (live mode only)
        if (ch == 'F' && live) {
            ui.auto_follow = !ui.auto_follow;
            redraw();
            continue;
        }

        // Toggle addr2line source-line resolution in the detail pane.
        if (ch == 'L') {
            ui.resolve_lines = !ui.resolve_lines;
            ui.detail_top    = 0;
            goto next_draw;
        }

        // Thread filter — cycles in the same order as the Thread Summary pane.
        // 't' = forward, 'T' = backward.
        if (ch == 't' || ch == 'T') {
            if (!ps.threads.empty()) {
                // Use the same sorted order as the Thread Summary pane.
                vector<const ThreadInfo*> sv = sorted_threads(ui);
                vector<int> order;
                order.reserve(sv.size());
                for (auto* t : sv) order.push_back(t->tid);

                if (ch == 't') {
                    // Forward: all → order[0] → order[1] → … → all
                    if (ui.tid_filter == -1) {
                        ui.tid_filter = order[0];
                    } else {
                        auto it = std::find(order.begin(), order.end(), ui.tid_filter);
                        if (it == order.end() || std::next(it) == order.end())
                            ui.tid_filter = -1;   // wrap to "all"
                        else
                            ui.tid_filter = *std::next(it);
                    }
                } else {
                    // Backward: all → order[last] → order[last-1] → … → all
                    if (ui.tid_filter == -1) {
                        ui.tid_filter = order.back();
                    } else {
                        auto it = std::find(order.begin(), order.end(), ui.tid_filter);
                        if (it == order.end() || it == order.begin())
                            ui.tid_filter = -1;   // wrap to "all"
                        else
                            ui.tid_filter = *std::prev(it);
                    }
                }

                // Pause auto-follow so the user can browse the filtered view.
                if (live) ui.auto_follow = false;
                ui.rebuild();

                // Scroll the Thread Summary pane so the selected thread is visible.
                // Must happen AFTER rebuild() since rebuild(reset_scroll=true) resets threads_top.
                if (ui.tid_filter != -1) {
                    int pos = 0;
                    for (size_t i = 0; i < order.size(); i++) {
                        if (order[i] == ui.tid_filter) { pos = (int)i; break; }
                    }
                    int th_page = threads_pane_h((int)ps.threads.size()) - THREADS_HDR;
                    if (pos < ui.threads_top)
                        ui.threads_top = pos;
                    if (pos >= ui.threads_top + th_page)
                        ui.threads_top = pos - th_page + 1;
                }
            }
            goto next_draw;
        }

        // Lifetime filter: 'a' (age) prompts for minimum age in seconds
        if (ch == 'a') {
            string s = prompt_input(win.status, "Min lifetime (seconds, 0=off): ");
            if (!s.empty()) {
                double secs = atof(s.c_str());
                ui.lifetime_min_us = (uint64_t)(secs * 1e6);
                ui.rebuild();
            }
            goto next_draw;
        }

        // Symbol / text search: '/' prompts for substring, empty clears filter
        if (ch == '/') {
            string s = prompt_input(win.status, "Search (frames/thread/op, empty=clear): ");
            ui.search_str = s;
            ui.rebuild();
            goto next_draw;
        }

        // Mark / delta mode: 'm' sets mark at current max timestamp and shows
        // only allocations that appear after it.  Press again to clear.
        if (ch == 'm') {
            if (ui.delta_mode) {
                // Clear mark
                ui.delta_mode = false;
                ui.mark_ts    = 0;
            } else {
                // Mark at selected record's timestamp; fall back to max_ts if nothing selected
                uint64_t ts = 0;
                if (ui.has_selection()) {
                    ts = ui.selected_ts();
                } else {
                    for (const auto& r : ui.ps->records) {
                        ts = max(ts, r.timestamp_us);
                        if (r.freed) ts = max(ts, r.free_timestamp_us);
                    }
                }
                ui.mark_ts    = ts;
                ui.delta_mode = true;
            }
            ui.rebuild();
            goto next_draw;
        }

        // Export visible records to a text file
        if (ch == 'W') {
            string path = prompt_input(win.status, "Export to file: ");
            if (!path.empty()) {
                if (export_to_file(path, ui, filename))
                    redraw_with_msg("Exported " + std::to_string(ui.group_mode ? ui.groups.size() : ui.visible.size()) + " records to " + path);
                else
                    redraw_with_msg("ERROR: could not write to " + path);
            }
            goto next_draw;
        }

        // Toggle group mode (folds visible records by stack trace)
        if (ch == '\\') {
            ui.group_mode = !ui.group_mode;
            ui.selected   = 0;
            ui.list_top   = 0;
            ui.detail_top = 0;
            if (ui.group_mode) ui.rebuild_groups();
            goto next_draw;
        }

        // Toggle timeline sparkline row in header
        if (ch == 'Z') {
            ui.show_timeline = !ui.show_timeline;
            rebuild_windows();
            goto next_draw;
        }

        // Toggle size histogram in list pane
        if (ch == 'H') {
            ui.hist_mode = !ui.hist_mode;
            goto next_draw;
        }

        // ── Persistent markers & range diff ──────────────────────────────

        // M: add marker at selected record's timestamp
        if (ch == 'M') {
            if (ui.has_selection()) {
                uint64_t ts = ui.selected_ts();
                int id = ui.add_marker(ts);
                if (id > 0)
                    redraw_with_msg("Marker M" + std::to_string(id) + " added at " + fmt_time_ms(ts));
                else
                    redraw_with_msg("Max 9 markers reached — press D to remove one");
            } else {
                redraw_with_msg("No record selected");
            }
            goto next_draw;
        }

        // D: delete nearest marker to selected record
        if (ch == 'D') {
            uint64_t ts = ui.selected_ts();
            if (!ui.markers.empty()) {
                ui.remove_nearest_marker(ts > 0 ? ts : ui.markers[0].ts);
                redraw_with_msg("Marker removed (" + std::to_string(ui.markers.size()) + " remaining)");
            }
            goto next_draw;
        }

        // [: set range start (snap to nearest marker ≤ selected ts, or selected ts)
        if (ch == '[') {
            if (ui.has_selection()) {
                ui.range_start = ui.snap_left(ui.selected_ts());
                if (ui.range_active && ui.range_end < ui.range_start)
                    ui.range_end = ui.range_start;
                ui.range_active = (ui.range_end > ui.range_start);
                ui.rebuild();
                redraw_with_msg("Range start: " + fmt_time_ms(ui.range_start));
            }
            goto next_draw;
        }

        if (ch == ']') {
            if (ui.has_selection()) {
                ui.range_end = ui.snap_right(ui.selected_ts());
                if (ui.range_end < ui.range_start) ui.range_start = ui.range_end;
                ui.range_active = (ui.range_end > ui.range_start);
                ui.rebuild();
                redraw_with_msg("Range end: " + fmt_time_ms(ui.range_end));
            }
            goto next_draw;
        }

        // R: clear range filter (keeps markers)
        if (ch == 'R') {
            ui.range_active = false;
            ui.range_start  = 0;
            ui.range_end    = 0;
            ui.rebuild();
            redraw_with_msg("Range cleared");
            goto next_draw;
        }

        if (!ui.focus_detail && !ui.focus_threads) {
            // ── List navigation ──────────────────────────────────────────
            int h, w2; getmaxyx(win.list, h, w2); (void)w2;
            int page = h - 2;
            int list_size = ui.group_mode ? (int)ui.groups.size() : (int)ui.visible.size();

            if ((ch == KEY_UP || ch == 'k') && ui.selected > 0) {
                ui.selected--;
                ui.detail_top = 0;
                // Manual navigation disables auto-follow so the view doesn't
                // jump away while the user is browsing.
                if (live) ui.auto_follow = false;
                ensure_visible();
            } else if ((ch == KEY_DOWN || ch == 'j') && ui.selected < list_size - 1) {
                ui.selected++;
                ui.detail_top = 0;
                if (live) ui.auto_follow = false;
                ensure_visible();
            } else if (ch == KEY_PPAGE || ch == ctrl('b')) {
                ui.selected = max(0, ui.selected - page);
                ui.list_top = max(0, ui.list_top - page);
                ui.detail_top = 0;
                if (live) ui.auto_follow = false;
            } else if (ch == KEY_NPAGE || ch == ctrl('f')) {
                ui.selected = min(list_size - 1, ui.selected + page);
                ui.list_top = min(max(0, list_size - page), ui.list_top + page);
                ui.detail_top = 0;
                if (live) ui.auto_follow = false;
            } else if (ch == KEY_HOME || ch == 'g') {
                ui.selected = 0; ui.list_top = 0; ui.detail_top = 0;
                if (live) ui.auto_follow = false;
            } else if (ch == KEY_END || ch == 'G') {
                ui.selected = max(0, list_size - 1);
                ui.list_top = max(0, ui.selected - page + 1);
                ui.detail_top = 0;
                // G = jump to bottom → re-enable auto-follow
                if (live) ui.auto_follow = true;
            } else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
                // Enter in normal mode: jump to the group containing this record.
                // Enter in group mode: expand to normal view showing example record.
                if (!ui.group_mode) {
                    const AllocRecord* r = ui.current();
                    if (r) ui.jump_to_group(*r);
                } else {
                    // Ungroup and land on the example record of the selected group
                    size_t example = ui.groups.empty() ? 0
                                   : ui.groups[min(ui.selected,(int)ui.groups.size()-1)].example_idx;
                    ui.group_mode = false;
                    ui.selected   = 0;
                    ui.list_top   = 0;
                    ui.detail_top = 0;
                    // Find example in visible[]
                    for (int i = 0; i < (int)ui.visible.size(); i++) {
                        if (ui.visible[i] == example) {
                            ui.selected = i;
                            ui.list_top = max(0, i - 3);
                            break;
                        }
                    }
                }
            } else if (ch == 'f') {
                ui.filter = (FilterMode)((ui.filter + 1) % F_COUNT);
                ui.rebuild();
            } else if (ch == 's') {
                if (ui.group_mode) {
                    // Cycle group sort: Total → Count → Rate → Total
                    if      (ui.group_sort == GS_TOTAL) ui.group_sort = GS_COUNT;
                    else if (ui.group_sort == GS_COUNT) ui.group_sort = GS_RATE;
                    else                                ui.group_sort = GS_TOTAL;
                    ui.group_sort_rev = false;
                    ui.rebuild_groups();
                } else {
                    SortMode next = (SortMode)((ui.sort + 1) % S_COUNT);
                    if (next == ui.sort)
                        ui.sort_rev = !ui.sort_rev;
                    else {
                        ui.sort_rev = false;
                        ui.sort = next;
                    }
                    ui.rebuild();
                }
            } else if (ch == 'S') {
                if (ui.group_mode) {
                    ui.group_sort_rev = !ui.group_sort_rev;
                    ui.rebuild_groups();
                } else {
                    ui.sort_rev = !ui.sort_rev;
                    ui.rebuild();
                }
            } else if (ch == '1') {
                ui.sort_rev = (ui.sort == S_TIME) ? !ui.sort_rev : false;
                ui.sort = S_TIME; ui.rebuild();
            } else if (ch == '2') {
                ui.sort_rev = (ui.sort == S_SIZE) ? !ui.sort_rev : false;
                ui.sort = S_SIZE; ui.rebuild();
            } else if (ch == '3') {
                ui.sort_rev = (ui.sort == S_THREAD) ? !ui.sort_rev : false;
                ui.sort = S_THREAD; ui.rebuild();
            }
        } else if (ui.focus_detail) {
            // ── Detail pane navigation ───────────────────────────────────
            int h, w2; getmaxyx(win.detail, h, w2); (void)w2;
            int page = h - 2;
            if      (ch == KEY_UP   || ch == 'k') ui.detail_top = max(0, ui.detail_top - 1);
            else if (ch == KEY_DOWN || ch == 'j') ui.detail_top++;
            else if (ch == KEY_PPAGE || ch == ctrl('b')) ui.detail_top = max(0, ui.detail_top - page);
            else if (ch == KEY_NPAGE || ch == ctrl('f')) ui.detail_top += page;
            else if (ch == KEY_HOME || ch == 'g') ui.detail_top = 0;
            else if (ch == 'G') ui.detail_top = INT_MAX;  // clamped on draw
        } else {
            // ── Thread pane navigation ───────────────────────────────────
            int page  = threads_pane_h((int)ps.threads.size()) - THREADS_HDR;
            int total = (int)ps.threads.size();
            int max_top = max(0, total - page);
            if      (ch == KEY_UP   || ch == 'k') ui.threads_top = max(0, ui.threads_top - 1);
            else if (ch == KEY_DOWN || ch == 'j') ui.threads_top = min(max_top, ui.threads_top + 1);
            else if (ch == KEY_PPAGE || ch == ctrl('b')) ui.threads_top = max(0, ui.threads_top - page);
            else if (ch == KEY_NPAGE || ch == ctrl('f')) ui.threads_top = min(max_top, ui.threads_top + page);
            else if (ch == KEY_HOME || ch == 'g') ui.threads_top = 0;
            else if (ch == 'G') ui.threads_top = max_top;
            else if (ch == 's') {
                // Cycle sort column forward: Net → Name → Allocated → Freed → Leaks → Net
                const ThreadSortMode cycle[] = { TS_NAME, TS_TID, TS_ALLOC, TS_FREED, TS_NET, TS_LEAKS };
                constexpr int N = (int)(sizeof(cycle)/sizeof(cycle[0]));
                for (int i = 0; i < N; i++) {
                    if (cycle[i] == ui.thread_sort) {
                        ui.thread_sort = cycle[(i + 1) % N];
                        ui.thread_sort_rev = false;
                        break;
                    }
                }
            } else if (ch == 'S') {
                ui.thread_sort_rev = !ui.thread_sort_rev;
            }
        }

        next_draw:
        redraw();
    }

    free_windows(win);
    endwin();
}

// ─── Entry point ─────────────────────────────────────────────────────────────

// Parse "[host]:port" or ":port".  Returns true if the string looks like a
// TCP address.  `host` defaults to "127.0.0.1" when omitted.
static bool parse_tcp_addr(const string& s, string& host, int& port)
{
    // Must contain a colon separating host from port.
    auto colon = s.rfind(':');
    if (colon == string::npos) return false;
    string port_str = s.substr(colon + 1);
    if (port_str.empty()) return false;
    for (char c : port_str) if (!isdigit((unsigned char)c)) return false;
    port = atoi(port_str.c_str());
    if (port <= 0 || port > 65535) return false;
    host = s.substr(0, colon);
    if (host.empty()) host = "127.0.0.1";
    return true;
}

int main(int argc, char* argv[])
{
    bool   live = false;
    string path;
    string save_path;   // --save / -o <file>

    bool dump_threads = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f") || !strcmp(argv[i], "--follow"))
            live = true;
        else if (!strcmp(argv[i], "--dump-threads"))
            dump_threads = true;
        else if ((!strcmp(argv[i], "--save") || !strcmp(argv[i], "-o")) && i+1 < argc)
            save_path = argv[++i];
        else if (argv[i][0] != '-' || !strcmp(argv[i], "-"))
            path = argv[i];
        else {
            // Also accept ":port" as a positional argument even though it
            // starts with '-' (the host part is empty → looks like a flag).
            string h; int p;
            if (parse_tcp_addr(argv[i], h, p))
                path = argv[i];
        }
    }

    if (path.empty()) {
        fprintf(stderr,
                "Usage: %s [-f] <memtrack.log>\n"
                "       %s [-f] -              (stdin)\n"
                "       %s :PORT [-o save.log] (connect to memtrack TCP server, localhost)\n"
                "       %s HOST:PORT [-o save.log]\n"
                "\n"
                "  -f / --follow        Live file/stdin mode (TCP is always live).\n"
                "  -o / --save <file>   Save TCP stream to file while viewing.\n",
                argv[0], argv[0], argv[0], argv[0]);
        return 1;
    }

    // ── TCP client mode ───────────────────────────────────────────────────────
    string tcp_host; int tcp_port = 0;
    if (parse_tcp_addr(path, tcp_host, tcp_port)) {
        fprintf(stderr, "[memview] connecting to %s:%d ...\n",
                tcp_host.c_str(), tcp_port);
        LiveReader reader;
        if (!reader.connect_tcp(tcp_host, tcp_port)) {
            fprintf(stderr, "[memview] connection failed: %s\n", strerror(errno));
            return 1;
        }
        if (!save_path.empty()) {
            if (!reader.open_save(save_path)) {
                fprintf(stderr, "[memview] warning: cannot open save file '%s': %s\n",
                        save_path.c_str(), strerror(errno));
            } else {
                fprintf(stderr, "[memview] saving stream to '%s'\n", save_path.c_str());
            }
        }
        fprintf(stderr, "[memview] connected\n");
        ParseState ps;
        live = true;
        run(ps, path, live, &reader);
        return 0;
    }

    // ── File / stdin mode ─────────────────────────────────────────────────────
    long initial_pos = 0;
    string real_path = (path == "-") ? "/dev/stdin" : path;
    ParseState ps = parse_file(real_path, &initial_pos);
    // Non-live mode: the file is complete — mark remaining unfreed allocations
    // as leaks (memtrack does not emit LEAK/SUMMARY lines).
    if (!live) finalize_leaks(ps);

    if (!live && ps.records.empty()) {
        fprintf(stderr, "No memtrack records found in '%s'.\n"
                        "Make sure MEMTRACK_MIN_SIZE is not filtering everything out.\n",
                path.c_str());
        return 1;
    }

    // Diagnostic mode: dump thread summary and exit (no ncurses).
    if (dump_threads) {
        printf("Records: %zu  |  Threads: %zu\n\n",
               ps.records.size(), ps.threads.size());
        printf("  %-20s  %8s  %12s  %12s  %12s\n",
               "Thread", "TID", "Allocated", "Freed", "Net (live)");
        printf("  %s\n", string(72, '-').c_str());
        for (auto& t : ps.threads) {
            printf("  %-20s  %8d  %12s  %12s  %12s\n",
                   t.name.c_str(), t.tid,
                   fmt_size(t.allocated).c_str(),
                   fmt_size(t.freed).c_str(),
                   fmt_size(t.net()).c_str());
        }
        return 0;
    }

    // In live mode, open the reader from where the initial parse left off.
    LiveReader reader;
    if (live) reader.open(real_path, initial_pos);

    run(ps, path, live, live ? &reader : nullptr);
    return 0;
}
