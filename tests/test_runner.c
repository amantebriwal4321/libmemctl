#include "libmemctl.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FRAGMENTATION_SLOTS 2048U
#define FRAGMENTATION_OPERATIONS 50000U
#define THREAD_COUNT 8U
#define THREAD_SLOTS 48U
#define THREAD_OPERATIONS 20000U

typedef struct allocation_slot {
    unsigned char *ptr;
    size_t size;
    unsigned char tag;
} allocation_slot;

typedef struct thread_context {
    uint32_t seed;
    unsigned int errors;
    uint64_t operations;
} thread_context;

static uint32_t next_random(uint32_t *state)
{
    uint32_t value = *state;

    if (value == 0U) {
        value = UINT32_C(0x9e3779b9);
    }
    value ^= value << 13U;
    value ^= value >> 17U;
    value ^= value << 5U;
    *state = value;
    return value;
}

static bool pointer_is_aligned(const void *ptr)
{
    return ((uintptr_t)ptr % (uintptr_t)_Alignof(max_align_t)) == 0U;
}

static void fill_pattern(unsigned char *ptr, size_t size, unsigned char tag)
{
    size_t index;

    for (index = 0U; index < size; ++index) {
        ptr[index] = tag;
    }
}

static bool pattern_is_intact(const unsigned char *ptr,
                              size_t size,
                              unsigned char tag)
{
    size_t index;

    for (index = 0U; index < size; ++index) {
        if (ptr[index] != tag) {
            return false;
        }
    }
    return true;
}

static double elapsed_seconds(const struct timespec *start,
                              const struct timespec *finish)
{
    const double seconds = (double)(finish->tv_sec - start->tv_sec);
    const double nanoseconds = (double)(finish->tv_nsec - start->tv_nsec);
    return seconds + (nanoseconds / 1000000000.0);
}

static bool test_basic_and_alignment(void)
{
    static const size_t sizes[] = {
        1U, 2U, 7U, 8U, 15U, 16U, 17U, 31U, 32U, 33U,
        63U, 64U, 65U, 127U, 128U, 129U, 1024U, 4096U
    };
    size_t index;

    my_free(NULL);

    for (index = 0U; index < (sizeof(sizes) / sizeof(sizes[0])); ++index) {
        const size_t size = sizes[index];
        const unsigned char tag = (unsigned char)(index + 1U);
        unsigned char *ptr = (unsigned char *)my_malloc(size);

        if (ptr == NULL || !pointer_is_aligned(ptr)) {
            my_free(ptr);
            return false;
        }

        fill_pattern(ptr, size, tag);
        if (!pattern_is_intact(ptr, size, tag)) {
            my_free(ptr);
            return false;
        }
        my_free(ptr);
    }

    return true;
}

static bool test_calloc_and_overflow(void)
{
    const size_t count = 257U;
    const size_t width = 13U;
    const size_t total = count * width;
    unsigned char *zeroed = (unsigned char *)my_calloc(count, width);
    unsigned char *zero_count;
    size_t index;

    if (zeroed == NULL || !pointer_is_aligned(zeroed)) {
        my_free(zeroed);
        return false;
    }
    for (index = 0U; index < total; ++index) {
        if (zeroed[index] != 0U) {
            my_free(zeroed);
            return false;
        }
    }
    my_free(zeroed);

    zero_count = (unsigned char *)my_calloc(0U, SIZE_MAX);
    if (zero_count == NULL || !pointer_is_aligned(zero_count)) {
        my_free(zero_count);
        return false;
    }
    my_free(zero_count);

    errno = 0;
    {
        void *overflow = my_calloc(SIZE_MAX, 2U);
        if (overflow != NULL || errno != ENOMEM) {
            my_free(overflow);
            return false;
        }
    }

    errno = 0;
    {
        void *too_large = my_malloc(SIZE_MAX);
        if (too_large != NULL || errno != ENOMEM) {
            my_free(too_large);
            return false;
        }
    }

    return true;
}

static bool test_realloc_contract(void)
{
    const size_t initial_size = 73U;
    unsigned char *ptr = (unsigned char *)my_malloc(initial_size);
    unsigned char *resized;
    size_t index;

    if (ptr == NULL) {
        return false;
    }
    fill_pattern(ptr, initial_size, 0x6dU);

    resized = (unsigned char *)my_realloc(ptr, 4097U);
    if (resized == NULL || !pattern_is_intact(resized, initial_size, 0x6dU)) {
        my_free(resized == NULL ? ptr : resized);
        return false;
    }
    fill_pattern(resized + initial_size, 4097U - initial_size, 0x31U);

    ptr = (unsigned char *)my_realloc(resized, 11U);
    if (ptr == NULL) {
        my_free(resized);
        return false;
    }
    if (!pattern_is_intact(ptr, 11U, 0x6dU)) {
        my_free(ptr);
        return false;
    }

    errno = 0;
    resized = (unsigned char *)my_realloc(ptr, SIZE_MAX);
    if (resized != NULL || errno != ENOMEM || !pattern_is_intact(ptr, 11U, 0x6dU)) {
        my_free(resized);
        my_free(ptr);
        return false;
    }

    for (index = 0U; index < 11U; ++index) {
        ptr[index] = (unsigned char)index;
    }

    resized = (unsigned char *)my_realloc(ptr, 0U);
    if (resized != NULL) {
        my_free(resized);
        return false;
    }

    resized = (unsigned char *)my_realloc(NULL, 64U);
    if (resized == NULL) {
        return false;
    }
    my_free(resized);
    return true;
}

static bool test_boundary_integrity(void)
{
    const size_t guard_size = 96U;
    unsigned char *left = (unsigned char *)my_malloc(guard_size);
    unsigned char *middle = (unsigned char *)my_malloc(1U);
    unsigned char *right = (unsigned char *)my_malloc(guard_size);
    bool intact;

    if (left == NULL || middle == NULL || right == NULL) {
        my_free(left);
        my_free(middle);
        my_free(right);
        return false;
    }

    fill_pattern(left, guard_size, 0xa5U);
    fill_pattern(right, guard_size, 0x5aU);
    middle[0] = 0xc3U;
    left[guard_size - 1U] = 0xa6U;
    right[0] = 0x5bU;
    right[guard_size - 1U] = 0x5cU;

    intact = (middle[0] == 0xc3U) &&
             (left[guard_size - 1U] == 0xa6U) &&
             (right[0] == 0x5bU) &&
             (right[guard_size - 1U] == 0x5cU) &&
             pattern_is_intact(left, guard_size - 1U, 0xa5U) &&
             pattern_is_intact(right + 1U, guard_size - 2U, 0x5aU);

    my_free(left);
    my_free(middle);
    my_free(right);
    return intact;
}

static size_t choose_size(uint32_t random_value)
{
    static const size_t size_classes[] = {
        1U, 2U, 7U, 15U, 16U, 31U, 32U, 63U, 64U, 65U,
        127U, 128U, 129U, 255U, 256U, 257U, 511U, 512U,
        513U, 1023U, 1024U, 2048U, 4095U, 4096U, 4097U,
        8192U, 16384U
    };
    const size_t count = sizeof(size_classes) / sizeof(size_classes[0]);
    return size_classes[(size_t)random_value % count];
}

static bool test_fragmentation_stress(void)
{
    allocation_slot *slots = (allocation_slot *)calloc(FRAGMENTATION_SLOTS,
                                                       sizeof(*slots));
    uint32_t random_state = UINT32_C(0x1234abcd);
    size_t operation;
    bool success = true;

    if (slots == NULL) {
        return false;
    }

    for (operation = 0U; operation < FRAGMENTATION_OPERATIONS; ++operation) {
        const uint32_t random_value = next_random(&random_state);
        const size_t slot_index = (size_t)(random_value % FRAGMENTATION_SLOTS);
        allocation_slot *slot = &slots[slot_index];

        if (slot->ptr == NULL) {
            const size_t new_size = choose_size(random_value >> 8U);
            const unsigned char tag = (unsigned char)(random_value >> 24U);
            unsigned char *ptr = (unsigned char *)my_malloc(new_size);

            if (ptr == NULL || !pointer_is_aligned(ptr)) {
                my_free(ptr);
                success = false;
                break;
            }
            fill_pattern(ptr, new_size, tag);
            slot->ptr = ptr;
            slot->size = new_size;
            slot->tag = tag;
        } else if ((random_value % 100U) < 28U) {
            if (!pattern_is_intact(slot->ptr, slot->size, slot->tag)) {
                success = false;
                break;
            }
            my_free(slot->ptr);
            slot->ptr = NULL;
            slot->size = 0U;
        } else if ((random_value % 100U) < 62U) {
            const size_t new_size = choose_size(random_value >> 8U);
            const size_t preserve = (slot->size < new_size) ? slot->size : new_size;
            unsigned char *new_ptr;

            if (!pattern_is_intact(slot->ptr, slot->size, slot->tag)) {
                success = false;
                break;
            }

            new_ptr = (unsigned char *)my_realloc(slot->ptr, new_size);
            if (new_ptr != NULL) {
                if (!pointer_is_aligned(new_ptr) ||
                    !pattern_is_intact(new_ptr, preserve, slot->tag)) {
                    my_free(new_ptr);
                    slot->ptr = NULL;
                    success = false;
                    break;
                }
                slot->ptr = new_ptr;
                slot->size = new_size;
                fill_pattern(slot->ptr, slot->size, slot->tag);
            } else if (!pattern_is_intact(slot->ptr, slot->size, slot->tag)) {
                success = false;
                break;
            }
        } else if (!pattern_is_intact(slot->ptr, slot->size, slot->tag)) {
            success = false;
            break;
        }
    }

    for (operation = 0U; operation < FRAGMENTATION_SLOTS; ++operation) {
        if (slots[operation].ptr != NULL) {
            if (!pattern_is_intact(slots[operation].ptr,
                                   slots[operation].size,
                                   slots[operation].tag)) {
                success = false;
            }
            my_free(slots[operation].ptr);
        }
    }
    free(slots);
    return success;
}

static void *thread_worker(void *argument)
{
    thread_context *context = (thread_context *)argument;
    allocation_slot slots[THREAD_SLOTS] = {{0}};
    size_t operation;

    for (operation = 0U; operation < THREAD_OPERATIONS; ++operation) {
        const uint32_t random_value = next_random(&context->seed);
        const size_t slot_index = (size_t)(random_value % THREAD_SLOTS);
        allocation_slot *slot = &slots[slot_index];
        const uint32_t action = random_value % 100U;

        if (slot->ptr == NULL) {
            const size_t size = choose_size(random_value >> 8U) % 4097U + 1U;
            const unsigned char tag = (unsigned char)(random_value >> 24U);
            unsigned char *ptr = (unsigned char *)my_malloc(size);

            if (ptr == NULL || !pointer_is_aligned(ptr)) {
                ++context->errors;
                my_free(ptr);
                continue;
            }
            fill_pattern(ptr, size, tag);
            slot->ptr = ptr;
            slot->size = size;
            slot->tag = tag;
        } else if (action < 25U) {
            if (!pattern_is_intact(slot->ptr, slot->size, slot->tag)) {
                ++context->errors;
            }
            my_free(slot->ptr);
            slot->ptr = NULL;
            slot->size = 0U;
        } else if (action < 55U) {
            const size_t new_size = choose_size(random_value >> 8U) % 4097U + 1U;
            const size_t preserve = (slot->size < new_size) ? slot->size : new_size;
            unsigned char *new_ptr;

            if (!pattern_is_intact(slot->ptr, slot->size, slot->tag)) {
                ++context->errors;
            }
            new_ptr = (unsigned char *)my_realloc(slot->ptr, new_size);
            if (new_ptr != NULL) {
                if (!pointer_is_aligned(new_ptr) ||
                    !pattern_is_intact(new_ptr, preserve, slot->tag)) {
                    ++context->errors;
                }
                slot->ptr = new_ptr;
                slot->size = new_size;
                fill_pattern(slot->ptr, slot->size, slot->tag);
            } else if (!pattern_is_intact(slot->ptr, slot->size, slot->tag)) {
                ++context->errors;
            }
        } else if (!pattern_is_intact(slot->ptr, slot->size, slot->tag)) {
            ++context->errors;
        }

        ++context->operations;
    }

    for (operation = 0U; operation < THREAD_SLOTS; ++operation) {
        if (slots[operation].ptr != NULL) {
            if (!pattern_is_intact(slots[operation].ptr,
                                   slots[operation].size,
                                   slots[operation].tag)) {
                ++context->errors;
            }
            my_free(slots[operation].ptr);
        }
    }

    return NULL;
}

static bool test_multithreaded_contention(void)
{
    pthread_t threads[THREAD_COUNT];
    thread_context contexts[THREAD_COUNT];
    struct timespec start;
    struct timespec finish;
    size_t created = 0U;
    size_t index;
    uint64_t total_operations = 0U;
    unsigned int total_errors = 0U;
    double seconds;

    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        return false;
    }

    for (index = 0U; index < THREAD_COUNT; ++index) {
        contexts[index].seed = UINT32_C(0x6a09e667) ^
                               (uint32_t)(index + 1U) * UINT32_C(0x9e3779b9);
        contexts[index].errors = 0U;
        contexts[index].operations = 0U;

        if (pthread_create(&threads[index], NULL, thread_worker, &contexts[index]) != 0) {
            break;
        }
        ++created;
    }

    for (index = 0U; index < created; ++index) {
        if (pthread_join(threads[index], NULL) != 0) {
            ++total_errors;
        }
        total_errors += contexts[index].errors;
        total_operations += contexts[index].operations;
    }

    if (clock_gettime(CLOCK_MONOTONIC, &finish) != 0) {
        return false;
    }

    if (created != THREAD_COUNT || total_errors != 0U) {
        (void)fprintf(stderr,
                      "  thread details: created=%zu/%u errors=%u\n",
                      created,
                      THREAD_COUNT,
                      total_errors);
        return false;
    }

    seconds = elapsed_seconds(&start, &finish);
    (void)printf("  threaded workload: %" PRIu64 " operations in %.3f s (%.0f ops/s)\n",
                 total_operations,
                 seconds,
                 (seconds > 0.0) ? ((double)total_operations / seconds) : 0.0);
    return true;
}

typedef bool (*test_function)(void);

static bool run_test(const char *name, test_function function)
{
    const bool passed = function();
    (void)printf("[%s] %s\n", passed ? "PASS" : "FAIL", name);
    return passed;
}

int main(void)
{
    unsigned int passed = 0U;
    unsigned int failed = 0U;

    if (run_test("basic allocation, free, and max_align_t alignment",
                 test_basic_and_alignment)) {
        ++passed;
    } else {
        ++failed;
    }

    if (run_test("calloc zeroing and multiplication overflow",
                 test_calloc_and_overflow)) {
        ++passed;
    } else {
        ++failed;
    }

    if (run_test("realloc growth, shrink, and failure preservation",
                 test_realloc_contract)) {
        ++passed;
    } else {
        ++failed;
    }

    if (run_test("allocation boundary and neighboring-block integrity",
                 test_boundary_integrity)) {
        ++passed;
    } else {
        ++failed;
    }

    if (run_test("mixed-size fragmentation and reuse stress",
                 test_fragmentation_stress)) {
        ++passed;
    } else {
        ++failed;
    }

    if (run_test("multithreaded allocation contention",
                 test_multithreaded_contention)) {
        ++passed;
    } else {
        ++failed;
    }

    (void)printf("\nSummary: %u passed, %u failed\n", passed, failed);
    return (failed == 0U) ? EXIT_SUCCESS : EXIT_FAILURE;
}
