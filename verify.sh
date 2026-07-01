#!/usr/bin/env bash
# verify.sh – parse a memtrack log produced by test_app and report PASS/FAIL
# Works with BOTH compact mode (default: no size= in free lines, no LEAK/SUMMARY)
# and full mode (MEMTRACK_COMPACT=0: size= in free lines, LEAK/SUMMARY lines).
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

# Detect compact vs full mode: full mode has size= in free lines.
COMPACT=1
if grep -qE '^\[memtrack\].*\b(free|delete|delete\[\])\s.*size=[0-9]' "$LOG" 2>/dev/null; then
    COMPACT=0
fi

# Return ptrs (one per line) for all alloc lines of a given size.
alloc_ptrs() {
    local sz="$1"
    grep -E " (malloc|calloc|realloc|new|new\[\]) +.*size=${sz} " "$LOG" \
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
# This handles address reuse: the same ptr may be freed and reallocated many times.
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

# Backward-compat: true if ptr appears in any free line (ignores reuse — avoid where possible).
ptr_is_freed() {
    local ptr="$1"
    grep -qE " (free|delete(\[\])?) +.*\bptr=${ptr}$" "$LOG"
}
ptr_not_freed() { ! ptr_is_freed "$1"; }

# Count alloc lines with given size.
alloc_count() {
    grep -cE " (malloc|calloc|realloc|new|new\[\]) +.*size=${1} " "$LOG" 2>/dev/null || true
}

# ── Check that every alloc of a given size has a matching free ────────────────
# In compact mode: match by ptr with line-number awareness to handle address reuse.
# In full mode: count by size in both alloc and free lines.
check_alloc_free() {
    local sz="$1" label="$2"
    local allocs
    allocs=$(alloc_count "$sz")
    if [[ "$allocs" -eq 0 ]]; then
        fail "$label: size=$sz — not found in log"
        return
    fi
    if [[ "$COMPACT" -eq 0 ]]; then
        local frees
        frees=$(grep -cE " (free|delete|delete\[\]) +.*size=${sz} " "$LOG" 2>/dev/null || true)
        if [[ "$frees" -ge "$allocs" ]]; then
            pass "$label: size=$sz allocated ($allocs) and freed ($frees)"
        else
            fail "$label: size=$sz — $allocs allocs but only $frees frees (possible leak)"
        fi
    else
        local freed=0
        while IFS=' ' read -r lineno ptr; do
            ptr_freed_after "$ptr" "$lineno" && ((freed++)) || true
        done < <(alloc_entries "$sz")
        if [[ "$freed" -ge "$allocs" ]]; then
            pass "$label: size=$sz allocated ($allocs) and freed ($freed)"
        else
            fail "$label: size=$sz — $allocs allocs but only $freed freed (possible leak)"
        fi
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
echo "  memtrack verify: $LOG ($([ "$COMPACT" -eq 1 ] && echo compact || echo full) mode)"
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
# In compact mode: free of the 6006 ptr should appear before the realloc(12012) alloc line.
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
for sz in 200 400 800 9009; do
    a=$(grep -cE " (malloc|realloc) +.*size=${sz} " "$LOG" 2>/dev/null || true)
    if [[ "$COMPACT" -eq 0 ]]; then
        f=$(grep -cE " (free|realloc) +.*size=${sz} " "$LOG" 2>/dev/null || true)
        [[ "$a" -ge 1 && "$f" -ge 1 ]] && pass "T09: chain step size=$sz present and freed" \
                                        || fail "T09: chain step size=$sz missing (allocs=$a frees=$f)"
    else
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
    fi
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
if [[ "$COMPACT" -eq 0 ]]; then
    f13=$(grep -cE " free +.*size=13 " "$LOG" 2>/dev/null || true)
    if [[ "$m13" -ge 200 && "$f13" -ge 200 ]]; then
        pass "T13: 200 malloc(13) and 200 free(13) found"
    else
        fail "T13: expected ≥200 of each, got malloc=$m13 free=$f13"
    fi
else
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
# In full mode there is also a LEAK line; check it if so.
if [[ "$COMPACT" -eq 0 ]]; then
    check_present "LEAK.*size=14014" "T14: LEAK(14014) in full-mode log"
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
# In full mode, SUMMARY line confirms no leak.
if [[ "$COMPACT" -eq 0 ]]; then
    check_absent "t15-alloc.*SUMMARY.*unfreed" "T15: t15-alloc SUMMARY shows no unfreed"
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
    # In full mode, verify LEAK line exists before the free.
    if [[ "$COMPACT" -eq 0 ]]; then
        leak_line=$(grep -n "LEAK.*size=16016" "$LOG" | head -1 | cut -d: -f1)
        if [[ -n "$leak_line" && -n "$free_line" && "$free_line" -gt "$leak_line" ]]; then
            pass "T16: full mode — LEAK(16016) then free(16016)"
        else
            fail "T16: full mode — LEAK/free ordering wrong"
        fi
    fi
fi

# ── TEST 17: multi-thread clean ───────────────────────────────────────────────
echo ""; echo "TEST 17: multi-thread clean"
for w in 1 2 3 4; do
    if [[ "$COMPACT" -eq 0 ]]; then
        if grep -q "t17-w${w}.*SUMMARY.*all allocations freed" "$LOG"; then
            pass "T17: t17-w${w} SUMMARY: all allocations freed"
        else
            fail "T17: t17-w${w} SUMMARY did not say 'all allocations freed'"
        fi
    else
        # Compact mode: verify EXIT emitted and all ptrs by this thread name are freed.
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

# ── Final summary ─────────────────────────────────────────────────────────────
echo ""
echo "══════════════════════════════════════════════════════════════"
printf "  Total: \033[32m%d passed\033[0m, \033[31m%d failed\033[0m\n" "$PASS" "$FAIL"
echo "══════════════════════════════════════════════════════════════"
[[ "$FAIL" -eq 0 ]]
