#!/usr/bin/env bash
# verify.sh – parse a memtrack log produced by test_app and report PASS/FAIL
# Free lines have no size= field; alloc/free matching is done by ptr with
# line-number awareness to handle address reuse correctly.
# Usage:  ./verify.sh mt.log
#         LD_PRELOAD=./memtrack.so ./test_app 2>mt.log && ./verify.sh mt.log

set -uo pipefail
LOG="${1:-mt.log}"

if [[ ! -f "$LOG" ]]; then
    echo "Usage: $0 <memtrack-log>" >&2
    exit 1
fi

PASS=0; FAIL=0

pass() { printf "  \033[32m✓\033[0m  %s\n" "$1"; ((PASS++)); }
fail() { printf "  \033[31m✗\033[0m  %s\n" "$1"; ((FAIL++)); }

# Return ptrs (one per line) for all alloc lines of a given size.
alloc_ptrs() {
    grep -E " (malloc|calloc|realloc|new|new\[\]) +.*size=${1} " "$LOG" \
        | grep -oE 'ptr=0x[0-9a-f]+' | sed 's/ptr=//'
}

# Return "lineno ptr" pairs for all alloc lines of a given size.
alloc_entries() {
    local sz="$1"
    grep -nE " (malloc|calloc|realloc|new|new\[\]) +.*size=${sz} " "$LOG" \
        | while IFS= read -r line; do
            local lineno ptr
            lineno=$(echo "$line" | cut -d: -f1)
            ptr=$(echo "$line" | grep -oE 'ptr=0x[0-9a-f]+' | sed 's/ptr=//')
            echo "$lineno $ptr"
        done
}

# Return the ptr for the first alloc line with given size.
first_alloc_ptr() { alloc_ptrs "$1" | head -1; }

# Return "lineno ptr" for the first alloc of a given size.
first_alloc_entry() { alloc_entries "$1" | head -1; }

# True if ptr appears in a free/delete line AFTER a given log line number.
# Handles address reuse: the same ptr may be freed and reallocated many times.
ptr_freed_after() {
    local ptr="$1" after="$2"
    grep -nE " (free|delete(\[\])?) +.*\bptr=${ptr}$" "$LOG" \
        | awk -F: -v a="$after" '$1 > a {found=1; exit} END {exit !found}' 2>/dev/null
}

# Line number of first free for ptr that appears AFTER a given line.
ptr_free_lineno_after() {
    local ptr="$1" after="$2"
    grep -nE " (free|delete(\[\])?) +.*\bptr=${ptr}$" "$LOG" \
        | awk -F: -v a="$after" '$1 > a {print $1; exit}'
}

# True if ptr appears in any free line (for quick leak checks where reuse is not a concern).
ptr_is_freed() {
    grep -qE " (free|delete(\[\])?) +.*\bptr=${1}$" "$LOG"
}

# Count alloc lines with given size.
alloc_count() {
    grep -cE " (malloc|calloc|realloc|new|new\[\]) +.*size=${1} " "$LOG" 2>/dev/null || true
}

# ── Check that every alloc of a given size has a matching free ────────────────
# Match by ptr with line-number awareness to handle address reuse.
check_alloc_free() {
    local sz="$1" label="$2"
    local allocs
    allocs=$(alloc_count "$sz")
    if [[ "$allocs" -eq 0 ]]; then
        fail "$label: size=$sz — not found in log"
        return
    fi
    local freed=0
    while IFS=' ' read -r lineno ptr; do
        ptr_freed_after "$ptr" "$lineno" && ((freed++)) || true
    done < <(alloc_entries "$sz")
    if [[ "$freed" -ge "$allocs" ]]; then
        pass "$label: size=$sz allocated ($allocs) and freed ($freed)"
    else
        fail "$label: size=$sz — $allocs allocs but only $freed freed (possible leak)"
    fi
}

check_present() {
    local pat="$1" label="$2"
    if grep -qE "$pat" "$LOG"; then
        pass "$label"
    else
        fail "$label (pattern not found: '$pat')"
    fi
}

check_absent() {
    local pat="$1" label="$2"
    if ! grep -qE "$pat" "$LOG"; then
        pass "$label"
    else
        fail "$label (unexpected pattern found: '$pat')"
    fi
}

# Verify the alloc op (always has size=, works in both modes).
check_alloc_op() {
    local sz="$1" op_pat="$2" label="$3"
    if grep -qE " ($op_pat) +.*size=${sz} " "$LOG"; then
        pass "$label"
    else
        fail "$label (alloc op '$op_pat' not found for size=$sz)"
    fi
}

# Verify the free op by ptr.  Works in both modes (op is always in the line).
# Uses line-number awareness to handle address reuse correctly.
check_free_op() {
    local sz="$1" op_pat="$2" label="$3"
    local entry lineno ptr
    entry=$(first_alloc_entry "$sz")
    lineno=$(echo "$entry" | cut -d' ' -f1)
    ptr=$(echo "$entry" | cut -d' ' -f2)
    if [[ -z "$ptr" ]]; then
        fail "$label (no alloc for size=$sz found)"
        return
    fi
    if grep -nE " ($op_pat) +.*\bptr=${ptr}$" "$LOG" \
            | awk -F: -v a="$lineno" '$1 > a {found=1; exit} END {exit !found}' 2>/dev/null; then
        pass "$label"
    else
        fail "$label ($op_pat line for ptr=$ptr not found after line $lineno)"
    fi
}


echo ""
echo "══════════════════════════════════════════════════════════════"
echo "  memtrack verify: $LOG"
echo "══════════════════════════════════════════════════════════════"

# ── TEST 01: malloc/free ────────────────────────────────────────────────────
echo ""; echo "TEST 01: malloc/free"
check_alloc_free 1001 "T01"

# ── TEST 02: calloc/free ────────────────────────────────────────────────────
echo ""; echo "TEST 02: calloc/free"
check_alloc_free 2002 "T02"
check_alloc_op 2002 "calloc" "T02: calloc (not malloc) used"

# ── TEST 03: new/delete ─────────────────────────────────────────────────────
echo ""; echo "TEST 03: new/delete"
check_alloc_free 3000 "T03"
check_alloc_op 3000 "new"    "T03: new (not malloc) used"
check_free_op  3000 "delete" "T03: delete (not free) used"

# ── TEST 04: new[]/delete[] ──────────────────────────────────────────────────
echo ""; echo "TEST 04: new[]/delete[]"
check_alloc_free 4004 "T04"
check_alloc_op 4004 'new\[\]'    "T04: new[] used"
check_free_op  4004 'delete\[\]' "T04: delete[] used"

# ── TEST 05: realloc(NULL,n) ──────────────────────────────────────────────────
echo ""; echo "TEST 05: realloc(NULL,n)"
check_alloc_free 5005 "T05"
# GCC substitutes realloc(NULL,N) → malloc(N) at the call site (even at -O0)
check_alloc_op 5005 "(realloc|malloc)" "T05: allocation op logged for size=5005"

# ── TEST 06: realloc grow ─────────────────────────────────────────────────────
echo ""; echo "TEST 06: realloc grow"
check_alloc_free 6006  "T06: original malloc"
check_alloc_free 12012 "T06: grown realloc"
# The free of 6006 should precede the realloc(12012) in the log.
ptr6006=$(first_alloc_ptr 6006)
lno6006=$(first_alloc_entry 6006 | cut -d' ' -f1)
if [[ -n "$ptr6006" && -n "$lno6006" ]]; then
    line_free_6006=$(ptr_free_lineno_after "$ptr6006" "$lno6006")
    line_realloc_12012=$(grep -n "realloc.*size=12012 " "$LOG" | head -1 | cut -d: -f1)
    if [[ -n "$line_free_6006" && -n "$line_realloc_12012" && "$line_free_6006" -lt "$line_realloc_12012" ]]; then
        pass "T06: free(ptr for 6006) appears before realloc(12012) in log"
    else
        fail "T06: wrong ordering — free(6006) should precede realloc(12012)"
    fi
else
    fail "T06: alloc for size=6006 not found"
fi

# ── TEST 07: realloc shrink ───────────────────────────────────────────────────
echo ""; echo "TEST 07: realloc shrink"
check_alloc_free 7007 "T07: original malloc"
check_alloc_free 3500 "T07: shrunk realloc"

# ── TEST 08: realloc(ptr,0) ───────────────────────────────────────────────────
echo ""; echo "TEST 08: realloc(ptr,0)"
check_alloc_free 8008 "T08: malloc then realloc(ptr,0)"

# ── TEST 09: realloc chain ────────────────────────────────────────────────────
echo ""; echo "TEST 09: realloc chain"
check_alloc_free 9009 "T09: final realloc size"
# Verify each intermediate alloc has a corresponding free.
# Verify each intermediate alloc has a corresponding free.
for sz in 200 400 800 9009; do
    a=$(grep -cE " (malloc|realloc) +.*size=${sz} " "$LOG" 2>/dev/null || true)
    freed=0; found=0
    while IFS=' ' read -r lineno ptr; do
        ((found++))
        ptr_freed_after "$ptr" "$lineno" && ((freed++)) || true
    done < <(grep -nE " (malloc|realloc) +.*size=${sz} " "$LOG" \
               | while IFS= read -r line; do
                   lno=$(echo "$line" | cut -d: -f1)
                   p=$(echo "$line" | grep -oE 'ptr=0x[0-9a-f]+' | sed 's/ptr=//')
                   echo "$lno $p"
               done)
    [[ "$found" -ge 1 && "$freed" -ge "$found" ]] \
        && pass "T09: chain step size=$sz present and freed" \
        || fail "T09: chain step size=$sz: found=$found freed=$freed"
done

# ── TEST 10: free(NULL) silent ────────────────────────────────────────────────
echo ""; echo "TEST 10: free(NULL)"
check_absent 'free.*ptr=(0x0|0x00+)\b' "T10: no free(NULL) log entry"

# ── TEST 11: malloc(0) ────────────────────────────────────────────────────────
echo ""; echo "TEST 11: malloc(0)"
m0=$(grep -cE " malloc +.*size=0 " "$LOG" 2>/dev/null || true)
if [[ "$m0" -eq 0 ]]; then
    pass "T11: malloc(0) → NULL, no log entry"
else
    entry0=$(first_alloc_entry 0)
    lno0=$(echo "$entry0" | cut -d' ' -f1)
    ptr0=$(echo "$entry0" | cut -d' ' -f2)
    if [[ -n "$ptr0" ]] && ptr_freed_after "$ptr0" "$lno0"; then
        pass "T11: malloc(0) tracked and freed (glibc non-NULL behaviour)"
    else
        fail "T11: malloc(0) count=$m0 but ptr not freed — imbalanced"
    fi
fi

# ── TEST 12: 10 MB ────────────────────────────────────────────────────────────
echo ""; echo "TEST 12: large allocation"
check_alloc_free 10485760 "T12: 10 MB alloc+free"

# ── TEST 13: many small allocs ───────────────────────────────────────────────
echo ""; echo "TEST 13: many small allocs"
m13=$(grep -cE " malloc +.*size=13 " "$LOG" 2>/dev/null || true)
freed13=0
while IFS=' ' read -r lineno ptr; do
    ptr_freed_after "$ptr" "$lineno" && ((freed13++)) || true
done < <(grep -nE " malloc +.*size=13 " "$LOG" \
           | while IFS= read -r line; do
               lno=$(echo "$line" | cut -d: -f1)
               p=$(echo "$line" | grep -oE 'ptr=0x[0-9a-f]+' | sed 's/ptr=//')
               echo "$lno $p"
           done)
if [[ "$m13" -ge 200 && "$freed13" -ge 200 ]]; then
    pass "T13: 200 malloc(13) allocated ($m13) and freed ($freed13)"
else
    fail "T13: expected ≥200 of each, got malloc=$m13 freed=$freed13"
fi

# ── TEST 14: intentional leak ─────────────────────────────────────────────────
echo ""; echo "TEST 14: intentional leak"
entry14=$(first_alloc_entry 14014)
lno14=$(echo "$entry14" | cut -d' ' -f1)
ptr14=$(echo "$entry14" | cut -d' ' -f2)
if [[ -z "$ptr14" ]]; then
    fail "T14: malloc(14014) not found in log"
elif ! ptr_freed_after "$ptr14" "$lno14"; then
    pass "T14: malloc(14014) at $ptr14 — correctly NOT freed (leak)"
else
    fail "T14: malloc(14014) was freed after alloc — expected intentional leak"
fi

# ── TEST 15: cross-thread free ────────────────────────────────────────────────
echo ""; echo "TEST 15: cross-thread free"
entry15=$(first_alloc_entry 15015)
lno15=$(echo "$entry15" | cut -d' ' -f1)
ptr15=$(echo "$entry15" | cut -d' ' -f2)
tid_alloc=$(grep "size=15015 " "$LOG" | grep -E "malloc|calloc|new|realloc" \
            | grep -oE 'tid=[0-9]+' | head -1)
if [[ -n "$ptr15" ]]; then
    # Find the free that comes after the alloc (line-number aware).
    tid_free=$(grep -nE " (free|delete(\[\])?) +.*\bptr=${ptr15}$" "$LOG" \
               | awk -F: -v a="$lno15" '$1 > a {print; exit}' \
               | grep -oE 'tid=[0-9]+' | head -1)
    if [[ -n "$tid_alloc" && -n "$tid_free" && "$tid_alloc" != "$tid_free" ]]; then
        pass "T15: malloc(15015) $tid_alloc, free $tid_free — different threads"
    else
        fail "T15: tids expected to differ: alloc=$tid_alloc free=${tid_free:-<not found>}"
    fi
    # ptr should be freed (cross-thread free, no leak).
    if ptr_freed_after "$ptr15" "$lno15"; then
        pass "T15: t15-alloc allocation was freed by another thread"
    else
        fail "T15: t15-alloc allocation was NOT freed"
    fi
else
    fail "T15: malloc(15015) not found in log"
fi

# ── TEST 16: LEAK cancellation ───────────────────────────────────────────────
echo ""; echo "TEST 16: LEAK cancellation"
entry16=$(first_alloc_entry 16016)
lno16=$(echo "$entry16" | cut -d' ' -f1)
ptr16=$(echo "$entry16" | cut -d' ' -f2)
if [[ -z "$ptr16" ]]; then
    fail "T16: malloc(16016) not found in log"
else
    # The thread t16-leak exits leaving ptr16 unfreed; main later frees it.
    # Verify: EXIT appears for t16-leak before the free of ptr16 (after alloc line).
    exit_line=$(grep -nE "\(t16-leak\s*\) EXIT " "$LOG" | head -1 | cut -d: -f1)
    free_line=$(ptr_free_lineno_after "$ptr16" "$lno16")
    if [[ -n "$exit_line" && -n "$free_line" && "$free_line" -gt "$exit_line" ]]; then
        pass "T16: free($ptr16) at line $free_line appears after EXIT at line $exit_line"
    else
        fail "T16: ordering wrong — EXIT line=$exit_line, free line=${free_line:-<not found>}"
    fi
    # The ptr must ultimately be freed (cancellation).
    if ptr_freed_after "$ptr16" "$lno16"; then
        pass "T16: ptr16016 freed after thread exit (LEAK cancelled)"
    else
        fail "T16: ptr16016 was NOT freed — expected cancellation"
    fi
fi

# ── TEST 17: multi-thread clean ───────────────────────────────────────────────
echo ""; echo "TEST 17: multi-thread clean"
for w in 1 2 3 4; do
    # Verify EXIT emitted and all ptrs by this thread name are freed.
    if grep -qE "\(t17-w${w}\s*\) EXIT " "$LOG"; then
        leaked=0 total=0
        while IFS=' ' read -r lineno ptr; do
            ((total++))
            ptr_freed_after "$ptr" "$lineno" || ((leaked++)) || true
        done < <(grep -nE "\(t17-w${w}\s*\) (malloc|calloc|new|new\[\]|realloc) " "$LOG" \
                   | while IFS= read -r line; do
                       lno=$(echo "$line" | cut -d: -f1)
                       p=$(echo "$line" | grep -oE 'ptr=0x[0-9a-f]+' | sed 's/ptr=//')
                       echo "$lno $p"
                   done)
        if [[ "$leaked" -eq 0 && "$total" -gt 0 ]]; then
            pass "T17: t17-w${w} — all $total allocations freed"
        elif [[ "$total" -eq 0 ]]; then
            fail "T17: t17-w${w} — no allocations found in log"
        else
            fail "T17: t17-w${w} — $leaked/$total allocations NOT freed"
        fi
    else
        fail "T17: t17-w${w} — EXIT line not found"
    fi
done

# ── TEST 18: sized delete ────────────────────────────────────────────────────
echo ""; echo "TEST 18: sized delete"
check_alloc_free 18018 "T18: new+sized_delete(18018)"
check_alloc_op 18018 "new"    "T18: new used for alloc"
check_free_op  18018 "delete" "T18: delete (not free) used"

# ── TEST 19: failed realloc ───────────────────────────────────────────────────
echo ""; echo "TEST 19: failed realloc safety"
check_alloc_free 19019 "T19: malloc(19019) freed after failed realloc"

# ── Helper: check that a #0 frame exists within N lines after the alloc line ──
# Usage: check_frame_after_alloc <size> <search_window> <label>
check_frame_after_alloc() {
    local sz="$1" window="$2" label="$3"
    local entry lno
    entry=$(first_alloc_entry "$sz")
    lno=$(echo "$entry" | cut -d' ' -f1)
    if [[ -z "$lno" ]]; then
        fail "$label: no alloc for size=$sz found — cannot check frames"
        return
    fi
    local frame
    frame=$(awk -v s="$lno" -v w="$window" \
        'NR > s && NR <= s+w && /\[memtrack\]   #0 / {print; exit}' "$LOG")
    if [[ -n "$frame" ]]; then
        pass "$label: #0 frame present in stack trace"
    else
        fail "$label: #0 frame NOT found within $window lines after alloc (stack traces disabled?)"
    fi
}

# ── TEST 20: deep call stack ──────────────────────────────────────────────────
echo ""; echo "TEST 20: deep call stack"
check_alloc_free 20020 "T20: malloc(20020) deep stack"
check_frame_after_alloc 20020 80 "T20"
# Verify no line in the vicinity has two [memtrack] markers (corruption check).
lno20=$(first_alloc_entry 20020 | cut -d' ' -f1)
if [[ -n "$lno20" ]]; then
    double20=$(awk -v s="$lno20" 'NR >= s && NR <= s+80' "$LOG" \
               | grep -cP '\[memtrack\].*\[memtrack\]' || true)
    if [[ "$double20" -eq 0 ]]; then
        pass "T20: no log corruption around deep-stack event"
    else
        fail "T20: log corruption — $double20 lines with two [memtrack] markers"
    fi
else
    fail "T20: malloc(20020) not found — cannot do corruption check"
fi

# ── TEST 21: long symbol names (heap buffer) ──────────────────────────────────
echo ""; echo "TEST 21: long symbol names"
check_alloc_free 21021 "T21: malloc(21021) long template symbols"
check_frame_after_alloc 21021 80 "T21"
lno21=$(first_alloc_entry 21021 | cut -d' ' -f1)
if [[ -n "$lno21" ]]; then
    double21=$(awk -v s="$lno21" 'NR >= s && NR <= s+80' "$LOG" \
               | grep -cP '\[memtrack\].*\[memtrack\]' || true)
    if [[ "$double21" -eq 0 ]]; then
        pass "T21: no log corruption despite long symbol names"
    else
        fail "T21: log corruption — $double21 lines with two [memtrack] markers"
    fi
else
    fail "T21: malloc(21021) not found — cannot do corruption check"
fi

# ── TEST 22: MEMTRACK_STACK_THREADS filter ────────────────────────────────────
# Run test_app again with STACK_THREADS set to only "t22thread" (a name that
# matches no thread in the app).  No alloc events should have any frame lines.
# Then run again with STACK_THREADS matching "test_app" (the main thread name).
echo ""; echo "TEST 22: MEMTRACK_STACK_THREADS filter"

MT22_NONE=$(dirname "$LOG")/mt22_none.log
MT22_MAIN=$(dirname "$LOG")/mt22_main.log

# Run 1: thread name that matches nothing → zero frame lines in entire log
LD_PRELOAD=./memtrack.so \
    MEMTRACK_STACK_DEPTH=16 \
    MEMTRACK_STACK_THREADS=no_such_thread \
    ./test_app >"$MT22_NONE.stdout" 2>"$MT22_NONE" || true

frames_none=$(grep -cP '^\[memtrack\]   #' "$MT22_NONE" 2>/dev/null || true)
if [[ "$frames_none" -eq 0 ]]; then
    pass "T22: STACK_THREADS=no_such_thread → 0 frame lines (filter suppresses all)"
else
    fail "T22: STACK_THREADS=no_such_thread → $frames_none unexpected frame lines"
fi

# Run 2: thread name "test_app" matches the main thread → frame lines present
LD_PRELOAD=./memtrack.so \
    MEMTRACK_STACK_DEPTH=16 \
    MEMTRACK_STACK_THREADS=test_app \
    ./test_app >"$MT22_MAIN.stdout" 2>"$MT22_MAIN" || true

frames_main=$(grep -cP '^\[memtrack\]   #' "$MT22_MAIN" 2>/dev/null || true)
# Allocs logged for non-matching threads (t15-alloc, t16-leak, t17-wN, …) must
# have NO frame lines — spot-check t17-w1 allocs.
t17_frames=$(grep -A1 "(t17-w1" "$MT22_MAIN" | grep -cP '^\[memtrack\]   #' || true)
if [[ "$frames_main" -gt 0 ]]; then
    pass "T22: STACK_THREADS=test_app → $frames_main frame lines for main thread"
else
    fail "T22: STACK_THREADS=test_app → 0 frame lines, expected some for main"
fi
if [[ "$t17_frames" -eq 0 ]]; then
    pass "T22: t17-w1 thread (not in filter) has no frame lines"
else
    fail "T22: t17-w1 thread unexpectedly has $t17_frames frame lines"
fi

rm -f "$MT22_NONE" "$MT22_NONE.stdout" "$MT22_MAIN" "$MT22_MAIN.stdout"

# ── T23: strdup / strndup tracked ────────────────────────────────────────────
section "T23: strdup / strndup intercepted"

# strdup: 7 bytes (strlen("hello!")+1), op=strdup, freed → no LEAK
strdup_alloc=$(grep -cP '^\[memtrack\] tid=.*\) strdup\s.*size=7\b' "$LOG" || true)
[[ "$strdup_alloc" -ge 1 ]] \
    && pass "T23: strdup(7) logged with op=strdup" \
    || fail "T23: strdup(7) not found in log"

# strndup: 6 bytes (5 chars + NUL), op=strndup, freed → no LEAK
strndup_alloc=$(grep -cP '^\[memtrack\] tid=.*\) strndup\s.*size=6\b' "$LOG" || true)
[[ "$strndup_alloc" -ge 1 ]] \
    && pass "T23: strndup(5+1=6) logged with op=strndup" \
    || fail "T23: strndup(6) not found in log"

# Both should be freed (no LEAK line for either)
strdup_leak=$(grep -cP '^\[memtrack\].*LEAK.*\) strdup\b' "$LOG" || true)
strndup_leak=$(grep -cP '^\[memtrack\].*LEAK.*\) strndup\b' "$LOG" || true)
[[ "$strdup_leak" -eq 0 && "$strndup_leak" -eq 0 ]] \
    && pass "T23: strdup/strndup allocations not leaked" \
    || fail "T23: unexpected LEAK for strdup/strndup ($strdup_leak/$strndup_leak)"

# ── T24: mmap / munmap ───────────────────────────────────────────────────────
echo ""
echo "── T24: anonymous mmap/munmap tracking ──────────────────────────────────"

mmap_alloc=$(grep -cP '^\[memtrack\] tid=.*\) mmap\s.*size=24576\b' "$LOG" || true)
[[ "$mmap_alloc" -ge 1 ]] \
    && pass "T24: mmap(24576) logged with op=mmap" \
    || fail "T24: mmap(24576) not found in log"

mmap_leak=$(grep -cP '^\[memtrack\].*LEAK.*\) mmap\b' "$LOG" || true)
[[ "$mmap_leak" -eq 0 ]] \
    && pass "T24: mmap allocation not leaked (munmap'd)" \
    || fail "T24: mmap region shown as LEAK ($mmap_leak)"

echo ""
echo "══════════════════════════════════════════════════════════════"
printf "  Total: \033[32m%d passed\033[0m, \033[31m%d failed\033[0m\n" "$PASS" "$FAIL"
echo "══════════════════════════════════════════════════════════════"
[[ "$FAIL" -eq 0 ]]
