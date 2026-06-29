/*
 * memview.cpp – ncurses viewer for memtrack log output
 *
 * Build:  g++ -O2 -std=c++17 -o memview memview.cpp -lncurses
 * Usage:  ./memview <log>
 *         ./memview -              (read from stdin)
 *         ./memview -f <log>       (live follow mode)
 *         ./memview -f -           (live follow stdin)
 *
 * Keys:
 *   ↑ ↓  PgUp PgDn  Home End   Navigate allocation list
 *   Tab / ← →                  Switch focus: list ↔ detail pane
 *   f                           Cycle filter: All → Leaks → Active → Freed
 *   t                           Cycle thread filter
 *   F                           Toggle auto-follow (live mode)
 *   q / Esc                     Quit
 */

#include <ncurses.h>
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

using std::vector;
using std::string;
using std::unordered_map;
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
    size_t total_bytes    = 0;
    size_t leak_count     = 0;
    size_t leak_bytes     = 0;
};

// ─── Parser ──────────────────────────────────────────────────────────────────

struct ParseState {
    vector<AllocRecord>          records;
    unordered_map<uintptr_t, size_t> ptr_idx;   // live ptr → records index

    vector<ThreadInfo>           threads;
    unordered_map<int, size_t>   tid_idx;        // tid → threads index

    enum class Last { NONE, ALLOC, FREE, LEAK } last = Last::NONE;
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
           !strcmp(op,"realloc") || !strcmp(op,"new") || !strcmp(op,"new[]");
}

static bool is_free_op(const char* op)
{
    return !strcmp(op,"free") || !strcmp(op,"delete") || !strcmp(op,"delete[]");
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
        else if ((st.last == ParseState::Last::FREE || st.last == ParseState::Last::LEAK)
                 && st.last_rec < st.records.size())
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
        st.thread(tid, tname).total_bytes = total;
        return;
    }

    // ── SUMMARY ───────────────────────────────────────────────────────────
    if (!strcmp(op, "SUMMARY")) {
        size_t cnt = 0, bytes = 0;
        if (sscanf(after, "SUMMARY %zu unfreed allocation(s), %zu bytes leaked",
                   &cnt, &bytes) == 2) {
            auto& th = st.thread(tid, tname);
            th.leak_count = cnt;
            th.leak_bytes = bytes;
        }
        return;
    }

    // ── LEAK (from exit report) ───────────────────────────────────────────
    if (!strcmp(op, "LEAK")) {
        char   lop[32] = {};
        size_t sz      = 0;
        void*  ptr_raw = nullptr;
        unsigned long long ts_ull = 0;
        sscanf(after, "LEAK ts=%llu %31s size=%zu ptr=%p", &ts_ull, lop, &sz, &ptr_raw);
        uintptr_t ptr = (uintptr_t)ptr_raw;
        auto it = st.ptr_idx.find(ptr);
        if (it != st.ptr_idx.end()) {
            st.records[it->second].is_leak = true;
            st.last     = ParseState::Last::LEAK;
            st.last_rec = it->second;
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

        AllocRecord rec;
        rec.ptr          = ptr;
        rec.tid          = tid;
        rec.thread_name  = tname;
        rec.op           = op;
        rec.size         = sz;
        rec.total        = total;
        rec.timestamp_us = (uint64_t)ts_ull;

        size_t idx = st.records.size();
        st.ptr_idx[ptr] = idx;
        st.records.push_back(std::move(rec));
        st.last     = ParseState::Last::ALLOC;
        st.last_rec = idx;
        return;
    }

    // ── Free ──────────────────────────────────────────────────────────────
    if (is_free_op(op)) {
        size_t sz      = 0;
        void*  ptr_raw = nullptr;
        unsigned long long ts_ull = 0;
        sscanf(after, "%*s ts=%llu size=%zu ptr=%p", &ts_ull, &sz, &ptr_raw);
        uintptr_t ptr = (uintptr_t)ptr_raw;

        auto it = st.ptr_idx.find(ptr);
        if (it != st.ptr_idx.end()) {
            auto& rec              = st.records[it->second];
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

static ParseState parse_file(const string& path, long* out_pos = nullptr)
{
    ParseState st;
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
    FILE*  fp       = nullptr;
    long   pos      = 0;
    bool   is_stdin = false;

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
            fcntl(fileno(stdin), F_SETFL, flags | O_NONBLOCK);
        } else {
            fp  = fopen(p.c_str(), "r");
            pos = initial_pos;
            if (fp) fseek(fp, pos, SEEK_SET);
        }
    }

    // Read any newly appended lines into `st`.
    // Returns true if the file was truncated (application restarted) — the
    // caller should treat this as a full reset of the display.
    // `new_count` is set to the number of new lines parsed this call.
    bool poll_new(ParseState& st, int& new_count)
    {
        new_count = 0;

        if (!fp && !is_stdin) {
            // File may not have appeared yet — try again next poll.
            fp = fopen(path.c_str(), "r");
            if (!fp) return false;
            pos = 0;
        }

        if (!is_stdin) {
            // Detect truncation: current end is before our read position.
            long saved = ftell(fp);
            fseek(fp, 0, SEEK_END);
            long cur_end = ftell(fp);
            fseek(fp, saved, SEEK_SET);

            if (cur_end < pos) {
                // File was truncated — clear state and restart from top.
                st  = ParseState{};
                pos = 0;
                fseek(fp, 0, SEEK_SET);
                clearerr(fp);
                return true;   // signal: UI should reset
            }
            if (cur_end == pos) return false;  // nothing new
        } else {
            // Non-blocking stdin: check readability with poll.
            struct pollfd pfd = { fileno(stdin), POLLIN, 0 };
            if (::poll(&pfd, 1, 0) <= 0) return false;
        }

        char line_buf[4096];
        while (fgets(line_buf, sizeof(line_buf), fp)) {
            size_t len = strlen(line_buf);
            if (len > 0 && line_buf[len - 1] == '\n') line_buf[len - 1] = '\0';
            parse_line(line_buf, st);
            new_count++;
        }

        if (!is_stdin) {
            long p = ftell(fp);
            if (p >= 0) pos = p;
            clearerr(fp);   // clear EOF so next fgets works
        }

        return false;
    }
};

// ─── Colour pairs ────────────────────────────────────────────────────────────

enum {
    C_NORMAL = 1,  // white / black
    C_HEADER = 2,  // white / blue   (panel headers)
    C_ALLOC  = 3,  // green / black  (allocations)
    C_FREE   = 4,  // yellow / black (freed)
    C_LEAK   = 5,  // red / black    (leaks)
    C_SEL    = 6,  // black / white  (selected row)
    C_DIM    = 7,  // dim white      (stack frames, hints)
    C_THREAD = 8,  // cyan / black   (thread info)
};

static void init_colors()
{
    start_color();
    use_default_colors();
    init_pair(C_NORMAL, COLOR_WHITE,  COLOR_BLACK);
    init_pair(C_HEADER, COLOR_WHITE,  COLOR_BLUE);
    init_pair(C_ALLOC,  COLOR_GREEN,  COLOR_BLACK);
    init_pair(C_FREE,   COLOR_YELLOW, COLOR_BLACK);
    init_pair(C_LEAK,   COLOR_RED,    COLOR_BLACK);
    init_pair(C_SEL,    COLOR_BLACK,  COLOR_WHITE);
    init_pair(C_DIM,    COLOR_WHITE,  COLOR_BLACK);
    init_pair(C_THREAD, COLOR_CYAN,   COLOR_BLACK);
}

// ─── UI state ────────────────────────────────────────────────────────────────

enum FilterMode { F_ALL, F_LEAKS, F_ACTIVE, F_FREED, F_COUNT };
static const char* filter_label[] = { "All", "Leaks", "Active", "Freed" };

enum SortMode  { S_TIME, S_SIZE, S_THREAD, S_COUNT };
static const char* sort_label[]   = { "Time", "Size↕", "Thread" };

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
    bool           live          = false;
    bool           auto_follow   = true; // scroll to newest in live mode

    // reset_scroll=true: reset selection and scroll (filter changes, full reset).
    // reset_scroll=false: preserve selection and scroll (incremental data update).
    void rebuild(bool reset_scroll = true)
    {
        visible.clear();
        for (size_t i = 0; i < ps->records.size(); i++) {
            const auto& r = ps->records[i];
            if (tid_filter != -1 && r.tid != tid_filter) continue;
            if (filter == F_LEAKS  && r.freed && !r.is_leak) continue;
            if (filter == F_ACTIVE && r.freed)               continue;
            if (filter == F_FREED  && !r.freed)              continue;
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
        } else {
            selected = min(selected, (int)visible.size() - 1);
            if (selected < 0) selected = 0;
        }
    }

    const AllocRecord* current() const {
        if (visible.empty()) return nullptr;
        return &ps->records[visible[selected]];
    }
};

// ─── Helpers ─────────────────────────────────────────────────────────────────

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
    if (ms >= 60000.0)
        snprintf(buf, sizeof(buf), "%.3fs",  ms / 1000.0);
    else if (ms >= 1000.0)
        snprintf(buf, sizeof(buf), "%.3fs",  ms / 1000.0);
    else
        snprintf(buf, sizeof(buf), "%.3fms", ms);
    return buf;
}

static void hline_to_eol(WINDOW* w, int y, int col_pair)
{
    int h, width; getmaxyx(w, h, width); (void)h;
    int x = getcurx(w);
    wattron(w, COLOR_PAIR(col_pair));
    while (x < width) { mvwaddch(w, y, x++, ' '); }
    wattroff(w, COLOR_PAIR(col_pair));
}

// ─── Draw: header ────────────────────────────────────────────────────────────

static void draw_header(WINDOW* w, const UI& ui, const string& filename)
{
    int h, width; getmaxyx(w, h, width); (void)h;
    size_t leaks = 0, leak_bytes = 0;
    for (const auto& r : ui.ps->records)
        if (!r.freed || r.is_leak) { leaks++; leak_bytes += r.size; }

    wattron(w, COLOR_PAIR(C_HEADER) | A_BOLD);

    // Row 0: title + live indicator
    if (ui.live) {
        // Blinking green dot for live, dimmed when auto-follow is paused
        wattroff(w, COLOR_PAIR(C_HEADER) | A_BOLD);
        if (ui.auto_follow)
            wattron(w, COLOR_PAIR(C_ALLOC) | A_BOLD);
        else
            wattron(w, COLOR_PAIR(C_FREE)  | A_BOLD);
        mvwaddstr(w, 0, 0, ui.auto_follow ? " ● LIVE " : " ◌ LIVE ");
        wattroff(w, A_BOLD | COLOR_PAIR(C_ALLOC) | COLOR_PAIR(C_FREE));
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
    if (ui.live) {
        wprintw(w, "  │  Follow: %s", ui.auto_follow ? "ON [F]" : "OFF[F]");
    }
    hline_to_eol(w, 1, C_HEADER);
    wattroff(w, COLOR_PAIR(C_HEADER) | A_BOLD);
    wrefresh(w);
}

// ─── Draw: allocation list (left pane) ───────────────────────────────────────

static void draw_list(WINDOW* w, const UI& ui)
{
    int rows, cols; getmaxyx(w, rows, cols);
    werase(w);

    // Sub-header — underline active sort column
    werase(w);
    wattron(w, COLOR_PAIR(C_HEADER) | A_BOLD);
    mvwaddstr(w, 0, 0, " St. ");
    waddstr(w, "Pointer            ");
    waddstr(w, "Op         ");

    auto col = [&](const char* label, SortMode sm) {
        if (ui.sort == sm) wattron(w, A_UNDERLINE);
        waddstr(w, label);
        if (ui.sort == sm) wattroff(w, A_UNDERLINE);
    };
    col("Time        ", S_TIME);
    col("Size       ", S_SIZE);
    col("Thread",       S_THREAD);

    hline_to_eol(w, 0, C_HEADER);
    wattroff(w, COLOR_PAIR(C_HEADER) | A_BOLD);

    int list_rows = rows - 2;   // -1 header -1 counter
    for (int row = 0; row < list_rows; row++) {
        int idx = ui.list_top + row;
        if (idx >= (int)ui.visible.size()) break;

        const auto& r   = ui.ps->records[ui.visible[idx]];
        bool sel        = (idx == ui.selected);
        char status     = r.is_leak ? 'L' : (r.freed ? 'F' : 'A');

        char ptr_buf[24];
        snprintf(ptr_buf, sizeof(ptr_buf), "0x%014" PRIxPTR, r.ptr);

        string sz = fmt_size(r.size);

        int cp = r.is_leak ? C_LEAK : (r.freed ? C_FREE : C_ALLOC);
        if (sel && !ui.focus_detail) cp = C_SEL;
        attr_t attr = (sel && !ui.focus_detail) ? A_BOLD : 0;
        if (r.is_leak) attr |= A_BOLD;

        wattron(w, COLOR_PAIR(cp) | attr);
        mvwprintw(w, row + 1, 0, " [%c] %-18s %-9s %-12s %-9s %-15s",
                  status, ptr_buf, r.op.c_str(),
                  fmt_time_ms(r.timestamp_us).c_str(),
                  sz.c_str(), r.thread_name.c_str());
        int x = getcurx(w);
        while (x < cols) { waddch(w, ' '); x++; }
        wattroff(w, COLOR_PAIR(cp) | attr);
    }

    // Counter
    wattron(w, COLOR_PAIR(C_DIM) | A_DIM);
    mvwprintw(w, rows - 1, 0, " %d / %d  [L]eak [A]ctive [F]reed",
              ui.visible.empty() ? 0 : ui.selected + 1,
              (int)ui.visible.size());
    hline_to_eol(w, rows - 1, C_DIM);
    wattroff(w, COLOR_PAIR(C_DIM) | A_DIM);

    wrefresh(w);
}

// ─── Draw: detail pane (right) ───────────────────────────────────────────────

struct DLine { string text; int cp; int attr; };

static vector<DLine> build_detail(const AllocRecord& r)
{
    vector<DLine> d;
    auto add = [&](string t, int cp = C_NORMAL, int attr = 0) {
        d.push_back({std::move(t), cp, attr});
    };
    auto sep = [&](const char* title, int cp = C_HEADER) {
        add(string("── ") + title + " ", cp, A_BOLD);
    };

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
        for (int i = 0; i < (int)r.frames.size(); i++)
            add("    #" + std::to_string(i) + "  " + r.frames[i], C_DIM, A_DIM);
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
            for (int i = 0; i < (int)r.free_frames.size(); i++)
                add("    #" + std::to_string(i) + "  " + r.free_frames[i], C_DIM, A_DIM);
        }
    }
    return d;
}

static void draw_detail(WINDOW* w, const UI& ui)
{
    int rows, cols; getmaxyx(w, rows, cols);
    werase(w);

    // Sub-header
    bool focused = ui.focus_detail;
    wattron(w, COLOR_PAIR(C_HEADER) | A_BOLD);
    mvwprintw(w, 0, 0, " Detail%s", focused ? " [↑↓ scroll]" : "");
    hline_to_eol(w, 0, C_HEADER);
    wattroff(w, COLOR_PAIR(C_HEADER) | A_BOLD);

    const AllocRecord* r = ui.current();
    if (!r) {
        wattron(w, COLOR_PAIR(C_DIM) | A_DIM);
        mvwprintw(w, 2, 2, "(no allocations)");
        wattroff(w, COLOR_PAIR(C_DIM) | A_DIM);
        wrefresh(w);
        return;
    }

    auto detail  = build_detail(*r);
    int  visible = rows - 2;    // -1 header -1 scrollbar

    for (int row = 0; row < visible; row++) {
        int di = ui.detail_top + row;
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
                  ui.detail_top + 1, (int)detail.size());
        hline_to_eol(w, rows - 1, C_DIM);
        wattroff(w, COLOR_PAIR(C_DIM) | A_DIM);
    }

    wrefresh(w);
}

// ─── Draw: status / thread bar ───────────────────────────────────────────────

static void draw_status(WINDOW* w, const UI& ui)
{
    int rows, cols; getmaxyx(w, rows, cols); (void)rows;
    werase(w);

    // Thread summary
    wattron(w, COLOR_PAIR(C_THREAD));
    mvwaddstr(w, 0, 0, " Threads:");
    for (const auto& th : ui.ps->threads) {
        char buf[64];
        if (th.leak_count > 0)
            snprintf(buf, sizeof(buf), "  %s[%s total, LEAK %s]",
                     th.name.c_str(),
                     fmt_size(th.total_bytes).c_str(),
                     fmt_size(th.leak_bytes).c_str());
        else
            snprintf(buf, sizeof(buf), "  %s[%s]",
                     th.name.c_str(), fmt_size(th.total_bytes).c_str());
        if (getcurx(w) + (int)strlen(buf) < cols)
            waddstr(w, buf);
    }
    hline_to_eol(w, 0, C_THREAD);
    wattroff(w, COLOR_PAIR(C_THREAD));

    // Key hints
    wattron(w, COLOR_PAIR(C_DIM) | A_DIM);
    mvwprintw(w, 1, 0,
              " q:quit  f:filter  t:thread  s/S:sort(rev)  1/2/3:sort-by  Tab/h/l:pane  j/k:nav  ^f/^b:page  g/G:top/bot%s",
              ui.live ? "  F:follow" : "");
    hline_to_eol(w, 1, C_DIM);
    wattroff(w, COLOR_PAIR(C_DIM) | A_DIM);

    wrefresh(w);
}

// ─── Window management ───────────────────────────────────────────────────────

struct Windows {
    WINDOW *header, *list, *detail, *status;
};

// Exact column width required for the list pane:
//   " [X] " (5) + ptr "%-18s " (19) + op "%-9s " (10) +
//   time "%-12s " (13) + size "%-9s " (10) + thread "%-15s" (15) = 72
static constexpr int LIST_W = 72;

// Actual list pane width — shrinks to half on very narrow terminals so the
// detail pane always gets at least 30 columns.
static int list_pane_w() { return (COLS >= LIST_W + 30) ? LIST_W : COLS / 2; }

static Windows make_windows()
{
    int rows = LINES, cols = COLS;
    int lw   = list_pane_w();
    int body = rows - 4;   // 2 header + 2 status
    return {
        newwin(2,    cols,       0,      0),
        newwin(body, lw,         2,      0),
        newwin(body, cols-lw-1,  2,      lw+1),
        newwin(2,    cols,       rows-2, 0),
    };
}

static void free_windows(Windows& w)
{
    delwin(w.header); delwin(w.list);
    delwin(w.detail); delwin(w.status);
}

// ─── Main UI loop ────────────────────────────────────────────────────────────

static void run(ParseState& ps, const string& filename, bool live, LiveReader* reader)
{
    UI ui;
    ui.ps   = &ps;
    ui.live = live;
    ui.rebuild();

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    set_escdelay(50);
    init_colors();

    // In live mode use a 250 ms input timeout so we can poll for new data.
    if (live) wtimeout(stdscr, 250);

    Windows win = make_windows();

    auto redraw = [&]() {
        int lw = list_pane_w();
        attron(COLOR_PAIR(C_DIM) | A_DIM);
        mvvline(2, lw, ACS_VLINE, LINES - 4);
        attroff(COLOR_PAIR(C_DIM) | A_DIM);
        refresh();
        draw_header(win.header, ui, filename);
        draw_list(win.list, ui);
        draw_detail(win.detail, ui);
        draw_status(win.status, ui);
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
                redraw();
            } else if (new_lines > 0) {
                int prev_vis = (int)ui.visible.size();
                ui.rebuild(false);   // keep scroll position

                if (ui.auto_follow && (int)ui.visible.size() > prev_vis) {
                    ui.selected = max(0, (int)ui.visible.size() - 1);
                    ensure_visible();
                }
                redraw();
            }
            continue;
        }

        if (ch == 'q' || ch == 27) break;

        // Resize
        if (ch == KEY_RESIZE) {
            free_windows(win);
            clear(); refresh();
            win = make_windows();
            redraw();
            continue;
        }

        // Pane switch
        if (ch == '\t' || ch == KEY_LEFT || ch == KEY_RIGHT ||
            ch == 'h'  || ch == 'l') {
            ui.focus_detail = !ui.focus_detail;
            redraw();
            continue;
        }

        // Toggle auto-follow (live mode only)
        if (ch == 'F' && live) {
            ui.auto_follow = !ui.auto_follow;
            redraw();
            continue;
        }

        if (!ui.focus_detail) {
            // ── List navigation ──────────────────────────────────────────
            int h, w2; getmaxyx(win.list, h, w2); (void)w2;
            int page = h - 2;

            if ((ch == KEY_UP || ch == 'k') && ui.selected > 0) {
                ui.selected--;
                ui.detail_top = 0;
                // Manual navigation disables auto-follow so the view doesn't
                // jump away while the user is browsing.
                if (live) ui.auto_follow = false;
                ensure_visible();
            } else if ((ch == KEY_DOWN || ch == 'j') && ui.selected < (int)ui.visible.size()-1) {
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
                ui.selected = min((int)ui.visible.size()-1, ui.selected + page);
                ui.list_top = min(max(0,(int)ui.visible.size()-page), ui.list_top+page);
                ui.detail_top = 0;
                if (live) ui.auto_follow = false;
            } else if (ch == KEY_HOME || ch == 'g') {
                ui.selected = 0; ui.list_top = 0; ui.detail_top = 0;
                if (live) ui.auto_follow = false;
            } else if (ch == KEY_END || ch == 'G') {
                ui.selected = max(0, (int)ui.visible.size()-1);
                ui.list_top = max(0, ui.selected - page + 1);
                ui.detail_top = 0;
                // G = jump to bottom → re-enable auto-follow
                if (live) ui.auto_follow = true;
            } else if (ch == 'f') {
                ui.filter = (FilterMode)((ui.filter + 1) % F_COUNT);
                ui.rebuild();
            } else if (ch == 's') {
                SortMode next = (SortMode)((ui.sort + 1) % S_COUNT);
                if (next == ui.sort)
                    ui.sort_rev = !ui.sort_rev;
                else {
                    ui.sort_rev = false;
                    ui.sort = next;
                }
                ui.rebuild();
            } else if (ch == 'S') {
                ui.sort_rev = !ui.sort_rev;
                ui.rebuild();
            } else if (ch == '1') {
                ui.sort_rev = (ui.sort == S_TIME) ? !ui.sort_rev : false;
                ui.sort = S_TIME; ui.rebuild();
            } else if (ch == '2') {
                ui.sort_rev = (ui.sort == S_SIZE) ? !ui.sort_rev : false;
                ui.sort = S_SIZE; ui.rebuild();
            } else if (ch == '3') {
                ui.sort_rev = (ui.sort == S_THREAD) ? !ui.sort_rev : false;
                ui.sort = S_THREAD; ui.rebuild();
            } else if (ch == 't') {
                if (ps.threads.empty()) goto next_draw;
                if (ui.tid_filter == -1) {
                    ui.tid_filter = ps.threads[0].tid;
                } else {
                    bool advanced = false;
                    for (size_t i = 0; i < ps.threads.size(); i++) {
                        if (ps.threads[i].tid == ui.tid_filter) {
                            ui.tid_filter = (i+1 < ps.threads.size())
                                            ? ps.threads[i+1].tid : -1;
                            advanced = true;
                            break;
                        }
                    }
                    if (!advanced) ui.tid_filter = -1;
                }
                ui.rebuild();
            }
        } else {
            // ── Detail pane navigation ───────────────────────────────────
            int h, w2; getmaxyx(win.detail, h, w2); (void)w2;
            int page = h - 2;
            if      (ch == KEY_UP   || ch == 'k') ui.detail_top = max(0, ui.detail_top - 1);
            else if (ch == KEY_DOWN || ch == 'j') ui.detail_top++;
            else if (ch == KEY_PPAGE || ch == ctrl('b')) ui.detail_top = max(0, ui.detail_top - page);
            else if (ch == KEY_NPAGE || ch == ctrl('f')) ui.detail_top += page;
            else if (ch == KEY_HOME || ch == 'g') ui.detail_top = 0;
            else if (ch == 'G') ui.detail_top = INT_MAX;  // clamped on draw
        }

        next_draw:
        redraw();
    }

    free_windows(win);
    endwin();
}

// ─── Entry point ─────────────────────────────────────────────────────────────

int main(int argc, char* argv[])
{
    bool   live = false;
    string path;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f") || !strcmp(argv[i], "--follow"))
            live = true;
        else if (argv[i][0] != '-' || !strcmp(argv[i], "-"))
            path = argv[i];
    }

    if (path.empty()) {
        fprintf(stderr,
                "Usage: %s [-f] <memtrack.log>\n"
                "       %s [-f] -              (stdin)\n"
                "\n"
                "  -f / --follow   Live mode: monitor file and update display as new\n"
                "                  events are written.  Use F inside the viewer to\n"
                "                  toggle auto-scroll.\n",
                argv[0], argv[0]);
        return 1;
    }

    // Initial parse of whatever is already in the file.
    long initial_pos = 0;
    string real_path = (path == "-") ? "/dev/stdin" : path;
    ParseState ps = parse_file(real_path, &initial_pos);

    if (!live && ps.records.empty()) {
        fprintf(stderr, "No memtrack records found in '%s'.\n"
                        "Make sure MEMTRACK_MIN_SIZE is not filtering everything out.\n",
                path.c_str());
        return 1;
    }

    // In live mode, open the reader from where the initial parse left off.
    LiveReader reader;
    if (live) reader.open(real_path, initial_pos);

    run(ps, path, live, live ? &reader : nullptr);
    return 0;
}
