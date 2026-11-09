# Benchmarks

Numbers below are from one full run of `make bench` on this host after the thread-cache fix described in `DESIGN.md`. They are medians of five runs unless a row says otherwise. A later run on the same binary will not match bit for bit. The raw logs are `docs/measured/bench_libmemctl.txt`, `bench_baseline.txt`, and `bench_glibc.txt`.

`alloc_ops`, `free_ops`, and `tcache_hits` in the libmemctl log are 0 on purpose. Those counters are not incremented. `central_pops`, `mmap_calls`, and byte counts are live.

## Machine

| Item | Value |
|---|---|
| OS | Linux 6.12.113+ SMP PREEMPT_DYNAMIC, x86_64 |
| CPU | Intel Xeon, family 6 model 207, 4 cores, 1 thread per core, KVM |
| Caches | L1d 192 KiB, L1i 128 KiB, L2 8 MiB, L3 320 MiB |
| RAM | 16 GiB guest |
| Page size | 4096 |
| THP | `always [madvise] never` (no `madvise` in the allocator) |
| Compiler | gcc 13.3.0, `-O2 -g -std=c11 -D_GNU_SOURCE` |
| Alignment | `_Alignof(max_align_t) = 16` |

The guest is virtualized. Treat the rates as comparable across the three drivers on this boot, not as bare-metal line rate. Eight threads share four cores.

## Method

One process per driver. Clock is `CLOCK_MONOTONIC`.

1. **Space.** Idle `/proc/self/status`, then 20,000 blocks of 128 bytes, each byte written, then free all of them. RSS and VmSize are deltas from the idle reading. `live_over_requested` is `(RSS_live - RSS_base) / requested`. libmemctl then calls `libmemctl_trim` and reads RSS again. The baseline and glibc drivers have no trim hook (`rss_after_trim_kB -1`).
2. **Pairs.** One warmup of 250,000 alloc/free pairs of 64 bytes (the first byte is stored). Then five timed runs of 1,000,000 pairs. Reported rate is `ops / median_seconds` where one op is one alloc plus the matching free.
3. **Mixed.** Per thread, 1,024 private slots and 80,000 operations. The generator is xorshift. About 55% allocate (freeing the slot's previous block first), 25% `realloc`, 20% free, checking the tag byte. Threads run together; the run's elapsed time is the slowest thread, and the rate counts every thread's operations. Batch latency is the time for 1,024 operations, divided by 1,024; the table uses the median across runs of each run's worst-thread p99.

The official `tests/test_runner.c` print is a different workload: it fills and checks every payload byte. Five passes after the final allocator, all 6 tests passed:

| Run | Official threaded line |
|---|---|
| 1 | 160000 ops in 0.014 s (11,720,665 ops/s) |
| 2 | 160000 ops in 0.013 s (11,950,456 ops/s) |
| 3 | 160000 ops in 0.012 s (12,957,677 ops/s) |
| 4 | 160000 ops in 0.014 s (11,283,107 ops/s) |
| 5 | 160000 ops in 0.012 s (13,336,688 ops/s) |

Median of those five rates: **11,950,456 ops/s**. That figure includes the harness's own fills. It is the public smoke metric, not the pair rate below.

## Results

### 64-byte alloc/free pairs

| Driver | Median ops/s | Best | Worst | Median seconds |
|---|---:|---:|---:|---:|
| libmemctl | 117,594,708 | 126,555,285 | 115,763,585 | 0.008504 |
| one-mmap baseline | 263,790 | 266,903 | 263,323 | 3.790897 |
| glibc | 138,680,906 | 143,110,203 | 130,155,475 | 0.007211 |

libmemctl / baseline = 117,594,708 / 263,790 = **446×**. libmemctl / glibc = **0.85×** on this pure pair loop.

### Mixed workload

| Threads | libmemctl ops/s | p99 ns/op | baseline ops/s | glibc ops/s |
|---:|---:|---:|---:|---:|
| 1 | 34,276,039 | 78.2 | 222,064 | 15,240,098 |
| 2 | 43,532,179 | 179.2 | 142,091 | 27,629,962 |
| 4 | 53,922,091 | 277.0 | 118,383 | 40,982,767 |
| 8 | 48,298,617 | 1929.1 | 151,444 | 49,010,204 |

At 1 thread, libmemctl / baseline = 34,276,039 / 222,064 = **154×**, and libmemctl / glibc = **2.25×**. At 4 threads (one per core), libmemctl / glibc = 53,922,091 / 40,982,767 = **1.32×**. At 8 threads the two are within about 2% and libmemctl's batch p99 jumps, which matches running eight threads on four cores.

### Space, 20,000 × 128 bytes (2.441 MiB requested)

| Driver | RSS base kB | RSS live kB | RSS after free kB | RSS / requested | VmSize live delta kB | VmSize after free delta kB |
|---|---:|---:|---:|---:|---:|---:|
| libmemctl | 1812 | 4852 | 1996 (1984 after trim) | 1.216 | 2884 | 28 |
| one-mmap baseline | 1820 | 81940 | 1940 | 32.048 | 80000 | 0 |
| glibc | 1816 | 4748 | 2064 | 1.173 | 2772 | 4 |

Peak RSS ratio is close to glibc (1.216 vs 1.173) and far under the baseline (32.048). After every block was freed, libmemctl still held about 184 kB over its idle RSS until trim, then about 172 kB. The baseline gave the pages back. That is the cache tradeoff in `DESIGN.md`.

During this whole libmemctl process the log recorded 3,051 `mmap` calls and 3,049 `munmap` calls, 245,780,480 bytes mapped and 245,755,904 bytes unmapped, 2,974 slabs reclaimed, 370,064 central pops, and 0 large allocations. The pair and mixed loops were served from size classes, not from a mapping per object.

## Earlier pair measurement

Before caches were kept across a quiescent free, the pair loop mapped and unmapped a slab every iteration. Median of five runs at 200,000 pairs: **198,024 ops/s**, which was slower than the baseline's **260,053 ops/s** on that same 200,000-op setting. Those two rates used a shorter loop than the table above. They are why the final policy flushes a cache on overflow, thread exit, and `libmemctl_trim` instead of unmapping a slab that the cache still owns.
