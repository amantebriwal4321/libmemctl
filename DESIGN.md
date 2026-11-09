# Design

libmemctl serves the four required calls from anonymous private mappings. It does not call `malloc`, `free`, `calloc`, `realloc`, `sbrk`, or the other heap entry points banned by the statement. Returned pointers are aligned to `_Alignof(max_align_t)` (16 on the measurement host). `max_align_t` itself is 32 bytes there; the requirement is the alignment, not that size.

## Layout

Small and medium requests (1 byte through 32,768, with `malloc(0)` treated as a 1-byte capacity) round up to a size class. Classes are multiples of the alignment: 16, 32, 48, 64, then wider steps up to 32,768. A 1,024-entry lookup table maps a size to a class.

Each class owns slabs. A slab is one `mmap`. Its header sits at the start of the mapping; the rest is a packed run of blocks. A block is a 16-byte header plus the class payload:

- `meta` is the slab pointer. Slab addresses are page-aligned, so bit 0 is clear.
- `requested` is the size argument, which may be 0. `realloc` copies `min(old requested, new size)` bytes.

The freelist link lives in the payload of a free block, via `memcpy`, so it does not alias the header. The smallest class is at least `sizeof(void *)`.

Requests above 32,768 bytes are large. The header is the first bytes of a dedicated mapping. `meta` is `(mapping_bytes << 2) | 1`. `free` unmaps that whole region. `realloc` that still fits keeps the mapping and, when a whole trailing page is unused, `munmap`s the tail. `mremap` is not used; the allowed VM calls in the statement are `mmap` and `munmap`.

## Ownership and locks

There is one mutex per size class, cache-line padded. The mutex covers that class's partial-slab list and each slab's freelist and `free_count`. Locks are not nested.

`free_count` is the number of blocks on that slab's freelist only. A slab is on the partial list exactly when `free_count > 0`. A slab is unmapped only when a free returns the last outstanding block to the freelist (`free_count` reaches `nblocks`) and the slab has been used. Blocks sitting in a thread cache are not on the freelist, so they keep the slab alive.

Each thread `mmap`s its own cache (no libc heap) and stores the pointer in `_Thread_local` storage. The cache holds up to 32 free user pointers per class. Allocation pops that array. A miss locks the class once and refills up to 16 blocks. A full cache flushes all 32 plus the new free under one lock. `pthread_key` runs the same flush when the thread exits, which is what returns a worker's slabs after `pthread_join`. The main thread does not run that destructor on `exit`; `libmemctl_trim()` flushes the calling thread. That function is optional and is not in `libmemctl.h`.

Cross-thread free is a normal free on the freeing thread. The block moves to that thread's cache or back to the slab. Callers still have to hand off a live object before another thread frees or reallocates it; the allocator does not make concurrent use of one object safe.

`calloc` checks `nmemb * size` against `SIZE_MAX` before multiplying, sets `errno` to `ENOMEM` on overflow, and `memset`s the requested bytes. Fresh anonymous pages are already zero; reused blocks are not, so the clear is unconditional. `malloc(0)` and `calloc(0, n)` / `calloc(n, 0)` return a real freeable block and store a requested size of 0.

`realloc(NULL, n)` is `malloc`. `realloc(p, 0)` frees and returns `NULL`. If the new size fits the current class, or fits the current large mapping, the same pointer is returned and `requested` is updated. Otherwise a new block is allocated first. On failure the old block is left alone.

## What changed the speed, and what it costs

An early policy unmapped a slab as soon as every block was either on the freelist or in the freeing thread's cache. That is a correct description of "the application holds nothing," and the 20,000 × 128-byte RSS test looked tight. It also deleted the cache on the next free of a size that had no other live block. A 64-byte alloc/free loop then mapped a fresh slab every iteration.

Measured on this host, median of five runs, that policy did **198,024** alloc/free pairs per second (200,000 ops/run). The one-mapping baseline did **260,053** on the same shape of loop. After the cache was left intact until it overflowed, the thread exited, or `libmemctl_trim` ran, the same pair test at 1,000,000 ops/run did **117,594,708** per second (best 126,555,285, worst 115,763,585).

The tradeoff is retained memory. Each thread can hold 32 free blocks per class, and those blocks pin their slabs. After freeing 20,000 blocks of 128 bytes in the main thread, RSS was **1,996 kB** against an **1,812 kB** idle baseline (+184 kB). `libmemctl_trim` brought it to **1,984 kB**. Peak RSS during that hold was **4,852 kB** for **2.441 MiB** of requested payload, a ratio of **1.216**. The baseline's peak ratio on the same driver was **32.048**, and it did return essentially all of that on free. Empty slabs are still unmapped once their blocks have been flushed out of the cache; a long free of one size does not keep every slab.

## What this is not

- Not a `malloc` interposer.
- Not async-signal-safe. The fast path is not a signal handler.
- Not a `brk` allocator. The harness uses the libc heap for its own bookkeeping, so the statement's `brk` warning applies.
- No cross-size coalescing. Adjacent free blocks of different classes are not merged. External fragmentation is limited by segregated classes, and a fully free slab is returned with `munmap`.
- Double-free and frees of interior pointers are undefined in the contract. A bad small-object header fails a slab magic check and aborts. That is a debugging aid, not a graded detector.
- Fast-path alloc and free counts are not incremented. A shared atomic there bounced a cache line across cores. `libmemctl_get_stats` still reports mapping calls, bytes, and central pops.
