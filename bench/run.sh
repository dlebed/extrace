#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# run.sh - reproduce the producer cost figures of docs/performance.md and
# docs/flyer.html into bench/results.txt.
#
# The figures are instructions retired per trace event, counted on emulators by
# tests/perf/run_perf.py (RV32 on qemu-system-riscv32 -icount, Cortex-M4 on
# unicorn), not wall-clock time: they do not depend on the host. Each
# configuration runs RUNS times (default 5); the script fails if any count
# differs between runs, so every figure is the median, minimum and maximum at
# once. It also runs the functional ftrace test on QEMU (tests/ftrace/run_emu.py).
#
# Needs: clang, ld.lld and LLVM binutils, qemu-system-riscv32 (8.1+),
# qemu-system-arm, Python 3 with unicorn.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
out=${1:-$here/results.txt}
runs=${RUNS:-5}
pin=()
command -v taskset >/dev/null && pin=(taskset -c 0)

configs=(
    "--opt O2"
    "--opt O2 -D EXTRACE_CFG_FTRACE_CALLER=1"
    "--opt Os"
)

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

{
    echo "# extrace producer instruction counts (tests/perf/run_perf.py)"
    echo "# date:      $(date -u +%Y-%m-%dT%H:%MZ)"
    echo "# sources:   sha256 $(cd "$root" && find include src port tests/perf -type f ! -name '*.pyc' | LC_ALL=C sort | xargs sha256sum | sha256sum | cut -c1-16) of include/ src/ port/ tests/perf/"
    echo "# host:      $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | sed 's/^ *//'), $(nproc) CPUs, $(uname -sr)"
    echo "# compiler:  $(clang --version | head -1)"
    echo "# emulators: $(qemu-system-riscv32 --version | head -1); unicorn $(python3 -c 'import unicorn; print(unicorn.__version__)')"
    echo "# targets:   rv32 = rv32im_zce_zbb (qemu virt, -icount shift=0, M-mode);"
    echo "#            armv7m = Cortex-M4F, hard-float ABI (unicorn)"
    echo "# flags:     clang --target=... -O2 or -Os -ffreestanding -ffunction-sections -fdata-sections -std=c23"
    echo "#            -DEXTRACE_CFG_FTRACE=1 -DEXTRACE_CFG_FTRACE_BASE=<image base>, library defaults otherwise"
    echo "# method:    instructions retired from the trace point to the return, steady state;"
    echo "#            $runs runs per configuration, counts identical in every run (min = median = max)"
    echo "# note:      run inside a cloud VM; the counts come from the emulators and do not depend"
    echo "#            on the host. They are instructions, not cycles or nanoseconds."
    echo
} > "$tmp/head"

: > "$tmp/body"
for cfg in "${configs[@]}"; do
    for i in $(seq 1 "$runs"); do
        # shellcheck disable=SC2086
        "${pin[@]}" python3 "$root/tests/perf/run_perf.py" $cfg > "$tmp/run$i"
    done
    for i in $(seq 2 "$runs"); do
        if ! cmp -s "$tmp/run1" "$tmp/run$i"; then
            echo "run.sh: counts differ between runs for '$cfg'" >&2
            diff "$tmp/run1" "$tmp/run$i" >&2 || true
            exit 1
        fi
    done
    cat "$tmp/run1" >> "$tmp/body"
done

{
    echo "## functional check: tests/ftrace/run_emu.py --emulator qemu"
    echo
    python3 "$root/tests/ftrace/run_emu.py" --emulator qemu | grep -E 'CALLER=. on' | sed 's/^/    /'
    echo
} >> "$tmp/body"

cat "$tmp/head" "$tmp/body" > "$out"
echo "wrote $out"
