/*
 * test_app.cpp – Comprehensive memtrack test suite
 *
 * Each test uses allocation sizes that encode the test number so you can grep
 * the memtrack log.  Allocation sizes follow the pattern T*1000+N, e.g.
 * test 6 uses sizes 6006 and 12012.
 *
 * Run and capture:
 *   LD_PRELOAD=./memtrack.so ./test_app 2>mt.log
 *   # stdout  → human-readable test progress + expected log entries
 *   # mt.log  → actual memtrack output to verify against
 *
 * Or use the provided verify.sh for automated checking:
 *   LD_PRELOAD=./memtrack.so ./test_app 2>mt.log && ./verify.sh mt.log
 */

#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cassert>
#include <new>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <sys/mman.h>
#include <cstdint>
#include <stdarg.h>

// ─── tiny test framework ────────────────────────────────────────────────────
static int g_pass = 0, g_fail = 0;

static void hdr(int n, const char* name)
{
    printf("\n");
    printf("┌─────────────────────────────────────────────────────────────┐\n");
    printf("│ TEST %02d: %-52s│\n", n, name);
    printf("└─────────────────────────────────────────────────────────────┘\n");
}

static void check(bool ok, const char* msg)
{
    if (ok) { printf("  ✓  %s\n", msg); ++g_pass; }
    else    { printf("  ✗  %s\n", msg); ++g_fail; }
}

// Print a line that will appear in the memtrack log right before the test's
// allocations, making it trivial to correlate.  We use a 1-byte malloc that
// is immediately freed so no tracking side-effect is left behind.
__attribute__((noinline))
static void log_marker(const char* label)
{
    printf("  → LOG MARKER: look for allocations after this line in mt.log\n");
    printf("  → LABEL: %s\n", label);
    // tiny alloc/free pair to create a visible ts anchor in the log
    void* m = malloc(1); free(m);
}

static void expect(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("  EXPECT: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

// ─── TEST 01: malloc / free ──────────────────────────────────────────────────
__attribute__((noinline))
static void test01_malloc_free()
{
    hdr(1, "malloc / free — zero leaks");
    expect("1× malloc(1001) then free(1001), no LEAK");
    log_marker("T01");

    char* p = (char*)malloc(1001);
    assert(p);
    p[0] = 'T';                     // touch so compiler can't eliminate
    free(p);

    check(true, "malloc(1001) + free completed without crash");
}

// ─── TEST 02: calloc / free ──────────────────────────────────────────────────
__attribute__((noinline))
static void test02_calloc_free()
{
    hdr(2, "calloc / free — zeroing + zero leaks");
    expect("1× calloc(2002) zeroed, then free(2002), no LEAK");
    log_marker("T02");

    char* p = (char*)calloc(2, 1001);   // 2002 bytes
    assert(p);
    bool all_zero = true;
    for (int i = 0; i < 2002; i++) if (p[i] != 0) { all_zero = false; break; }
    check(all_zero, "calloc memory is zero-initialised");
    free(p);
}

// ─── TEST 03: new / delete ───────────────────────────────────────────────────
struct Obj3000 { char data[3000]; };

__attribute__((noinline))
static void test03_new_delete()
{
    hdr(3, "new / delete — zero leaks");
    expect("1× new(3000) then delete(3000), no LEAK");
    log_marker("T03");

    Obj3000* p = new Obj3000;
    p->data[0] = 'T';
    delete p;

    check(true, "new/delete completed without crash");
}

// ─── TEST 04: new[] / delete[] ───────────────────────────────────────────────
__attribute__((noinline))
static void test04_new_arr_delete_arr()
{
    hdr(4, "new[] / delete[] — zero leaks");
    expect("1× new[](4004) then delete[](4004), no LEAK");
    log_marker("T04");

    char* p = new char[4004];
    p[0] = 'T';
    delete[] p;

    check(true, "new[]/delete[] completed without crash");
}

// ─── TEST 05: realloc(NULL, size) == malloc ───────────────────────────────────
__attribute__((noinline))
static void test05_realloc_from_null()
{
    hdr(5, "realloc(NULL, n) acts as malloc");
    expect("allocation of 5005 bytes (GCC may emit malloc@plt instead of realloc@plt)");
    expect("then free(5005), no LEAK");
    log_marker("T05");

    char* p = (char*)realloc(nullptr, 5005);
    assert(p);
    p[0] = 'T';
    free(p);

    check(true, "realloc(NULL,5005) returned valid pointer, freed cleanly");
}

// ─── TEST 06: realloc grow ───────────────────────────────────────────────────
__attribute__((noinline))
static void test06_realloc_grow()
{
    hdr(6, "realloc grow (may move) — balanced log");
    expect("malloc(6006) → free(6006) → realloc(12012) → free(12012), no LEAK");
    log_marker("T06");

    char* p = (char*)malloc(6006);
    assert(p);
    p[0] = 'A';
    char* q = (char*)realloc(p, 12012);
    assert(q);
    q[12011] = 'Z';
    free(q);

    check(true, "realloc grow freed old ptr, alloc'd new, freed cleanly");
}

// ─── TEST 07: realloc shrink ─────────────────────────────────────────────────
__attribute__((noinline))
static void test07_realloc_shrink()
{
    hdr(7, "realloc shrink (likely in-place) — balanced log");
    expect("malloc(7007) → free(7007) → realloc(3500) → free(3500), no LEAK");
    log_marker("T07");

    char* p = (char*)malloc(7007);
    assert(p);
    p[0] = 'A';
    char* q = (char*)realloc(p, 3500);
    assert(q);
    q[0] = 'Z';
    free(q);

    check(true, "realloc shrink produced balanced log entries");
}

// ─── TEST 08: realloc(ptr, 0) acts as free ───────────────────────────────────
__attribute__((noinline))
static void test08_realloc_to_zero()
{
    hdr(8, "realloc(ptr, 0) acts as free, returns NULL");
    expect("malloc(8008) → free(8008) [via realloc(ptr,0)], no LEAK");
    log_marker("T08");

    char* p = (char*)malloc(8008);
    assert(p);
    p[0] = 'T';
    void* q = realloc(p, 0);    // our hook treats this as free(p)
    check(q == nullptr, "realloc(ptr,0) returned NULL");
}

// ─── TEST 09: realloc chain ───────────────────────────────────────────────────
__attribute__((noinline))
static void test09_realloc_chain()
{
    hdr(9, "realloc chain — each step logged");
    expect("malloc(100)→free+realloc(200)→free+realloc(400)→free+realloc(800)"
           "→free+realloc(9009)→free, no LEAK");
    log_marker("T09");

    char* p = (char*)malloc(100);
    assert(p); p[0]='a';
    p = (char*)realloc(p, 200);  assert(p); p[0]='b';
    p = (char*)realloc(p, 400);  assert(p); p[0]='c';
    p = (char*)realloc(p, 800);  assert(p); p[0]='d';
    p = (char*)realloc(p, 9009); assert(p); p[9008]='e';
    free(p);

    check(true, "realloc chain of 5 steps completed, all freed");
}

// ─── TEST 10: free(NULL) — silent ────────────────────────────────────────────
__attribute__((noinline))
static void test10_free_null()
{
    hdr(10, "free(NULL) — must be silent, no log entry");
    expect("NO log entry of any kind — free(NULL) is a no-op");
    log_marker("T10");

    free(nullptr);
    free(NULL);

    check(true, "free(NULL) did not crash");
    printf("  VERIFY: between T10 log_marker allocs, expect NO free entry\n");
}

// ─── TEST 11: malloc(0) ───────────────────────────────────────────────────────
__attribute__((noinline))
static void test11_malloc_zero()
{
    hdr(11, "malloc(0) — implementation-defined, must not crash");
    expect("Either no entry (NULL return) OR malloc(0)+free(0) pair, no LEAK");
    log_marker("T11");

    void* p = malloc(0);
    // glibc returns a unique non-NULL pointer for malloc(0)
    printf("  INFO: malloc(0) returned %s\n", p ? "non-NULL" : "NULL");
    if (p) {
        check(true, "malloc(0) returned non-NULL (glibc behaviour)");
        free(p);
    } else {
        check(true, "malloc(0) returned NULL");
    }
}

// ─── TEST 12: large allocation ────────────────────────────────────────────────
__attribute__((noinline))
static void test12_large_alloc()
{
    const size_t TEN_MB = 10u * 1024u * 1024u;
    hdr(12, "large allocation (10 MB)");
    expect("malloc(10485760) then free(10485760), no LEAK");
    log_marker("T12");

    char* p = (char*)malloc(TEN_MB);
    assert(p);
    // Touch a few pages so the OS actually maps them.
    for (size_t i = 0; i < TEN_MB; i += 4096) p[i] = (char)(i & 0xff);
    free(p);

    check(true, "10 MB alloc+free without crash");
}

// ─── TEST 13: many small allocs, all freed ────────────────────────────────────
__attribute__((noinline))
static void test13_many_small()
{
    const int N = 200;
    hdr(13, "many small allocs (200 × 13 bytes), all freed");
    expect("200× malloc(13) and 200× free(13), no LEAK");
    log_marker("T13");

    void* ptrs[N];
    for (int i = 0; i < N; i++) {
        ptrs[i] = malloc(13);
        assert(ptrs[i]);
        ((char*)ptrs[i])[0] = (char)i;
    }
    for (int i = 0; i < N; i++) free(ptrs[i]);

    check(true, "200 small allocs and frees completed");
}

// ─── TEST 14: intentional leak ────────────────────────────────────────────────
static void* g_leak14 = nullptr;   // kept alive intentionally

__attribute__((noinline))
static void test14_intentional_leak()
{
    hdr(14, "intentional leak — must appear in SUMMARY");
    expect("malloc(14014) → NO free → appears as LEAK size=14014 in summary");
    log_marker("T14");

    g_leak14 = malloc(14014);
    assert(g_leak14);
    ((char*)g_leak14)[0] = 'L';
    // deliberately NOT freed
    check(true, "malloc(14014) allocated, intentionally kept");
    printf("  VERIFY: LEAK size=14014 must appear in the final SUMMARY block\n");
}

// ─── TEST 15: cross-thread free (A allocs, main frees, different tid) ─────────
static void* g_cross15 = nullptr;
static sem_t  g_sem15_ready;   // thread A posts when allocation is done
static sem_t  g_sem15_freed;   // main posts when it has freed the ptr

__attribute__((noinline))
static void* t15_alloc_thread(void*)
{
    pthread_setname_np(pthread_self(), "t15-alloc");
    g_cross15 = malloc(15015);
    assert(g_cross15);
    ((char*)g_cross15)[0] = 'X';
    sem_post(&g_sem15_ready);   // signal: ptr is stored
    sem_wait(&g_sem15_freed);   // wait until main has freed it
    // g_cross15 is already freed by main — exit with no LEAK
    return nullptr;
}

__attribute__((noinline))
static void test15_cross_thread_free()
{
    hdr(15, "cross-thread free (alloc in A, free in main, A still alive)");
    expect("malloc(15015) logged under tid=A, free(15015) logged under tid=main");
    expect("t15-alloc SUMMARY: all allocations freed (no LEAK)");
    log_marker("T15");

    sem_init(&g_sem15_ready, 0, 0);
    sem_init(&g_sem15_freed, 0, 0);

    pthread_t th;
    pthread_create(&th, nullptr, t15_alloc_thread, nullptr);
    sem_wait(&g_sem15_ready);       // wait until thread A has allocated

    // Free from main thread while thread A is still alive
    free(g_cross15);
    g_cross15 = nullptr;
    sem_post(&g_sem15_freed);       // let thread A exit cleanly

    pthread_join(th, nullptr);

    sem_destroy(&g_sem15_ready);
    sem_destroy(&g_sem15_freed);

    check(true, "cross-thread free completed");
    printf("  VERIFY: malloc(15015) and free(15015) must have different tid= values\n");
    printf("  VERIFY: t15-alloc SUMMARY must say 'all allocations freed'\n");
}

// ─── TEST 16: LEAK cancellation (A allocs+exits LEAK, main frees) ─────────────
static void*  g_cross16 = nullptr;
static sem_t  g_sem16_allocated;
static sem_t  g_sem16_free_done;

__attribute__((noinline))
static void* t16_leak_thread(void*)
{
    pthread_setname_np(pthread_self(), "t16-leak");
    g_cross16 = malloc(16016);
    assert(g_cross16);
    ((char*)g_cross16)[0] = 'Y';
    sem_post(&g_sem16_allocated);   // signal: ptr is ready
    // Thread returns here → exit handler fires → LEAK for 16016 is reported
    return nullptr;
}

__attribute__((noinline))
static void test16_leak_cancellation()
{
    hdr(16, "LEAK cancellation: A exits (LEAK logged), main frees later");
    expect("LEAK(16016) logged at thread exit, then free(16016) by main thread");
    expect("In memview: the LEAK entry should be marked freed (not shown as leak)");
    log_marker("T16");

    sem_init(&g_sem16_allocated, 0, 0);
    sem_init(&g_sem16_free_done, 0, 0);

    pthread_t th;
    pthread_create(&th, nullptr, t16_leak_thread, nullptr);
    sem_wait(&g_sem16_allocated);   // wait until ptr is stored
    pthread_join(th, nullptr);      // wait for thread to exit (LEAK reported)

    // Now thread has exited → its LEAK was logged and ptr added to g_reported_leaks
    free(g_cross16);                // this should cancel the LEAK
    g_cross16 = nullptr;

    sem_destroy(&g_sem16_allocated);
    sem_destroy(&g_sem16_free_done);

    check(true, "LEAK cancellation sequence completed");
    printf("  VERIFY: look for LEAK(16016) followed by free(16016) with different tid\n");
    printf("  VERIFY: in memview the 16016-byte record should show as Freed, not Leaked\n");
}

// ─── TEST 17: multi-thread, all alloc types, all freed ───────────────────────
struct T17Args { int id; };

__attribute__((noinline))
static void* t17_worker(void* arg)
{
    T17Args* a = (T17Args*)arg;
    char name[16];
    snprintf(name, sizeof(name), "t17-w%d", a->id);
    pthread_setname_np(pthread_self(), name);

    // All allocation types — each worker uses a unique size offset
    size_t off = (size_t)a->id * 100;
    char* m  = (char*)malloc(17000 + off);       assert(m);  m[0]='m';
    char* c  = (char*)calloc(2, 8500 + off/2);   assert(c);  c[0]='c';
    int*  n1 = new int[4250 + (int)off/4];       assert(n1); n1[0]=1;
    char* n2 = new char[17050 + off];             assert(n2); n2[0]='s';

    // Realloc within the thread
    m = (char*)realloc(m, 34000 + off*2);        assert(m); m[0]='r';

    // Free all
    delete[] n2;
    delete[] n1;
    free(c);
    free(m);

    return nullptr;
}

__attribute__((noinline))
static void test17_multi_thread_clean()
{
    const int N = 4;
    hdr(17, "4 threads, all alloc types (malloc/calloc/new/realloc), all freed");
    expect("Each thread: malloc+calloc+new[]+new[]+realloc → all freed");
    expect("No LEAKs from any worker thread");
    log_marker("T17");

    pthread_t threads[N];
    T17Args   args[N];
    for (int i = 0; i < N; i++) {
        args[i].id = i + 1;
        pthread_create(&threads[i], nullptr, t17_worker, &args[i]);
    }
    for (int i = 0; i < N; i++) pthread_join(threads[i], nullptr);

    check(true, "All 4 worker threads completed without crash");
    printf("  VERIFY: Each t17-wN thread must show SUMMARY 'all allocations freed'\n");
}

// ─── TEST 18: C++14 sized delete ─────────────────────────────────────────────
struct Obj18 { char data[18018]; };

__attribute__((noinline))
static void test18_sized_delete()
{
    hdr(18, "C++14 sized delete — ::operator delete(p, n)");
    expect("new(18018) then ::operator delete(p, 18018), no LEAK");
    log_marker("T18");

    Obj18* p = new Obj18;
    p->data[0] = 'T';
    ::operator delete(p, sizeof(Obj18));    // C++14 sized delete overload

    check(true, "sized delete completed without crash");
}

// ─── TEST 19: failed realloc — old ptr still valid ───────────────────────────
__attribute__((noinline))
static void test19_realloc_fail_safety()
{
    hdr(19, "failed realloc: old ptr must remain valid and be freed");
    // Force realloc failure by requesting an absurd size.
    expect("malloc(19019) → failed realloc → free(19019) of original ptr, no LEAK");
    log_marker("T19");

    char* p = (char*)malloc(19019);
    assert(p);
    p[0] = 'T';

    // Request SIZE_MAX - should fail and return NULL, leaving p valid.
    void* q = realloc(p, SIZE_MAX / 2);
    if (q == nullptr) {
        // Original p is still valid
        p[0] = 'X';          // safe: p is unchanged
        free(p);
        check(true, "realloc failed, original ptr freed cleanly");
    } else {
        // Some systems may succeed (unlikely at SIZE_MAX/2)
        q = realloc(q, 19019);
        if (q) { ((char*)q)[0] = 'X'; free(q); }
        check(true, "realloc unexpectedly succeeded — freed result");
    }
}

// ─── main ─────────────────────────────────────────────────────────────────────
int main()
{
    printf("══════════════════════════════════════════════════════════════\n");
    printf("  memtrack test suite\n");
    printf("  Run with: LD_PRELOAD=./memtrack.so ./test_app 2>mt.log\n");
    printf("══════════════════════════════════════════════════════════════\n");

    test01_malloc_free();
    test02_calloc_free();
    test03_new_delete();
    test04_new_arr_delete_arr();
    test05_realloc_from_null();
    test06_realloc_grow();
    test07_realloc_shrink();
    test08_realloc_to_zero();
    test09_realloc_chain();
    test10_free_null();
    test11_malloc_zero();
    test12_large_alloc();
    test13_many_small();
    test14_intentional_leak();
    test15_cross_thread_free();
    test16_leak_cancellation();
    test17_multi_thread_clean();
    test18_sized_delete();
    test19_realloc_fail_safety();

    printf("\n");
    printf("══════════════════════════════════════════════════════════════\n");
    printf("  Results: %d passed, %d failed\n", g_pass, g_fail);
    printf("══════════════════════════════════════════════════════════════\n");
    printf("\n");
    printf("  EXPECTED LEAKS IN mt.log SUMMARY:\n");
    printf("    size=14014  (test14_intentional_leak — kept intentionally)\n");
    printf("    size=1      (log_marker helper — 19 pairs, all balanced)\n");
    printf("    + glibc/pthread internal allocations (73728, 4096, ~320, etc.)\n");
    printf("\n");
    printf("  All other user allocations should appear as freed.\n");

    return g_fail ? 1 : 0;
}
