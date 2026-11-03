#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check that a producer change is a pure optimization: same behaviour as a base.

Builds diff/diff.c twice on the host, against the library sources of a git
revision (--base, default HEAD) and of the working tree, in several library
configurations, and compares their per-operation digests over random seeds.
Any difference in ring bytes, producer state, statistics, reservation positions
or the number of timer reads fails, with the first differing operation.

  run_diff.py                     working tree against HEAD
  run_diff.py --base HEAD~1 --seeds 100
"""
import argparse
import io
import pathlib
import subprocess
import sys
import tarfile
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]

CONFIGS = ["", "-DEXTRACE_CFG_LOST_BYTES=1", "-DEXTRACE_CFG_STATS=0",
           "-DEXTRACE_CFG_WRITE_IN_CS_MAX=16", "-DEXTRACE_CFG_PUBLISH_IN_ISR=0",
           "-DEXTRACE_CFG_RESV_ALIGN_MAX=1", "-DEXTRACE_CFG_EMIT_SPECIALIZE=0",
           "-DEXTRACE_CFG_FTRACE_CALLER=1"]


def build(src, out, cfg, cc):
    """The driver and the POSIX port of the working tree; producer, consumer, decoder of src."""
    cmd = [cc, "-std=c2x", "-O1", "-w", f"-I{ROOT / 'tests/common'}",
           f"-I{src / 'include/extrace'}", f"-I{ROOT / 'port/posix'}", f"-I{src / 'port/common'}",
           "-DEXTRACE_GRP_FORCED_ON=1", "-DEXTRACE_CFG_FTRACE=1", "-DEXTRACE_CFG_TEST_HOOKS=1",
           "-DEXTRACE_CFG_MAX_PAYLOAD=128", *cfg.split(),
           str(HERE / "diff/diff.c"), str(src / "src/extrace_prod.c"),
           str(src / "src/extrace_cons.c"), str(src / "src/extrace_rec.c"),
           str(src / "src/extrace_test_hooks.c"), str(ROOT / "port/posix/extrace_port_posix.c"),
           "-o", str(out), "-lpthread"]
    subprocess.run(cmd, check=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--base", default="HEAD")
    ap.add_argument("--seeds", type=int, default=40)
    ap.add_argument("--ops", type=int, default=4000)
    ap.add_argument("--cc", default="cc")
    a = ap.parse_args()
    bad = 0
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        base = tmp / "base"
        arch = subprocess.run(["git", "-C", str(ROOT), "archive", a.base, "src", "include", "port"],
                              check=True, capture_output=True).stdout
        tarfile.open(fileobj=io.BytesIO(arch)).extractall(base, filter="data")
        for cfg in CONFIGS:
            old, new = tmp / "old", tmp / "new"
            build(base, old, cfg, a.cc)
            build(ROOT, new, cfg, a.cc)
            differ = 0
            for seed in range(1, a.seeds + 1):
                o, n = (subprocess.run([str(x), str(seed), str(a.ops)], check=True,
                                       capture_output=True, text=True).stdout.splitlines()
                        for x in (old, new))
                if o != n:
                    first = next(i for i, (x, y) in enumerate(zip(o + [""], n + [""])) if x != y)
                    print(f"  seed {seed}: differs from operation {first}")
                    differ += 1
            print(f"{cfg or 'defaults'}: {a.seeds} seeds, {differ} differ")
            bad += differ
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
