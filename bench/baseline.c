#include "libmemctl.h"

/*
 * One-mapping-per-allocation baseline, written to the published contract
 * so benchmark ratios have a fixed reference. This is not the submission
 * allocator. It is the strategy the problem statement describes as the
 * correctness-first starter: every object is its own anonymous mapping.
 */

#include <errno.h>
#include <stdalign.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

typedef union baseline_header {
    struct {
        size_t mapping_bytes;
        size_t requested_bytes;
    } meta;
    _Alignas(max_align_t) char force_align;
} baseline_header;

static int mapping_size(size_t payload, size_t *out)
{
    const long page_long = sysconf(_SC_PAGESIZE);
    size_t page;
    size_t total;

    if (page_long <= 0) {
        errno = EINVAL;
        return -1;
    }
    page = (size_t)page_long;
    if (payload > SIZE_MAX - sizeof(baseline_header) ||
        payload > SIZE_MAX - (page - 1u)) {
        errno = ENOMEM;
        return -1;
    }
    total = sizeof(baseline_header) + payload;
    if (total > SIZE_MAX - (page - 1u)) {
        errno = ENOMEM;
        return -1;
    }
    *out = (total + page - 1u) / page * page;
    return 0;
}

void *my_malloc(size_t size)
{
    const size_t payload = (size == 0u) ? 1u : size;
    size_t bytes;
    void *mapping;
    baseline_header *header;

    if (mapping_size(payload, &bytes) != 0) {
        return NULL;
    }
    mapping = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) {
        return NULL;
    }
    header = (baseline_header *)mapping;
    header->meta.mapping_bytes = bytes;
    header->meta.requested_bytes = size;
    return (unsigned char *)mapping + sizeof(*header);
}

void my_free(void *ptr)
{
    baseline_header *header;

    if (ptr == NULL) {
        return;
    }
    header = (baseline_header *)((unsigned char *)ptr - sizeof(*header));
    (void)munmap(header, header->meta.mapping_bytes);
}

void *my_realloc(void *ptr, size_t size)
{
    baseline_header *header;
    size_t old_size;
    size_t copy_size;
    void *fresh;

    if (ptr == NULL) {
        return my_malloc(size);
    }
    if (size == 0u) {
        my_free(ptr);
        return NULL;
    }
    header = (baseline_header *)((unsigned char *)ptr - sizeof(*header));
    old_size = header->meta.requested_bytes;
    fresh = my_malloc(size);
    if (fresh == NULL) {
        return NULL;
    }
    copy_size = (old_size < size) ? old_size : size;
    if (copy_size != 0u) {
        memcpy(fresh, ptr, copy_size);
    }
    my_free(ptr);
    return fresh;
}

void *my_calloc(size_t nmemb, size_t size)
{
    size_t total;
    void *ptr;

    if (size != 0u && nmemb > SIZE_MAX / size) {
        errno = ENOMEM;
        return NULL;
    }
    total = nmemb * size;
    ptr = my_malloc(total);
    if (ptr == NULL) {
        return NULL;
    }
    if (total != 0u) {
        memset(ptr, 0, total);
    }
    return ptr;
}
