# libmemctl — SYSPROG10

User-space allocator for the Low-Level Hackathon (Synergy '26, IIIT Bangalore). Question **SYSPROG10**. Team Tebri, solo: Aman Tebriwal.

## Track

Track: Systems Programming. Question ID: SYSPROG10.

Build and test from this directory with `make clean`, `make`, and `make check`. The commands and what `make check` covers are in [Build and test](#build-and-test).

`my_malloc`, `my_free`, `my_realloc`, and `my_calloc` follow the contract in the official `PROBLEM.md`. The implementation is size-class slabs plus a per-thread cache. Objects larger than 32,768 bytes are their own anonymous mappings. See `DESIGN.md` and `docs/BENCHMARKS.md`.

This tree has not been submitted. The Unstop form wants a GitHub link; publishing that mirror still needs Aman's go-ahead. Deadline: 13 Nov 2026, 11:59 PM IST. Live demos: 14 Nov 2026, 9:00 AM IST.

## Build and test

Ubuntu or another Linux x86-64 with `gcc`, `make`, and pthreads. From this directory:

```sh
make clean
make
make check
```

`make check` is the official symbol check plus `tests/test_runner.c`, then `tests/extra_tests.c`. Flags match the starter makefile: C11, `-O2 -g`, `-Wall -Wextra -Wpedantic -Werror`, and the alloc-builtin disables.

`make bench` rebuilds three drivers and writes:

- `build/bench_libmemctl.txt` — this allocator
- `build/bench_baseline.txt` — one anonymous mapping per object
- `build/bench_glibc.txt` — the same driver on glibc `malloc`

The full bench takes about a minute because the baseline issues a mapping per object. `./build/bench quick` is a single-run spot check.

Recorded medians from the full run are in `docs/measured/` and written up in `docs/BENCHMARKS.md`.

## Run the prebuilt binaries

These are Linux x86-64 PIE executables built on the measurement host (glibc, dynamic). They do not replace a clean rebuild; the problem statement says judges rebuild from source.

```sh
./prebuilt/test_runner
./prebuilt/extra_tests
./prebuilt/bench quick
```

## Demo

```sh
./demo.sh
```

That runs `make check` and a live quick benchmark, then prints the recorded five-run medians.

## Layout

| Path | Role |
|---|---|
| `include/libmemctl.h` | Required API. Unchanged from the starter kit. |
| `include/libmemctl_stats.h` | Optional counters. Not part of the graded API. |
| `src/allocator.c` | The allocator. |
| `tests/test_runner.c` | Official public suite. |
| `tests/extra_tests.c` | Zero-size, alignment span, dirty `calloc`, cross-thread free, thread-exit reclamation. |
| `bench/` | Timing and RSS driver, baseline, glibc wrapper. |
| `docs/BENCHMARKS.md` | Machine, method, and measured numbers. |
| `DESIGN.md` | Layout, locks, and the reclamation tradeoff. |
| `CHOICE.md` | Why SYSPROG10. |
| `prebuilt/` | Testable x86-64 binaries and `libmemctl.a`. |

Link a program with `build/libmemctl.a -pthread`. Do not interpose `malloc`. The allocator object must not reference the libc heap; `make check-symbols` enforces the starter's symbol list.
