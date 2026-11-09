#include "libmemctl.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * Throughput reference only. glibc malloc(0) is not required to follow
 * the libmemctl zero-size rule, so the benchmark calls use non-zero sizes.
 */

void *my_malloc(size_t size)
{
    if (size == 0u) {
        size = 1u;
    }
    return malloc(size);
}

void my_free(void *ptr)
{
    free(ptr);
}

void *my_realloc(void *ptr, size_t size)
{
    if (ptr != NULL && size == 0u) {
        free(ptr);
        return NULL;
    }
    if (size == 0u) {
        size = 1u;
    }
    return realloc(ptr, size);
}

void *my_calloc(size_t nmemb, size_t size)
{
    if (size != 0u && nmemb > SIZE_MAX / size) {
        errno = ENOMEM;
        return NULL;
    }
    if (nmemb == 0u || size == 0u) {
        return malloc(1u);
    }
    return calloc(nmemb, size);
}
