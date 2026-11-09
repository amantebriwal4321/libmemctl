#ifndef LIBMEMCTL_STATS_H
#define LIBMEMCTL_STATS_H

/*
 * Optional instrumentation. Not part of the required my_* API.
 * Organizers do not need to call these.
 */
#include <stdint.h>

struct libmemctl_stats {
    /* alloc_ops, free_ops and tcache_hits are reserved. They stay 0:
     * a shared atomic on the fast path serialized cores in measurement. */
    uint64_t alloc_ops;
    uint64_t free_ops;
    uint64_t tcache_hits;
    uint64_t central_pops;
    uint64_t large_allocs;
    uint64_t mmap_calls;
    uint64_t munmap_calls;
    uint64_t bytes_mapped;
    uint64_t bytes_unmapped;
    uint64_t slabs_reclaimed;
};

void libmemctl_get_stats(struct libmemctl_stats *out);
void libmemctl_trim(void);

#endif /* LIBMEMCTL_STATS_H */
