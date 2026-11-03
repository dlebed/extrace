#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Instructions retired by the producer fast paths, per event (docs/performance.md).

Builds bench.c with the producer for a bare-metal target, runs it and prints one
row per event: record bytes, instructions, loads/stores, and where the
instructions go, by function. The counts come from the image itself (bench.c)
and are checked against an instruction trace, which also gives the breakdown.

  rv32    qemu-system-riscv32 (virt, M-mode, -icount), rv32im_zce_zbb, clang or gcc
  armv7m  unicorn (Cortex-M4, hard float), clang

  run_perf.py                         both targets, clang -O2
  run_perf.py rv32 --cc gcc --opt Os
  run_perf.py rv32 --trace MARK       also print the executed instructions of a row
  run_perf.py --src DIR               measure the sources of another tree
  run_perf.py -D EXTRACE_CFG_STATS=0  library configuration
"""
import argparse
import bisect
import collections
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]

# regions per row: function entry tracing rows measure the traced call, then the plain one
ROWS = ["MARK", "NONE", "INTS 1 arg", "FTRACE not patched", "FTRACE MARK", "FTRACE NONE",
        "FTRACE ARGS 1 arg", "FTRACE ARGS 4 args", "FTRACE MARK, caller", "FTRACE NONE, caller",
        "FTRACE ARGS 1 arg, caller", "FTRACE patched, switch off",
        "MARK, event off", "publish", "publish, nothing new"]
PAIRED = {r for r in ROWS if r.startswith("FTRACE")}
WRAPPERS = {"main", "call_traced", "call_plain", "traced", "plain"}
SITES = ("b_", "main")

TARGETS = {
    "rv32": dict(
        clang=["clang", "--target=riscv32-unknown-elf", "-march=rv32im_zce_zbb", "-mabi=ilp32"],
        # GCC 13 binutils lack Zcmp: compressed instructions, no push/pop
        gcc=["riscv64-unknown-elf-gcc", "-march=rv32imc_zicsr_zbb", "-mabi=ilp32"],
        mattr="+zce,+zbb,+zicsr,+m",
        cpu="rv32,zcb=true,zcmp=true,zcmt=true,zbb=true,f=false,d=false,zfa=false",
        cpu_gcc="rv32,zbb=true"),
    "armv7m": dict(
        clang=["clang", "--target=thumbv7em-none-eabihf", "-mcpu=cortex-m4",
               "-mfpu=fpv4-sp-d16", "-mfloat-abi=hard"],
        mattr=None),
}


def sh(cmd, **kw):
    return subprocess.run(cmd, check=True, **kw)


def build(target, cc, opt, src, out, defines):
    t = TARGETS[target]
    base = "0x80000000u" if target == "rv32" else "0x20000000u"

    def flags(c):
        return [f"-{opt}", "-ffreestanding", "-ffunction-sections", "-fdata-sections",
                "-std=c2x" if c == "gcc" else "-std=c23", "-DEXTRACE_CFG_FTRACE=1",
                f"-DEXTRACE_CFG_FTRACE_BASE={base}", *(f"-D{d}" for d in defines),
                f"-I{HERE / target}", f"-I{HERE}", f"-I{src / 'include/extrace'}",
                f"-I{src / 'port/common'}", f"-I{src / 'port/arch' / target}"]

    # the library is never instrumented (clang has no such flag for Thumb)
    lib = ["-fpatchable-function-entry=0"] if target == "rv32" else []
    objs = []

    def cc_(c, extra, s, o):
        cmd = t[c]
        if c == "clang" and cc == "gcc":  # the whole image on the ISA of the gcc build
            cmd = [cmd[0], cmd[1], t["gcc"][1], *cmd[3:]]
        sh(cmd + flags(c) + extra + ["-c", str(s), "-o", str(out / o)])
        objs.append(str(out / o))

    cc_(cc, lib, src / "src/extrace_prod.c", "prod.o")
    cc_("clang", lib, src / "src/extrace_ftrace.c", "ftrace.o")
    cc_("clang", [], src / "port/arch" / target / f"extrace_ftrace_{target}.S", "tramp.o")
    cc_("clang", [], HERE / "bench.c", "bench.o")
    cc_("clang", ["-fno-builtin"], HERE / "libc.c", "libc.o")
    if target == "rv32":
        cc_("clang", [], HERE / "rv32/crt0.S", "crt0.o")
    elf = out / "bench.elf"
    sh(["ld.lld", "-T", str(HERE / target / "link.ld"), "--gc-sections",
        *(["-e", "main"] if target == "armv7m" else []), "-o", str(elf), *objs])
    size = sum(int(l.split()[1]) for l in
               sh(["llvm-size", "-A", str(out / "prod.o")], capture_output=True,
                  text=True).stdout.splitlines() if l.startswith(".text"))
    return elf, size


def disasm(elf, target):
    cmd = ["llvm-objdump", "-d", "--no-show-raw-insn", str(elf)]
    if TARGETS[target]["mattr"]:
        cmd.insert(2, f"--mattr={TARGETS[target]['mattr']}")
    insn, syms = {}, []
    for line in sh(cmd, capture_output=True, text=True).stdout.splitlines():
        m = re.match(r"^([0-9a-f]{8}) <(.+)>:", line)
        if m and not m[2].startswith(("$", ".")):
            syms.append((int(m[1], 16), m[2]))
            continue
        m = re.match(r"^\s*([0-9a-f]+):\s+(.*)", line)
        if m:
            insn[int(m[1], 16)] = m[2].strip()
    syms.sort()
    addrs = [a for a, _ in syms]
    return insn, lambda pc: syms[bisect.bisect_right(addrs, pc) - 1][1]


def nregs(text):
    """Registers moved by a push/pop-like instruction."""
    m = re.search(r"\{(.*)\}", text)
    if not m:
        return 1
    n = 0
    for part in m[1].split(","):
        part = part.strip()
        r = re.match(r"([a-z]+)(\d+)-[a-z]*?(\d+)$", part)
        n += int(r[3]) - int(r[2]) + 1 if r else 1
    return n


def memops(text):
    """(loads, stores) of one instruction; push/pop count every register."""
    op = text.split()[0] if text else ""
    if op.startswith("cm.pop"):
        return nregs(text), 0
    if op == "cm.push":
        return 0, nregs(text)
    if op in ("lw", "lh", "lhu", "lb", "lbu", "c.lw"):
        return 1, 0
    if op in ("sw", "sh", "sb", "c.sw"):
        return 0, 1
    if op in ("push", "vpush") or op.startswith(("stm", "vstm")):
        return 0, nregs(text) * (2 if "d" in text.split("{")[1][:1] else 1)
    if op in ("pop", "vpop") or op.startswith(("ldm", "vldm")):
        return nregs(text) * (2 if "d" in text.split("{")[1][:1] else 1), 0
    if op.startswith(("ldr", "vldr")):
        return (2 if op.startswith("ldrd") else 1), 0
    if op.startswith(("str", "vstr")):
        return 0, (2 if op.startswith("strd") else 1)
    return 0, 0


def run_rv32(elf, cpu, out):
    trace = out / "trace.log"
    q = ["qemu-system-riscv32", "-M", "virt", "-cpu", cpu, "-bios", "none", "-nographic",
         "-icount", "shift=0", "-monitor", "none", "-kernel", str(elf)]
    text = sh(q + ["-serial", "stdio"], capture_output=True, text=True, timeout=60).stdout
    sh(q + ["-serial", "file:/dev/null", "-accel", "tcg,one-insn-per-tb=on", "-d",
            "exec,nochain", "-D", str(trace)], timeout=60)
    pcs = [int(l.split("/")[1], 16) for l in open(trace) if l.startswith("Trace")]
    return text, pcs


def run_armv7m(elf, out):
    import unicorn as uc
    from unicorn import arm_const as A

    binf = out / "bench.bin"
    sh(["llvm-objcopy", "-O", "binary", str(elf), str(binf)])
    syms = {l.split()[2]: int(l.split()[0], 16) for l in
            sh(["llvm-nm", str(elf)], capture_output=True, text=True).stdout.splitlines()
            if len(l.split()) == 3}
    mu = uc.Uc(uc.UC_ARCH_ARM, uc.UC_MODE_THUMB | uc.UC_MODE_MCLASS)
    mu.ctl_set_cpu_model(A.UC_CPU_ARM_CORTEX_M4)
    mu.mem_map(0x20000000, 0x100000)
    mu.mem_write(0x20000000, binf.read_bytes())
    pcs, text, marks = [], [], []

    def code(m, a, s, d):
        pcs.append(a)

    def rd(m, off, size, d):
        n = len(pcs) - 1  # instructions before the reading one
        if off == 0:      # retired-instruction counter
            marks.append(n)
            return n
        return 1000 + n // 100 if off == 4 else 0  # timer

    def wr(m, off, size, v, d):
        if off == 8:      # UART
            text.append(chr(v & 0xFF))

    # MMIO callbacks, not memory hooks: a write from a hook can restart a block
    mu.mmio_map(0x40000000, 0x1000, rd, None, wr, None)
    mu.hook_add(uc.UC_HOOK_CODE, code)
    stop = 0x200FFE00
    mu.reg_write(A.UC_ARM_REG_SP, 0x200FF000)
    mu.reg_write(A.UC_ARM_REG_LR, stop | 1)
    try:
        mu.emu_start(syms["main"] | 1, stop, count=5_000_000)
    except uc.UcError as e:
        raise RuntimeError(f"{e} at {pcs[-1]:#x}") from None
    return "".join(text), pcs, marks


def regions_rv32(pcs, insn):
    out, cur = [], None
    for pc in pcs:
        if insn.get(pc, "").startswith("csrr") and "minstret" in insn[pc]:
            if cur is None:
                cur = []
            else:
                out.append(cur)
                cur = None
        elif cur is not None:
            cur.append(pc)
    return out


def regions_marks(pcs, marks):
    return [pcs[a + 1:b] for a, b in zip(marks[0::2], marks[1::2])]


def profile(region, insn, sym):
    c, ld, st = collections.Counter(), 0, 0
    for pc in region:
        c[sym(pc)] += 1
        l, s = memops(insn.get(pc, ""))
        ld, st = ld + l, st + s
    return c, ld, st


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("targets", nargs="*", default=list(TARGETS))
    ap.add_argument("--cc", default="clang", choices=["clang", "gcc"])
    ap.add_argument("--opt", default="O2")
    ap.add_argument("--src", type=pathlib.Path, default=ROOT)
    ap.add_argument("--trace", metavar="ROW", help="print the executed instructions of ROW")
    ap.add_argument("-D", dest="defines", action="append", default=[], metavar="NAME=VALUE")
    a = ap.parse_args()
    bad = False
    for target in a.targets:
        cc = a.cc if target == "rv32" else "clang"
        with tempfile.TemporaryDirectory() as tmp:
            out = pathlib.Path(tmp)
            elf, size = build(target, cc, a.opt, a.src.resolve(), out, a.defines)
            insn, sym = disasm(elf, target)
            if target == "rv32":
                text, pcs = run_rv32(elf, TARGETS[target]["cpu" if cc == "clang" else "cpu_gcc"], out)
                regions = regions_rv32(pcs, insn)
            else:
                text, pcs, marks = run_armv7m(elf, out)
                regions = regions_marks(pcs, marks)
        rows = {}
        lines = text.split("# pass 2\n", 1)[1].splitlines()
        for l in lines:
            name, n, b = l.rsplit(",", 2)
            rows[name] = (int(n), int(b))
        # regions: calibration, pass 1, pass 2
        order = [r for r in ROWS if r in rows]  # caller rows: EXTRACE_CFG_FTRACE_CALLER=1
        per_pass = sum(2 if r in PAIRED else 1 for r in order)
        regions = regions[1 + per_pass:1 + 2 * per_pass]
        cfg = "".join(f", {d}" for d in a.defines)
        print(f"## {target}, {cc} -{a.opt}{cfg}: producer .text {size} bytes, "
              f"publish threshold {rows['publish_thr'][0]} bytes\n")
        print("| Event | Bytes | Instructions | Loads / stores | Breakdown |")
        print("|---|---|---|---|---|")
        i = 0
        for r in order:
            n, b = rows[r]
            c, ld, st = profile(regions[i], insn, sym)
            if r in PAIRED:
                c0, ld0, st0 = profile(regions[i + 1], insn, sym)
                got = len(regions[i]) - len(regions[i + 1]) - (1 if target == "armv7m" else 0)
                ld, st = ld - ld0, st - st0
                # the traced call runs the plain body too; on ARM after a b.w
                patch = c["traced"] + c["plain"] - c0["plain"] - (1 if target == "armv7m" else 0)
                parts = ([f"patched entry {patch}"] if patch else []) + \
                        [f"{s} {k}" for s, k in c.items() if s not in WRAPPERS]
                show = regions[i]
                i += 2
            else:
                got = len(regions[i])
                site = sum(k for s, k in c.items() if s.startswith(SITES))
                parts = [f"call site {site}"] + [f"{s} {k}" for s, k in c.items()
                                                 if not s.startswith(SITES)]
                show = regions[i]
                i += 1
            if got != n:
                print(f"ERROR: {r}: counter {n}, trace {got}", file=sys.stderr)
                bad = True
            print(f"| {r} | {b} | {n} | {ld} / {st} | {', '.join(parts)} |")
            if a.trace == r:
                for pc in show:
                    print(f"    {pc:08x}  {sym(pc):24s} {insn.get(pc, '?')}")
        print()
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
