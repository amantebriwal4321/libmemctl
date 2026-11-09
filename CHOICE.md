# Choice: SYSPROG10 — libmemctl

Question ID: **SYSPROG10** (Systems Programming, libmemctl).

Team Tebri, solo. Aman Tebriwal. All problem statements in the LLH-26 repository and the OS-track kickoff kit were read before this choice: OS01 PageWeaver, OS10 ContextChaos, OS11 AnarchyOS, OS100 Cat-V, CN01 Bytecode Bouncer, CN10 Packet Rush, COMP01 Compile Yourself, COMP10 Oxygen Debt, SYSPROG01 Phoenix Protocol, SYSPROG10 libmemctl, LLS01 LeakScope, LLS10 VaultFS.

## Why this one

The public contract is closed. `PROBLEM.md` specifies the four functions, alignment, overflow, `calloc` / `realloc` behavior, thread safety, and the ban on the libc heap. The repository ships the harness (`make check`: symbol check plus `tests/test_runner.c`). That harness runs on this machine with no extra image, kernel feature, or hidden golden file.

A solo implementation can be correct against that contract and still show a large, measured gap against the one-mapping-per-allocation baseline the statement itself calls out. Throughput, RSS, and virtual size are all measurable here. The scoring note treats correctness as a gate and then scores throughput, space, and the design write-up.

## What was set aside

- **OS01–OS100.** Real harnesses, but they run only inside the OS-track image (`ostrack:core` / `ostrack:full`). Judging is against a reference machine and, for several items, rank among teams. AnarchyOS and Cat-V also need a bootable image before any test counts.
- **CN01.** The rules file, XDP verdicts, and counters are specified, and the organizers say an automatic harness grades them. The surface is a full gateway firewall (IPv4 and IPv6, jail, port-scan, bounce, honeypot, hot reload, exact JSON). That is a lot of independent behaviors for one person, and it needs a kernel test network this environment does not provide.
- **CN10.** The scoring formulas are explicit (25.0 Mpps, 2.5 µs P99, 32 MiB RSS, 5 µs updates) and a mismatch zeroes the scenario. Hitting those targets for a 13-field classifier is a research-sized risk next to a four-function allocator with a public smoke suite.
- **COMP01.** The machine and `vm` tool are clear, and Tier 1 is finite. Tier 3 (a v1 compiler written in v1, then a fixed point) plus the 10^8 / 10^10 step limits is a poor fit for "high correctness by 13 Nov" as the only submission.
- **COMP10.** `kvalidate` / `ksim` are a real harness, but a wrong optimization fails the case, the hidden programs include odd control flow and ~100,000-instruction inputs, and the tool must stay correct on machines it has not seen. A conservative optimizer can be correct and still not move the cycle count much.
- **SYSPROG01.** Parts 1–4 are well described. Marking is by the demos and a stranger program for Part 4. There is no public pass/fail harness in the kit.
- **LLS01 and LLS10.** The score weights are published. There is no sample corpus, no checker, and no single behavior that is "done" the way `make check` is done.

## Scope taken

Implement `my_malloc`, `my_free`, `my_realloc`, and `my_calloc` in C11 on Linux x86-64, backed by anonymous `mmap` / `munmap`, passing the supplied `make check` and an extra suite, with measured single-thread and multi-thread numbers. No API was added to the required header. `libmemctl_stats.h` is optional instrumentation only.
