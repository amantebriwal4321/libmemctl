#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "libmemctl.h"
#include "libmemctl_stats.h"

/*
 * libmemctl — size-class slab allocator with per-thread caches.
 *
 * Invariants (held whenever a bin mutex is not mid-update):
 *   1. Every live or cached small object carries a header immediately
 *      before the user pointer. header->meta is the owning slab.
 *      header->requested is the size passed to my_malloc / my_realloc.
 *   2. A slab is on its size-class partial list if and only if
 *      free_count > 0. free_count is the number of blocks on that
 *      slab's freelist (not live, not in any thread cache).
 *   3. A slab mapping is munmap'd only when every block is back on that
 *      slab's freelist (nothing live, nothing sitting in a thread cache)
 *      and has_been_used is set. Caches flush when they overflow, when
 *      the owning thread exits, and from libmemctl_trim().
 *   4. Large objects (above the largest size class) are their own
 *      mmap. header->meta bit 0 is set and the mapping size is meta >> 2.
 *      The header sits at the start of that mapping.
 *   5. User pointers are aligned to _Alignof(max_align_t). Size-class
 *      payloads are multiples of that alignment, and headers are too.
 *   6. Bin mutexes are not nested. Thread caches are touched only by
 *      their own thread. Cross-thread free is safe: the freeing thread
 *      caches the block or returns it to the slab.
 *
 * malloc(0) stores requested == 0 and reserves a real one-byte-or-more
 * size-class block, so distinct calls return distinct freeable pointers.
 */

#include <errno.h>
#include <pthread.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define SMALL_MAX 32768u
#define MAX_CLASS 48
#define TCACHE_CAP 32
#define REFILL_BATCH 16
#define SLAB_MAGIC 0x51AB51ABu

#if defined(__GNUC__)
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define LIKELY(x) (x)
#define UNLIKELY(x) (x)
#endif

typedef union hdr {
    struct {
        uintptr_t meta;
        size_t requested;
    } f;
    _Alignas(max_align_t) char force_align;
} hdr;

_Static_assert(sizeof(uintptr_t) == sizeof(size_t), "size_t matches uintptr_t");
_Static_assert(sizeof(hdr) % sizeof(void *) == 0, "header holds a freelist link alignment");

struct slab {
    uint32_t magic;
    uint32_t class_id;
    uint32_t stride;
    uint32_t nblocks;
    uint32_t has_been_used;
    atomic_uint free_count;
    void *freelist;
    struct slab *next;
    struct slab *prev;
    void *mapping;
    size_t mapping_bytes;
};

struct bin {
    pthread_mutex_t mu;
    struct slab *partial;
    unsigned char separate_cache_line[64];
};

struct tcache {
    void *slots[MAX_CLASS][TCACHE_CAP];
    uint16_t count[MAX_CLASS];
    size_t map_bytes;
};

static size_t page_size;
static size_t class_sizes[MAX_CLASS];
static int nclasses;
static uint8_t lookup[SMALL_MAX + 1];
static struct bin bins[MAX_CLASS];
static pthread_once_t once_control = PTHREAD_ONCE_INIT;
static pthread_key_t tcache_key;
static _Thread_local struct tcache *tc;

static atomic_uint_fast64_t st_alloc_ops;
static atomic_uint_fast64_t st_free_ops;
static atomic_uint_fast64_t st_tcache_hits;
static atomic_uint_fast64_t st_central_pops;
static atomic_uint_fast64_t st_large_allocs;
static atomic_uint_fast64_t st_mmap_calls;
static atomic_uint_fast64_t st_munmap_calls;
static atomic_uint_fast64_t st_bytes_mapped;
static atomic_uint_fast64_t st_bytes_unmapped;
static atomic_uint_fast64_t st_slabs_reclaimed;

static void central_free_batch(int class_id, void **ptrs, int n);
static void *central_alloc(struct tcache *t, int class_id);
static void tcache_destroy(void *argument);

static size_t align_up(size_t value, size_t alignment)
{
    return (value + alignment - 1u) & ~(alignment - 1u);
}

static void freelist_set(void *user, void *next)
{
    memcpy(user, &next, sizeof(next));
}

static void *freelist_get(void *user)
{
    void *next;
    memcpy(&next, user, sizeof(next));
    return next;
}

static hdr *hdr_from_user(void *user)
{
    return (hdr *)((unsigned char *)user - sizeof(hdr));
}

static void note_mmap(size_t bytes)
{
    atomic_fetch_add_explicit(&st_mmap_calls, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&st_bytes_mapped, bytes, memory_order_relaxed);
}

static void note_release(size_t bytes)
{
    atomic_fetch_add_explicit(&st_munmap_calls, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&st_bytes_unmapped, bytes, memory_order_relaxed);
}

static void note_slab_release(size_t bytes)
{
    note_release(bytes);
    atomic_fetch_add_explicit(&st_slabs_reclaimed, 1, memory_order_relaxed);
}

static void partial_push(struct bin *bin, struct slab *slab)
{
    slab->prev = NULL;
    slab->next = bin->partial;
    if (bin->partial != NULL) {
        bin->partial->prev = slab;
    }
    bin->partial = slab;
}

static void partial_remove(struct bin *bin, struct slab *slab)
{
    if (slab->prev != NULL) {
        slab->prev->next = slab->next;
    } else {
        bin->partial = slab->next;
    }
    if (slab->next != NULL) {
        slab->next->prev = slab->prev;
    }
    slab->next = NULL;
    slab->prev = NULL;
}

static struct slab *slab_create(struct bin *bin, int class_id)
{
    const size_t payload = class_sizes[class_id];
    const size_t stride = sizeof(hdr) + payload;
    size_t want = 64u;
    size_t header;
    size_t bytes;
    size_t usable;
    uint32_t nblocks;
    struct slab *slab;
    unsigned char *base;
    void *head;
    uint32_t index;
    void *mapping;

    if (payload >= 16384u) {
        want = 8u;
    } else if (payload >= 4096u) {
        want = 16u;
    }

    header = align_up(sizeof(struct slab), sizeof(hdr));
    if (stride == 0u || want > (SIZE_MAX - header) / stride) {
        errno = ENOMEM;
        return NULL;
    }
    bytes = header + stride * want;
    bytes = align_up(bytes, page_size);
    if (bytes > (256u * 1024u)) {
        bytes = 256u * 1024u;
        if (bytes < header + stride) {
            bytes = align_up(header + stride, page_size);
        }
    }

    mapping = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) {
        return NULL;
    }
    note_mmap(bytes);

    usable = bytes - header;
    nblocks = (uint32_t)(usable / stride);
    if (nblocks == 0u) {
        (void)munmap(mapping, bytes);
        note_release(bytes);
        errno = ENOMEM;
        return NULL;
    }

    slab = (struct slab *)mapping;
    slab->magic = SLAB_MAGIC;
    slab->class_id = (uint32_t)class_id;
    slab->stride = (uint32_t)stride;
    slab->nblocks = nblocks;
    slab->has_been_used = 0u;
    atomic_store_explicit(&slab->free_count, nblocks, memory_order_relaxed);
    slab->next = NULL;
    slab->prev = NULL;
    slab->mapping = mapping;
    slab->mapping_bytes = bytes;

    base = (unsigned char *)mapping + header;
    head = NULL;
    for (index = 0u; index < nblocks; ++index) {
        hdr *block = (hdr *)(base + (size_t)index * stride);
        void *user = (unsigned char *)block + sizeof(hdr);
        block->f.meta = (uintptr_t)slab;
        block->f.requested = 0u;
        freelist_set(user, head);
        head = user;
    }
    slab->freelist = head;
    partial_push(bin, slab);
    return slab;
}

static void *bin_pop(struct bin *bin, int class_id)
{
    struct slab *slab = bin->partial;
    void *user;
    unsigned int previous;

    if (slab == NULL) {
        slab = slab_create(bin, class_id);
        if (slab == NULL) {
            return NULL;
        }
    }

    user = slab->freelist;
    if (UNLIKELY(user == NULL || slab->magic != SLAB_MAGIC)) {
        abort();
    }
    slab->freelist = freelist_get(user);
    previous = atomic_fetch_sub_explicit(&slab->free_count, 1u, memory_order_relaxed);
    if (UNLIKELY(previous == 0u)) {
        abort();
    }
    slab->has_been_used = 1u;
    if (previous == 1u) {
        partial_remove(bin, slab);
    }
    atomic_fetch_add_explicit(&st_central_pops, 1, memory_order_relaxed);
    return user;
}

static void init_all(void)
{
    static const size_t bases[] = {
        8u, 16u, 32u, 48u, 64u, 80u, 96u, 112u, 128u,
        160u, 192u, 224u, 256u,
        320u, 384u, 448u, 512u,
        640u, 768u, 896u, 1024u,
        1280u, 1536u, 1792u, 2048u,
        2560u, 3072u, 3584u, 4096u,
        6144u, 8192u,
        12288u, 16384u,
        24576u, 32768u
    };
    const long page = sysconf(_SC_PAGESIZE);
    size_t align = _Alignof(max_align_t);
    size_t previous = 0u;
    size_t index;
    int class_id;
    int lookup_class;

    if (page <= 0 || (page & (page - 1)) != 0) {
        abort();
    }
    page_size = (size_t)page;
    if (align < sizeof(void *)) {
        align = sizeof(void *);
    }
    if ((align & (align - 1u)) != 0u || sizeof(hdr) < align ||
        (sizeof(hdr) % align) != 0u) {
        abort();
    }

    nclasses = 0;
    for (index = 0u; index < (sizeof(bases) / sizeof(bases[0])); ++index) {
        size_t rounded = align_up(bases[index], align);
        if (rounded <= previous || rounded > SMALL_MAX) {
            continue;
        }
        if (nclasses >= MAX_CLASS) {
            abort();
        }
        class_sizes[nclasses++] = rounded;
        previous = rounded;
    }
    if (nclasses == 0 || class_sizes[nclasses - 1] < SMALL_MAX) {
        if (nclasses >= MAX_CLASS) {
            abort();
        }
        class_sizes[nclasses++] = align_up(SMALL_MAX, align);
    }

    lookup_class = 0;
    for (index = 1u; index <= SMALL_MAX; ++index) {
        while (class_sizes[lookup_class] < index) {
            ++lookup_class;
            if (lookup_class >= nclasses) {
                abort();
            }
        }
        lookup[index] = (uint8_t)lookup_class;
    }

    for (class_id = 0; class_id < nclasses; ++class_id) {
        if (pthread_mutex_init(&bins[class_id].mu, NULL) != 0) {
            abort();
        }
        bins[class_id].partial = NULL;
    }
    if (pthread_key_create(&tcache_key, tcache_destroy) != 0) {
        abort();
    }
}

static void ensure(void)
{
    (void)pthread_once(&once_control, init_all);
}

static void tcache_destroy(void *argument)
{
    struct tcache *cache = (struct tcache *)argument;
    int class_id;
    size_t bytes;

    if (tc == cache) {
        tc = NULL;
    }
    for (class_id = 0; class_id < nclasses; ++class_id) {
        if (cache->count[class_id] != 0u) {
            central_free_batch(class_id,
                               cache->slots[class_id],
                               (int)cache->count[class_id]);
            cache->count[class_id] = 0u;
        }
    }
    bytes = cache->map_bytes;
    (void)munmap(cache, bytes);
    note_release(bytes);
}

static struct tcache *get_tcache(void)
{
    struct tcache *cache;
    size_t bytes;

    if (LIKELY(tc != NULL)) {
        return tc;
    }
    bytes = align_up(sizeof(struct tcache), page_size);
    cache = (struct tcache *)mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (cache == MAP_FAILED) {
        return NULL;
    }
    note_mmap(bytes);
    memset(cache, 0, sizeof(*cache));
    cache->map_bytes = bytes;
    tc = cache;
    (void)pthread_setspecific(tcache_key, cache);
    return cache;
}

static void central_free_batch(int class_id, void **ptrs, int count)
{
    struct bin *bin = &bins[class_id];
    void *unmaps[TCACHE_CAP + 1];
    size_t unmap_lens[TCACHE_CAP + 1];
    int unmap_count = 0;
    int index;

    if (count <= 0) {
        return;
    }
    pthread_mutex_lock(&bin->mu);
    for (index = 0; index < count; ++index) {
        hdr *block = hdr_from_user(ptrs[index]);
        struct slab *slab = (struct slab *)block->f.meta;
        unsigned int now;

        if (UNLIKELY(slab == NULL || slab->magic != SLAB_MAGIC ||
                     (int)slab->class_id != class_id)) {
            abort();
        }
        freelist_set(ptrs[index], slab->freelist);
        slab->freelist = ptrs[index];
        now = atomic_fetch_add_explicit(&slab->free_count, 1u, memory_order_relaxed) + 1u;
        if (UNLIKELY(now > slab->nblocks)) {
            abort();
        }
        if (now == 1u) {
            partial_push(bin, slab);
        }
        if (now == slab->nblocks && slab->has_been_used) {
            partial_remove(bin, slab);
            unmaps[unmap_count] = slab->mapping;
            unmap_lens[unmap_count] = slab->mapping_bytes;
            ++unmap_count;
        }
    }
    pthread_mutex_unlock(&bin->mu);

    for (index = 0; index < unmap_count; ++index) {
        (void)munmap(unmaps[index], unmap_lens[index]);
        note_slab_release(unmap_lens[index]);
    }
}

static void *central_alloc(struct tcache *cache, int class_id)
{
    void *batch[REFILL_BATCH];
    int got = 0;
    struct bin *bin = &bins[class_id];
    void *result;

    pthread_mutex_lock(&bin->mu);
    while (got < REFILL_BATCH) {
        void *block = bin_pop(bin, class_id);
        if (block == NULL) {
            break;
        }
        batch[got++] = block;
    }
    pthread_mutex_unlock(&bin->mu);

    if (got == 0) {
        errno = ENOMEM;
        return NULL;
    }

    result = batch[--got];
    if (cache != NULL) {
        while (got > 0 && cache->count[class_id] < TCACHE_CAP) {
            cache->slots[class_id][cache->count[class_id]++] = batch[--got];
        }
    }
    if (got > 0) {
        central_free_batch(class_id, batch, got);
    }
    return result;
}

static void *large_alloc(size_t requested, size_t payload)
{
    size_t total;
    size_t mapping_bytes;
    void *mapping;
    hdr *block;

    if (payload > SIZE_MAX - sizeof(hdr)) {
        errno = ENOMEM;
        return NULL;
    }
    total = sizeof(hdr) + payload;
    if (total > SIZE_MAX - (page_size - 1u)) {
        errno = ENOMEM;
        return NULL;
    }
    mapping_bytes = align_up(total, page_size);
    if (mapping_bytes > (UINTPTR_MAX >> 2)) {
        errno = ENOMEM;
        return NULL;
    }

    mapping = mmap(NULL, mapping_bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) {
        return NULL;
    }
    note_mmap(mapping_bytes);
    atomic_fetch_add_explicit(&st_large_allocs, 1, memory_order_relaxed);

    block = (hdr *)mapping;
    block->f.meta = (mapping_bytes << 2) | (uintptr_t)1u;
    block->f.requested = requested;
    return (unsigned char *)mapping + sizeof(hdr);
}

static void large_free(hdr *block)
{
    size_t mapping_bytes = block->f.meta >> 2;
    (void)munmap(block, mapping_bytes);
    note_release(mapping_bytes);
}

static int large_shrink(hdr *block, size_t size)
{
    size_t mapping_bytes = block->f.meta >> 2;
    size_t usable = mapping_bytes - sizeof(hdr);
    size_t new_mapping;

    if (size > usable) {
        return 0;
    }
    new_mapping = align_up(sizeof(hdr) + size, page_size);
    if (new_mapping < mapping_bytes) {
        if (munmap((unsigned char *)block + new_mapping,
                   mapping_bytes - new_mapping) == 0) {
            note_release(mapping_bytes - new_mapping);
            block->f.meta = (new_mapping << 2) | (uintptr_t)1u;
        }
    }
    block->f.requested = size;
    return 1;
}

void *my_malloc(size_t size)
{
    size_t payload = (size == 0u) ? 1u : size;
    struct tcache *cache;
    int class_id;
    void *user;
    hdr *block;

    ensure();

    if (payload > class_sizes[nclasses - 1]) {
        return large_alloc(size, payload);
    }

    class_id = (int)lookup[payload];
    cache = get_tcache();
    if (cache != NULL && cache->count[class_id] > 0u) {
        user = cache->slots[class_id][--cache->count[class_id]];
    } else {
        user = central_alloc(cache, class_id);
        if (user == NULL) {
            return NULL;
        }
    }

    block = hdr_from_user(user);
    block->f.requested = size;
    return user;
}

void my_free(void *ptr)
{
    hdr *block;
    uintptr_t meta;
    struct slab *slab;
    int class_id;
    struct tcache *cache;

    if (ptr == NULL) {
        return;
    }

    ensure();
    block = hdr_from_user(ptr);
    meta = block->f.meta;
    if (meta & (uintptr_t)1u) {
        large_free(block);
        return;
    }

    slab = (struct slab *)meta;
    if (UNLIKELY(slab == NULL || slab->magic != SLAB_MAGIC)) {
        abort();
    }
    class_id = (int)slab->class_id;
    cache = tc;
    if (cache == NULL) {
        cache = get_tcache();
    }

    if (cache != NULL && cache->count[class_id] == TCACHE_CAP) {
        void *batch[TCACHE_CAP + 1];
        memcpy(batch, cache->slots[class_id], sizeof(void *) * TCACHE_CAP);
        batch[TCACHE_CAP] = ptr;
        cache->count[class_id] = 0u;
        central_free_batch(class_id, batch, TCACHE_CAP + 1);
        return;
    }

    if (cache == NULL) {
        central_free_batch(class_id, &ptr, 1);
        return;
    }

    cache->slots[class_id][cache->count[class_id]++] = ptr;
}

void *my_realloc(void *ptr, size_t size)
{
    hdr *block;
    uintptr_t meta;
    size_t old_requested;
    size_t copy_bytes;
    void *fresh;

    if (ptr == NULL) {
        return my_malloc(size);
    }
    if (size == 0u) {
        my_free(ptr);
        return NULL;
    }

    ensure();
    block = hdr_from_user(ptr);
    meta = block->f.meta;
    old_requested = block->f.requested;

    if (meta & (uintptr_t)1u) {
        if (large_shrink(block, size)) {
            return ptr;
        }
    } else {
        struct slab *slab = (struct slab *)meta;
        if (UNLIKELY(slab == NULL || slab->magic != SLAB_MAGIC)) {
            abort();
        }
        if (size <= class_sizes[slab->class_id]) {
            block->f.requested = size;
            return ptr;
        }
    }

    fresh = my_malloc(size);
    if (fresh == NULL) {
        return NULL;
    }
    copy_bytes = (old_requested < size) ? old_requested : size;
    if (copy_bytes != 0u) {
        memcpy(fresh, ptr, copy_bytes);
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

void libmemctl_get_stats(struct libmemctl_stats *out)
{
    if (out == NULL) {
        return;
    }
    out->alloc_ops = atomic_load_explicit(&st_alloc_ops, memory_order_relaxed);
    out->free_ops = atomic_load_explicit(&st_free_ops, memory_order_relaxed);
    out->tcache_hits = atomic_load_explicit(&st_tcache_hits, memory_order_relaxed);
    out->central_pops = atomic_load_explicit(&st_central_pops, memory_order_relaxed);
    out->large_allocs = atomic_load_explicit(&st_large_allocs, memory_order_relaxed);
    out->mmap_calls = atomic_load_explicit(&st_mmap_calls, memory_order_relaxed);
    out->munmap_calls = atomic_load_explicit(&st_munmap_calls, memory_order_relaxed);
    out->bytes_mapped = atomic_load_explicit(&st_bytes_mapped, memory_order_relaxed);
    out->bytes_unmapped = atomic_load_explicit(&st_bytes_unmapped, memory_order_relaxed);
    out->slabs_reclaimed = atomic_load_explicit(&st_slabs_reclaimed, memory_order_relaxed);
}

void libmemctl_trim(void)
{
    struct tcache *cache;
    int class_id;

    ensure();
    cache = tc;
    if (cache == NULL) {
        return;
    }
    for (class_id = 0; class_id < nclasses; ++class_id) {
        if (cache->count[class_id] != 0u) {
            central_free_batch(class_id,
                               cache->slots[class_id],
                               (int)cache->count[class_id]);
            cache->count[class_id] = 0u;
        }
    }
}
