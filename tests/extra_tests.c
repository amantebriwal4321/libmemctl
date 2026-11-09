#include "libmemctl.h"

#include <errno.h>
#include <pthread.h>
#include <stdalign.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

static void expect(int condition, const char *name)
{
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", name);
        ++failures;
    } else {
        printf("PASS %s\n", name);
    }
}

static int aligned(const void *ptr)
{
    return ((uintptr_t)ptr % _Alignof(max_align_t)) == 0u;
}

static void test_zero_size_distinct(void)
{
    enum { N = 128 };
    void *ptrs[N];
    size_t i;
    size_t j;
    int ok = 1;

    for (i = 0; i < N; ++i) {
        ptrs[i] = my_malloc(0);
        if (ptrs[i] == NULL || !aligned(ptrs[i])) {
            ok = 0;
        }
    }
    for (i = 0; i < N && ok; ++i) {
        for (j = i + 1; j < N; ++j) {
            if (ptrs[i] == ptrs[j]) {
                ok = 0;
            }
        }
    }
    for (i = 0; i < N; ++i) {
        my_free(ptrs[i]);
    }
    my_free(NULL);
    expect(ok, "malloc(0) is distinct, aligned, and freeable");
}

static void test_alignment_span(void)
{
    size_t size;
    int ok = 1;

    for (size = 1; size <= 4096u; size += 3u) {
        unsigned char *ptr = (unsigned char *)my_malloc(size);
        size_t i;
        if (ptr == NULL || !aligned(ptr)) {
            ok = 0;
            my_free(ptr);
            break;
        }
        for (i = 0; i < size; ++i) {
            ptr[i] = (unsigned char)(size + i);
        }
        for (i = 0; i < size; ++i) {
            if (ptr[i] != (unsigned char)(size + i)) {
                ok = 0;
            }
        }
        my_free(ptr);
        if (!ok) {
            break;
        }
    }
    expect(ok, "alignment and full-payload integrity for sizes 1..4096");
}

static void test_calloc_reuse_is_zero(void)
{
    unsigned char *keep = (unsigned char *)my_malloc(64);
    unsigned char *dirty = (unsigned char *)my_malloc(64);
    unsigned char *zeroed;
    size_t i;
    int ok;

    ok = keep != NULL && dirty != NULL;
    if (ok) {
        memset(dirty, 0xab, 64);
        my_free(dirty);
        zeroed = (unsigned char *)my_calloc(64, 1);
        ok = zeroed == dirty;
        for (i = 0; ok && i < 64u; ++i) {
            if (zeroed[i] != 0) {
                ok = 0;
            }
        }
        my_free(zeroed);
    }
    my_free(keep);

    errno = 0;
    {
        void *zero_count = my_calloc(0, 5);
        void *zero_size = my_calloc(5, 0);
        if (zero_count == NULL || zero_size == NULL) {
            ok = 0;
        }
        my_free(zero_count);
        my_free(zero_size);
    }
    if (my_calloc(SIZE_MAX, 2) != NULL || errno != ENOMEM) {
        ok = 0;
    }
    expect(ok, "calloc zeroes a reused block and rejects overflow");
}

static void test_realloc_in_place_and_move(void)
{
    unsigned char *ptr = (unsigned char *)my_malloc(20);
    unsigned char *grown;
    size_t i;
    int ok = ptr != NULL;

    if (ok) {
        memset(ptr, 0x11, 20);
        grown = (unsigned char *)my_realloc(ptr, 24);
        ok = grown == ptr;
        for (i = 0; ok && i < 20u; ++i) {
            if (grown[i] != 0x11) {
                ok = 0;
            }
        }
        ptr = grown;
    }
    if (ok) {
        grown = (unsigned char *)my_realloc(ptr, 100000);
        ok = grown != NULL;
        for (i = 0; ok && i < 20u; ++i) {
            if (grown[i] != 0x11) {
                ok = 0;
            }
        }
        ptr = grown;
    }
    if (ok) {
        errno = 0;
        grown = (unsigned char *)my_realloc(ptr, SIZE_MAX);
        ok = grown == NULL && errno == ENOMEM;
        for (i = 0; ok && i < 20u; ++i) {
            if (ptr[i] != 0x11) {
                ok = 0;
            }
        }
    }
    if (ok) {
        ok = my_realloc(ptr, 0) == NULL;
    }
    if (ok) {
        ptr = (unsigned char *)my_realloc(NULL, 32);
        ok = ptr != NULL && aligned(ptr);
        my_free(ptr);
    }
    expect(ok, "realloc in-place, move, failure preservation, and NULL");
}

static void test_large_roundtrip(void)
{
    const size_t size = 1024u * 1024u;
    unsigned char *ptr = (unsigned char *)my_malloc(size);
    size_t i;
    int ok = ptr != NULL && aligned(ptr);

    if (ok) {
        ptr[0] = 0x5a;
        ptr[size - 1u] = 0xa5;
        for (i = 1; i < 64u; ++i) {
            ptr[i] = (unsigned char)i;
        }
        ok = ptr[0] == 0x5a && ptr[size - 1u] == 0xa5 && ptr[63] == 63;
    }
    if (ok) {
        unsigned char *smaller = (unsigned char *)my_realloc(ptr, 100);
        ok = smaller != NULL && smaller[0] == 0x5a && smaller[63] == 63;
        ptr = smaller;
    }
    my_free(ptr);
    expect(ok, "1 MiB object, edge bytes, and shrink");
}

struct cross_arg {
    int id;
    void **slots;
    size_t count;
    pthread_barrier_t *barrier;
    int errors;
};

static void *cross_worker(void *argument)
{
    struct cross_arg *arg = (struct cross_arg *)argument;
    size_t i;

    for (i = 0; i < arg->count; ++i) {
        unsigned char *ptr = (unsigned char *)my_malloc(32u + (i % 200u));
        if (ptr == NULL || !aligned(ptr)) {
            ++arg->errors;
            arg->slots[i] = NULL;
            continue;
        }
        ptr[0] = (unsigned char)arg->id;
        ptr[1] = (unsigned char)i;
        arg->slots[i] = ptr;
    }
    pthread_barrier_wait(arg->barrier);
    {
        const int neighbor = (arg->id + 1) % 4;
        struct cross_arg *peer = arg - arg->id + neighbor;
        for (i = 0; i < peer->count; ++i) {
            unsigned char *ptr = (unsigned char *)peer->slots[i];
            if (ptr == NULL || ptr[0] != (unsigned char)neighbor ||
                ptr[1] != (unsigned char)i) {
                ++arg->errors;
            }
            my_free(ptr);
        }
    }
    return NULL;
}

static void test_cross_thread_free(void)
{
    enum { THREADS = 4, COUNT = 1500 };
    pthread_t threads[THREADS];
    struct cross_arg args[THREADS];
    pthread_barrier_t barrier;
    void *storage[THREADS][COUNT];
    int created = 0;
    int index;
    int errors = 0;

    if (pthread_barrier_init(&barrier, NULL, THREADS) != 0) {
        expect(0, "cross-thread free");
        return;
    }
    for (index = 0; index < THREADS; ++index) {
        args[index].id = index;
        args[index].slots = storage[index];
        args[index].count = COUNT;
        args[index].barrier = &barrier;
        args[index].errors = 0;
        if (pthread_create(&threads[index], NULL, cross_worker, &args[index]) != 0) {
            break;
        }
        ++created;
    }
    for (index = 0; index < created; ++index) {
        pthread_join(threads[index], NULL);
        errors += args[index].errors;
    }
    pthread_barrier_destroy(&barrier);
    expect(created == THREADS && errors == 0, "cross-thread free of 4 x 1500 blocks");
}

static void *rss_worker(void *argument)
{
    enum { N = 20000 };
    size_t i;
    (void)argument;
    for (i = 0; i < N; ++i) {
        void *ptr = my_malloc(256);
        if (ptr == NULL) {
            return (void *)1;
        }
        my_free(ptr);
    }
    return NULL;
}

static long vm_rss_kb(void)
{
    FILE *file = fopen("/proc/self/status", "r");
    char line[256];
    long rss = -1;

    if (file == NULL) {
        return -1;
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        if (sscanf(line, "VmRSS: %ld", &rss) == 1) {
            break;
        }
    }
    fclose(file);
    return rss;
}

static void test_thread_exit_reclaims(void)
{
    pthread_t thread;
    long before;
    long after;
    void *status = NULL;
    int ok;

    before = vm_rss_kb();
    ok = pthread_create(&thread, NULL, rss_worker, NULL) == 0;
    if (ok) {
        ok = pthread_join(thread, &status) == 0 && status == NULL;
    }
    after = vm_rss_kb();
    /* 20k x 256 B would be several megabytes if slabs were abandoned. */
    ok = ok && before > 0 && after > 0 && (after - before) < 2048;
    printf("  rss before %ld kB, after join %ld kB\n", before, after);
    expect(ok, "worker thread exit returns cached slabs");
}

static void test_neighbor_isolation(void)
{
    enum { N = 64, SIZE = 48 };
    unsigned char *ptrs[N];
    size_t i;
    int ok = 1;

    for (i = 0; i < N; ++i) {
        ptrs[i] = (unsigned char *)my_malloc(SIZE);
        if (ptrs[i] == NULL) {
            ok = 0;
            break;
        }
        memset(ptrs[i], (unsigned char)(0x30 + i), SIZE);
    }
    for (i = 0; ok && i < N; ++i) {
        size_t b;
        for (b = 0; b < SIZE; ++b) {
            if (ptrs[i][b] != (unsigned char)(0x30 + i)) {
                ok = 0;
            }
        }
    }
    for (i = 0; i < N; ++i) {
        my_free(ptrs[i]);
    }
    expect(ok, "adjacent live blocks do not overlap");
}

int main(void)
{
    test_zero_size_distinct();
    test_alignment_span();
    test_calloc_reuse_is_zero();
    test_realloc_in_place_and_move();
    test_large_roundtrip();
    test_neighbor_isolation();
    test_cross_thread_free();
    test_thread_exit_reclaims();
    if (failures != 0) {
        fprintf(stderr, "\n%d extra test(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    printf("\nExtra tests passed\n");
    return EXIT_SUCCESS;
}
