/*
 * test_app.cpp – exercises malloc, calloc, realloc, new, new[], and threads
 * to verify memtrack.so output.
 */
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <stdio.h>

static void* thread_fn(void* arg)
{
    int id = *(int*)arg;
    printf("[test] thread %d starting allocations\n", id);

    void* a = malloc(128);
    void* b = calloc(4, 64);
    void* c = realloc(a, 512);

    int*  arr = new int[32];
    char* str = new char[256];

    delete[] arr;
    delete[] str;
    free(b);
    // Intentional leak in thread 1: c is not freed
    if (id != 1)
        free(c);

    return nullptr;
}

int main()
{
    printf("[test] main thread allocations\n");

    void* p1 = malloc(1024);
    void* p2 = calloc(8, 128);
    void* p3 = realloc(p1, 2048);
    double* d = new double[16];

    free(p2);
    free(p3);
    delete[] d;

    const int N = 3;
    pthread_t threads[N];
    int ids[N];

    for (int i = 0; i < N; i++) {
        ids[i] = i + 1;
        pthread_create(&threads[i], nullptr, thread_fn, &ids[i]);
    }
    for (int i = 0; i < N; i++)
        pthread_join(threads[i], nullptr);

    printf("[test] done\n");
    return 0;
}
