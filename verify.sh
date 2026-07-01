#!/usr/bin/env bash
# verify.sh – parse a memtrack log produced by test_app and report PASS/FAIL
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

check_alloc_free() {
    local sz="$1" label="$2"
    local allocs frees
    allocs=$(grep -cE " (malloc|calloc|realloc|new|new\[\]) +.*size=${sz} " "$LOG" || true)
    frees=$(grep  -cE " (free|delete|delete\[\]) +.*size=${sz} "            "$LOG" || true)
    if   [[ "$allocs" -ge 1 && "$frees" -ge "$allocs" ]]; then
        pass "$label: size=$sz allocated ($allocs) and freed ($frees)"
    elif [[ "$allocs" -ge 1 && "$frees" -lt "$allocs" ]]; then
        fail "$label: size=$sz — $allocs allocs but only $frees frees (possible leak)"
    else
        fail "$label: size=$sz — not found in log"
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
check_present "calloc.*size=2002" "T02: calloc (not malloc) used"

# ── TEST 03: new/delete ─────────────────────────────────────────────────────
echo ""; echo "TEST 03: new/delete"
check_alloc_free 3000 "T03"
check_present "new.*size=3000"    "T03: new (not malloc) used"
check_present "delete.*size=3000" "T03: delete (not free) used"

# ── TEST 04: new[]/delete[] ──────────────────────────────────────────────────
echo ""; echo "TEST 04: new[]/delete[]"
check_alloc_free 4004 "T04"
check_present 'new\[\].*size=4004'    "T04: new[] used"
check_present 'delete\[\].*size=4004' "T04: delete[] used"

# ── TEST 05: realloc(NULL,n) ──────────────────────────────────────────────────
echo ""; echo "TEST 05: realloc(NULL,n)"
check_alloc_free 5005 "T05"
# GCC substitutes realloc(NULL,N) → malloc(N) at the call site (even at -O0)
check_present "(realloc|malloc).*size=5005" "T05: allocation op logged for size=5005"

# ── TEST 06: realloc grow ─────────────────────────────────────────────────────
echo ""; echo "TEST 06: realloc grow"
check_alloc_free 6006  "T06: original malloc"
check_alloc_free 12012 "T06: grown realloc"
# The free of 6006 should precede the realloc(12012)
line_free=$(grep -n "size=6006 " "$LOG"  | grep "free"    | head -1 | cut -d: -f1)
line_real=$(grep -n "size=12012 " "$LOG" | grep "realloc" | head -1 | cut -d: -f1)
if [[ -n "$line_free" && -n "$line_real" && "$line_free" -lt "$line_real" ]]; then
    pass "T06: free(6006) appears before realloc(12012) in log"
else
    fail "T06: wrong ordering — free(6006) should precede realloc(12012)"
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
# Verify intermediate sizes 200 and 400 also appear as both alloc and free
for sz in 200 400 800 9009; do
    a=$(grep -cE " (malloc|realloc) +.*size=${sz} " "$LOG" || true)
    f=$(grep -cE " (free|realloc) +.*size=${sz} "   "$LOG" || true)
    if [[ "$a" -ge 1 && "$f" -ge 1 ]]; then
        pass "T09: chain step size=$sz present and freed"
    else
        fail "T09: chain step size=$sz missing (allocs=$a frees=$f)"
    fi
done

# ── TEST 10: free(NULL) silent ────────────────────────────────────────────────
echo ""; echo "TEST 10: free(NULL)"
# There should be no log line with "free.*size=0.*ptr=0x0" or similar null-ptr free
check_absent "free.*ptr=(nil)\|free.*ptr=0x0\b" "T10: no free(NULL) log entry"

# ── TEST 11: malloc(0) ────────────────────────────────────────────────────────
echo ""; echo "TEST 11: malloc(0)"
# Either no entry or a balanced pair (glibc returns non-NULL)
m0=$(grep -cE " malloc +.*size=0 " "$LOG" || true)
f0=$(grep -cE " free +.*size=0 "   "$LOG" || true)
if [[ "$m0" -eq 0 ]]; then
    pass "T11: malloc(0) → NULL, no log entry"
elif [[ "$m0" -eq "$f0" ]]; then
    pass "T11: malloc(0) tracked and freed (glibc non-NULL behaviour)"
else
    fail "T11: malloc(0) count=$m0 but free(0) count=$f0 — imbalanced"
fi

# ── TEST 12: 10 MB ────────────────────────────────────────────────────────────
echo ""; echo "TEST 12: large allocation"
check_alloc_free 10485760 "T12: 10 MB alloc+free"

# ── TEST 13: many small ───────────────────────────────────────────────────────
echo ""; echo "TEST 13: many small allocs"
m13=$(grep -cE " malloc +.*size=13 "   "$LOG" || true)
f13=$(grep -cE " free +.*size=13 "     "$LOG" || true)
if [[ "$m13" -ge 200 && "$f13" -ge 200 ]]; then
    pass "T13: 200 malloc(13) and 200 free(13) found"
else
    fail "T13: expected ≥200 of each, got malloc=$m13 free=$f13"
fi

# ── TEST 14: intentional leak ─────────────────────────────────────────────────
echo ""; echo "TEST 14: intentional leak"
check_present "LEAK.*size=14014" "T14: LEAK(14014) appears in log"
check_absent  "free.*size=14014" "T14: no free(14014) — correctly leaked"

# ── TEST 15: cross-thread free ────────────────────────────────────────────────
echo ""; echo "TEST 15: cross-thread free"
# malloc should be logged under t15-alloc, free under test_app (main)
tid_alloc=$(grep "malloc.*size=15015" "$LOG" | grep -o "tid=[0-9]*" | head -1)
tid_free=$( grep "free.*size=15015"   "$LOG" | grep -o "tid=[0-9]*" | head -1)
if [[ -n "$tid_alloc" && -n "$tid_free" && "$tid_alloc" != "$tid_free" ]]; then
    pass "T15: malloc(15015) tid=$tid_alloc, free(15015) tid=$tid_free — different threads"
else
    fail "T15: malloc/free tids expected to differ, got alloc=$tid_alloc free=$tid_free"
fi
check_absent "t15-alloc.*SUMMARY.*unfreed" "T15: t15-alloc thread shows no unfreed allocations"

# ── TEST 16: LEAK cancellation ───────────────────────────────────────────────
echo ""; echo "TEST 16: LEAK cancellation"
check_present "LEAK.*size=16016"    "T16: LEAK(16016) logged by t16-leak"
check_present "free.*size=16016"    "T16: free(16016) logged by main after LEAK"
# The free must appear AFTER the LEAK line in the log
line_leak=$(grep -n "LEAK.*size=16016" "$LOG" | head -1 | cut -d: -f1)
line_free=$(grep -n "free.*size=16016" "$LOG" | head -1 | cut -d: -f1)
if [[ -n "$line_leak" && -n "$line_free" && "$line_free" -gt "$line_leak" ]]; then
    pass "T16: free(16016) appears after LEAK(16016) — correct cancellation ordering"
else
    fail "T16: ordering wrong (LEAK line=$line_leak, free line=$line_free)"
fi

# ── TEST 17: multi-thread clean ───────────────────────────────────────────────
echo ""; echo "TEST 17: multi-thread clean"
for w in 1 2 3 4; do
    if grep -q "t17-w${w}.*SUMMARY.*all allocations freed" "$LOG"; then
        pass "T17: t17-w${w} SUMMARY: all allocations freed"
    else
        fail "T17: t17-w${w} SUMMARY did not say 'all allocations freed'"
    fi
done

# ── TEST 18: sized delete ────────────────────────────────────────────────────
echo ""; echo "TEST 18: sized delete"
check_alloc_free 18018 "T18: new+sized_delete(18018)"
check_present "delete.*size=18018" "T18: delete (not free) used"

# ── TEST 19: failed realloc ───────────────────────────────────────────────────
echo ""; echo "TEST 19: failed realloc safety"
check_alloc_free 19019 "T19: malloc(19019) freed after failed realloc"

# ── Final summary ─────────────────────────────────────────────────────────────
echo ""
echo "══════════════════════════════════════════════════════════════"
printf "  Total: \033[32m%d passed\033[0m, \033[31m%d failed\033[0m\n" "$PASS" "$FAIL"
echo "══════════════════════════════════════════════════════════════"
[[ "$FAIL" -eq 0 ]]
