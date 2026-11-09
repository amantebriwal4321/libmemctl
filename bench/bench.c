#include "libmemctl.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#ifdef LIBMEMCTL_STATS
#include "libmemctl_stats.h"
#endif

static volatile unsigned char sink_byte;
static int g_quick;

static unsigned char **mmap_alloc_ptrs(size_t count);
static void munmap_ptrs(unsigned char **ptrs, size_t count);

static double seconds_between(const struct timespec *start, const struct timespec *finish)
{
    return (double)(finish->tv_sec - start->tv_sec) +
           (double)(finish->tv_nsec - start->tv_nsec) / 1e9;
}

static uint32_t xorshift(uint32_t *state)
{
    uint32_t value = *state;
    if (value == 0u) {
        value = 0x9e3779b9u;
    }
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    return value;
}

static size_t choose_size(uint32_t random_value)
{
    static const size_t sizes[] = {
        1u, 8u, 16u, 24u, 32u, 48u, 64u, 96u, 128u, 192u, 256u,
        512u, 1024u, 2048u, 4096u, 8192u, 16384u
    };
    return sizes[random_value % (uint32_t)(sizeof(sizes) / sizeof(sizes[0]))];
}

static int read_vm(long *size_kb, long *rss_kb, long *hwm_kb, long *peak_kb)
{
    FILE *file = fopen("/proc/self/status", "r");
    char line[256];

    if (file == NULL) {
        return -1;
    }
    *size_kb = *rss_kb = *hwm_kb = *peak_kb = -1;
    while (fgets(line, sizeof(line), file) != NULL) {
        long value = 0;
        if (sscanf(line, "VmSize: %ld", &value) == 1) {
            *size_kb = value;
        } else if (sscanf(line, "VmRSS: %ld", &value) == 1) {
            *rss_kb = value;
        } else if (sscanf(line, "VmHWM: %ld", &value) == 1) {
            *hwm_kb = value;
        } else if (sscanf(line, "VmPeak: %ld", &value) == 1) {
            *peak_kb = value;
        }
    }
    fclose(file);
    return 0;
}

static int cmp_double(const void *left, const void *right)
{
    const double a = *(const double *)left;
    const double b = *(const double *)right;
    return (a > b) - (a < b);
}

static double median(double *values, size_t count)
{
    qsort(values, count, sizeof(*values), cmp_double);
    if ((count % 2u) == 1u) {
        return values[count / 2u];
    }
    return 0.5 * (values[count / 2u - 1u] + values[count / 2u]);
}

static double pair_once(size_t ops, size_t size)
{
    struct timespec start;
    struct timespec finish;
    size_t i;

    clock_gettime(CLOCK_MONOTONIC, &start);
    for (i = 0; i < ops; ++i) {
        unsigned char *ptr = (unsigned char *)my_malloc(size);
        if (ptr == NULL) {
            return -1.0;
        }
        ptr[0] = (unsigned char)i;
        sink_byte = ptr[0];
        my_free(ptr);
    }
    clock_gettime(CLOCK_MONOTONIC, &finish);
    return seconds_between(&start, &finish);
}

struct mix_slot {
    unsigned char *ptr;
    size_t size;
    unsigned char tag;
};

struct mix_job {
    uint32_t seed;
    size_t ops;
    size_t slots;
    int errors;
    double seconds;
    double batch_p50_ns;
    double batch_p99_ns;
};

static double percentile_sorted(double *values, size_t count, double pct)
{
    size_t index;
    if (count == 0u) {
        return 0.0;
    }
    index = (size_t)(pct * (double)(count - 1u));
    return values[index];
}

static void *mix_worker(void *argument)
{
    struct mix_job *job = (struct mix_job *)argument;
    struct mix_slot *slots = (struct mix_slot *)calloc(job->slots, sizeof(*slots));
    enum { BATCH = 1024 };
    double *batches = NULL;
    size_t batch_count = 0;
    size_t batch_cap = (job->ops / BATCH) + 2u;
    struct timespec origin;
    struct timespec mark;
    size_t op;
    uint32_t state = job->seed;

    if (slots == NULL) {
        job->errors = 1;
        return NULL;
    }
    batches = (double *)calloc(batch_cap, sizeof(*batches));
    if (batches == NULL) {
        free(slots);
        job->errors = 1;
        return NULL;
    }

    clock_gettime(CLOCK_MONOTONIC, &origin);
    clock_gettime(CLOCK_MONOTONIC, &mark);
    for (op = 0; op < job->ops; ++op) {
        const uint32_t roll = xorshift(&state);
        const size_t index = (size_t)(roll % (uint32_t)job->slots);
        struct mix_slot *slot = &slots[index];
        const unsigned action = roll % 100u;

        if (slot->ptr == NULL || action < 55u) {
            const size_t size = choose_size(roll >> 8);
            const unsigned char tag = (unsigned char)(roll >> 24);
            unsigned char *ptr = (unsigned char *)my_malloc(size);
            if (ptr == NULL) {
                ++job->errors;
            } else {
                ptr[0] = tag;
                if (size > 1u) {
                    ptr[size - 1u] = tag;
                }
                sink_byte = ptr[0];
                if (slot->ptr != NULL) {
                    my_free(slot->ptr);
                }
                slot->ptr = ptr;
                slot->size = size;
                slot->tag = tag;
            }
        } else if (action < 80u) {
            const size_t size = choose_size(roll >> 8);
            unsigned char *ptr = (unsigned char *)my_realloc(slot->ptr, size);
            if (ptr == NULL) {
                ++job->errors;
            } else {
                ptr[0] = slot->tag;
                if (size > 1u) {
                    ptr[size - 1u] = slot->tag;
                }
                slot->ptr = ptr;
                slot->size = size;
            }
        } else {
            if (slot->ptr[0] != slot->tag) {
                ++job->errors;
            }
            my_free(slot->ptr);
            slot->ptr = NULL;
        }

        if (((op + 1u) % BATCH) == 0u && batch_count < batch_cap) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            batches[batch_count++] = seconds_between(&mark, &now) * 1e9 / (double)BATCH;
            mark = now;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &mark);
    job->seconds = seconds_between(&origin, &mark);

    for (op = 0; op < job->slots; ++op) {
        my_free(slots[op].ptr);
    }
    if (batch_count > 0u) {
        qsort(batches, batch_count, sizeof(*batches), cmp_double);
        job->batch_p50_ns = percentile_sorted(batches, batch_count, 0.50);
        job->batch_p99_ns = percentile_sorted(batches, batch_count, 0.99);
    }
    free(batches);
    free(slots);
    return NULL;
}

static void run_pairs(void)
{
    const int runs = g_quick ? 1 : 5;
    const int warmups = 1;
    const size_t ops = g_quick ? 200000u : 1000000u;
    const size_t size = 64u;
    double samples[5];
    int run;
    double med;
    double best;
    double worst;

    for (run = 0; run < warmups; ++run) {
        if (pair_once(ops / 4u, size) < 0.0) {
            printf("pair_malloc_free status FAIL\n");
            return;
        }
    }
    best = 1e300;
    worst = 0.0;
    for (run = 0; run < runs; ++run) {
        samples[run] = pair_once(ops, size);
        if (samples[run] < 0.0) {
            printf("pair_malloc_free status FAIL\n");
            return;
        }
        if (samples[run] < best) {
            best = samples[run];
        }
        if (samples[run] > worst) {
            worst = samples[run];
        }
    }
    med = median(samples, (size_t)runs);
    printf("pair_malloc_free size %zu ops %zu runs %d median_s %.6f "
           "ops_per_s %.0f best_ops_per_s %.0f worst_ops_per_s %.0f\n",
           size, ops, runs, med, (double)ops / med, (double)ops / best,
           (double)ops / worst);
}

static void run_mixed(int threads)
{
    const int runs = g_quick ? 1 : 5;
    const size_t ops_per_thread = 80000u;
    const size_t slots = 1024u;
    double samples[5];
    double p99[5];
    int run;

    for (run = 0; run < runs; ++run) {
        pthread_t *handles = (pthread_t *)calloc((size_t)threads, sizeof(*handles));
        struct mix_job *jobs = (struct mix_job *)calloc((size_t)threads, sizeof(*jobs));
        int index;
        int created = 0;
        int errors = 0;
        double elapsed = 0.0;
        double worst_p99 = 0.0;
        uint64_t total_ops;

        if (handles == NULL || jobs == NULL) {
            free(handles);
            free(jobs);
            printf("mixed threads %d status FAIL\n", threads);
            return;
        }
        for (index = 0; index < threads; ++index) {
            jobs[index].seed = 0x6a09e667u ^ (uint32_t)(index + 1 + run * 17) * 0x9e3779b9u;
            jobs[index].ops = ops_per_thread;
            jobs[index].slots = slots;
            if (pthread_create(&handles[index], NULL, mix_worker, &jobs[index]) != 0) {
                break;
            }
            ++created;
        }
        for (index = 0; index < created; ++index) {
            pthread_join(handles[index], NULL);
            errors += jobs[index].errors;
            if (jobs[index].seconds > elapsed) {
                elapsed = jobs[index].seconds;
            }
            if (jobs[index].batch_p99_ns > worst_p99) {
                worst_p99 = jobs[index].batch_p99_ns;
            }
        }
        free(handles);
        free(jobs);
        if (created != threads || errors != 0 || elapsed <= 0.0) {
            printf("mixed threads %d status FAIL created %d errors %d\n",
                   threads, created, errors);
            return;
        }
        total_ops = (uint64_t)ops_per_thread * (uint64_t)threads;
        samples[run] = (double)total_ops / elapsed;
        p99[run] = worst_p99;
    }
    printf("mixed threads %d ops_per_thread %zu slots %zu runs %d "
           "median_ops_per_s %.0f median_batch_p99_ns %.1f\n",
           threads, ops_per_thread, slots, runs, median(samples, (size_t)runs),
           median(p99, (size_t)runs));
}

static void run_space(void)
{
    enum { N = 20000 };
    const size_t size = 128u;
    unsigned char **ptrs;
    long base_size = 0;
    long base_rss = 0;
    long base_hwm = 0;
    long base_peak = 0;
    long live_size = 0;
    long live_rss = 0;
    long live_hwm = 0;
    long live_peak = 0;
    long done_size = 0;
    long done_rss = 0;
    long done_hwm = 0;
    long done_peak = 0;
    long trim_rss = -1;
    size_t i;
    size_t live = 0;
    const double requested_mb = (double)N * (double)size / (1024.0 * 1024.0);

    ptrs = (unsigned char **)mmap_alloc_ptrs(N);
    if (ptrs == NULL) {
        printf("space status FAIL\n");
        return;
    }
    if (read_vm(&base_size, &base_rss, &base_hwm, &base_peak) != 0) {
        printf("space status FAIL\n");
        return;
    }
    for (i = 0; i < N; ++i) {
        ptrs[i] = (unsigned char *)my_malloc(size);
        if (ptrs[i] == NULL) {
            break;
        }
        memset(ptrs[i], 0x5c, size);
        ++live;
    }
    (void)read_vm(&live_size, &live_rss, &live_hwm, &live_peak);
    for (i = 0; i < live; ++i) {
        my_free(ptrs[i]);
    }
    (void)read_vm(&done_size, &done_rss, &done_hwm, &done_peak);
#ifdef LIBMEMCTL_STATS
    libmemctl_trim();
    {
        long trim_size = 0;
        long trim_hwm = 0;
        long trim_peak = 0;
        (void)read_vm(&trim_size, &trim_rss, &trim_hwm, &trim_peak);
        (void)trim_size;
        (void)trim_hwm;
        (void)trim_peak;
    }
#endif
    printf("space blocks %zu payload_bytes %zu requested_MiB %.3f "
           "rss_base_kB %ld rss_live_kB %ld rss_after_free_kB %ld "
           "rss_after_trim_kB %ld vm_live_kB %ld vm_after_free_kB %ld "
           "hwm_kB %ld live_over_requested %.3f\n",
           live, live * size, requested_mb,
           base_rss, live_rss, done_rss, trim_rss,
           live_size - base_size, done_size - base_size, live_hwm,
           (requested_mb > 0.0)
               ? ((double)(live_rss - base_rss) / 1024.0) / requested_mb
               : 0.0);
    munmap_ptrs(ptrs, N);
}

static unsigned char **mmap_alloc_ptrs(size_t count)
{
    const size_t bytes = count * sizeof(unsigned char *);
    void *mapping = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) {
        return NULL;
    }
    memset(mapping, 0, bytes);
    return (unsigned char **)mapping;
}

static void munmap_ptrs(unsigned char **ptrs, size_t count)
{
    if (ptrs != NULL) {
        (void)munmap(ptrs, count * sizeof(unsigned char *));
    }
}

int main(int argc, char **argv)
{
    long page = sysconf(_SC_PAGESIZE);

    g_quick = (argc > 1 && strcmp(argv[1], "quick") == 0);
    printf("bench page_size %ld max_align %zu quick %d\n",
           page, _Alignof(max_align_t), g_quick);
    run_space();
    run_pairs();
    run_mixed(1);
    if (!g_quick) {
        run_mixed(2);
        run_mixed(4);
        run_mixed(8);
    }
#ifdef LIBMEMCTL_STATS
    {
        struct libmemctl_stats stats;
        libmemctl_get_stats(&stats);
        printf("stats alloc_ops %" PRIu64 " free_ops %" PRIu64
               " tcache_hits %" PRIu64 " central_pops %" PRIu64
               " large_allocs %" PRIu64 " mmap_calls %" PRIu64
               " munmap_calls %" PRIu64 " bytes_mapped %" PRIu64
               " bytes_unmapped %" PRIu64 " slabs_reclaimed %" PRIu64 "\n",
               stats.alloc_ops, stats.free_ops, stats.tcache_hits,
               stats.central_pops, stats.large_allocs, stats.mmap_calls,
               stats.munmap_calls, stats.bytes_mapped, stats.bytes_unmapped,
               stats.slabs_reclaimed);
    }
#endif
    return 0;
}
