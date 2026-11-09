#!/bin/sh
# Live or recorded demo for SYSPROG10 libmemctl.
# Runs the official harness, a short fresh timing pass, then the recorded medians.
set -eu
cd "$(dirname "$0")"

echo "== build and official harness =="
make check

echo
echo "== live spot check (one run; the write-up uses the five-run median) =="
make build/bench
./build/bench quick

echo
echo "== recorded five-run medians =="
cat docs/measured/bench_libmemctl.txt
