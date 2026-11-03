#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run the ftrace trampolines of port/arch and the patcher on an emulator.

Every image is built twice, with EXTRACE_CFG_FTRACE_CALLER=0 and =1, with clang
and ld.lld. Each prints (QEMU) or leaves in memory (unicorn) a bitmask of failed
checks and the failed checks of every trampoline entry, one per flavor.

  emu   emu_<arch>.c: the real trampoline and patcher, mock emitters that log
        what they get and clobber every scratch register (emu_common.h)
  rec   rec.c: the real trampoline, patcher and producer; the exact record
        bytes of every flavor in the ring (QEMU only)

  --emulator unicorn  emu images, loaded at 0x20000000; the link uses
                      linker/extrace_ftrace.ld, and the site table must stay in
                      the ELF, out of the image, with the address of traced()
  --emulator qemu     emu and rec images on qemu-system-riscv32 -M virt and
                      qemu-system-arm -M mps2-an386 (Cortex-M4F, hard float)
"""
import argparse
import pathlib
import re
import struct
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
HERE = ROOT / "tests/ftrace"
PERF = ROOT / "tests/perf"
BASE, SIZE, STOP = 0x20000000, 0x100000, 0x20000000 + 0x100000 - 0x100

ARCHES = {
    "armv7m": dict(
        target="thumbv7em-none-eabihf",
        flags=["-mcpu=cortex-m4", "-mfpu=fpv4-sp-d16", "-mfloat-abi=hard"],
        asm="extrace_ftrace_armv7m.S", nreg=4, qbase=0x20000000,
        qemu=["qemu-system-arm", "-M", "mps2-an386", "-semihosting-config",
              "enable=on,target=native"]),
    "rv32": dict(
        target="riscv32-unknown-elf",
        flags=["-march=rv32imc_zicsr_zifencei", "-mabi=ilp32"],
        asm="extrace_ftrace_rv32.S", nreg=8, qbase=0x80000000,
        qemu=["qemu-system-riscv32", "-M", "virt", "-bios", "none"]),
}


def tool(lld, name):
    p = pathlib.Path(lld).with_name(name)
    return str(p) if p.exists() else name


def flavor_name(arch, k):
    shapes = 2 + ARCHES[arch]["nreg"]
    s = k % shapes
    name = "MARK" if s == 0 else "NONE" if s == 1 else f"ARGS({s - 1})"
    return name + (" | CALLER" if k >= shapes else "")


def build(arch, image, emulator, caller, a, out):
    cfg = ARCHES[arch]
    qemu = emulator == "qemu"
    inc = []
    if image == "rec":  # the producer: the perf images' port, catalog and configuration
        dirs = [PERF / arch, PERF]
    else:
        dirs = [HERE]
    for d in [*dirs, ROOT / "include/extrace", ROOT / "port/arch" / arch, ROOT / "port/common"]:
        inc += ["-I", str(d)]
    defs = [f"-DEXTRACE_CFG_FTRACE_CALLER={caller}"]
    srcs = [ROOT / "port/arch" / arch / cfg["asm"], ROOT / "src/extrace_ftrace.c"]
    if image == "rec":
        defs += ["-DEXTRACE_CFG_FTRACE=1", f"-DEXTRACE_CFG_FTRACE_BASE={cfg['qbase']:#x}u"]
        srcs += [HERE / "rec.c", ROOT / "src/extrace_prod.c", PERF / "libc.c"]
    else:
        srcs += [HERE / f"emu_{arch}.c"]
    if qemu:
        srcs += [HERE / "qemu_main.c"]
        if arch == "rv32":
            srcs += [PERF / "rv32/crt0.S"]
    objs = []
    for src in srcs:
        obj = out / (src.stem + ".o")
        extra = ["-fno-builtin"] if src.name == "libc.c" else []
        subprocess.run([a.clang, f"--target={cfg['target']}", *cfg["flags"], "-O2",
                        "-ffreestanding", "-std=c2x", "-Wall", "-Werror",
                        *defs, *inc, *extra, "-c", str(src), "-o", str(obj)], check=True)
        objs.append(str(obj))
    if not qemu:
        ld = HERE / "emu.ld"
    elif arch == "rv32":
        ld = PERF / "rv32/link.ld"
    else:
        ld = HERE / "qemu_armv7m.ld"
    elf, binf = out / f"{arch}.elf", out / f"{arch}.bin"
    subprocess.run([a.lld, "-o", str(elf), "-T", str(ld), "-L", str(ROOT / "linker"),
                    *([] if qemu else ["-e", "start"]), *objs], check=True)
    subprocess.run([tool(a.lld, "llvm-objcopy"), "-O", "binary", str(elf), str(binf)],
                   check=True)
    syms = subprocess.run([tool(a.lld, "llvm-nm"), str(elf)], check=True, capture_output=True,
                          text=True).stdout
    addr = {l.split()[2]: int(l.split()[0], 16) for l in syms.splitlines() if len(l.split()) == 3}
    return elf, binf, addr


def check_table(elf, traced):
    """The site table: in the ELF, not allocated (SHF_ALLOC clear), holding traced()."""
    d = elf.read_bytes()
    shoff, = struct.unpack_from("<I", d, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x2E)
    sec = [struct.unpack_from("<IIIIII", d, shoff + i * shentsize) for i in range(shnum)]
    strtab = sec[shstrndx][4]
    for name, _, flags, _, off, size in sec:
        if d[strtab + name:].split(b"\0", 1)[0] == b"__patchable_function_entries":
            first, = struct.unpack_from("<I", d, off)
            print(f"  site table: {size // 4} entries, first {first:#x}, "
                  f"{'allocated' if flags & 2 else 'ELF only'}")
            return not flags & 2 and first == traced & ~1
    print("  site table: missing")
    return False


def run_unicorn(arch, binf, addr):
    import unicorn as uc
    from unicorn import arm_const, riscv_const

    if arch == "armv7m":
        mu = uc.Uc(uc.UC_ARCH_ARM, uc.UC_MODE_THUMB | uc.UC_MODE_MCLASS)
        pc, sp, ret, lr = arm_const.UC_ARM_REG_PC, arm_const.UC_ARM_REG_SP, \
            arm_const.UC_ARM_REG_R0, arm_const.UC_ARM_REG_LR
        entry, stop = addr["start"] | 1, STOP | 1
    else:
        mu = uc.Uc(uc.UC_ARCH_RISCV, uc.UC_MODE_RISCV32)
        pc, sp, ret, lr = riscv_const.UC_RISCV_REG_PC, riscv_const.UC_RISCV_REG_SP, \
            riscv_const.UC_RISCV_REG_A0, riscv_const.UC_RISCV_REG_RA
        entry, stop = addr["start"], STOP
    mu.mem_map(BASE, SIZE)
    mu.mem_write(BASE, binf.read_bytes())
    mu.reg_write(sp, STOP)
    mu.reg_write(lr, stop)
    mu.emu_start(entry, STOP, count=1000000)
    n = 2 * (2 + ARCHES[arch]["nreg"])
    ent = struct.unpack(f"<{n}I", mu.mem_read(addr["entry_fail"], 4 * n))
    return mu.reg_read(ret), list(ent)


def run_qemu(arch, elf):
    cmd = ARCHES[arch]["qemu"] + ["-nographic", "-monitor", "none", "-kernel", str(elf)]
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        text = p.stdout + p.stderr  # armv7m: semihosting writes to stderr
    except subprocess.TimeoutExpired as e:
        text = (e.stdout or b"").decode() if isinstance(e.stdout, bytes) else (e.stdout or "")
    m = re.search(r"result (0x[0-9a-f]+)", text)
    ent = [int(x, 16) for x in re.findall(r"entry 0x[0-9a-f]+ (0x[0-9a-f]+)", text)]
    if not m or "done" not in text or len(ent) != 2 * (2 + ARCHES[arch]["nreg"]):
        print(f"  no complete result from QEMU:\n{text}")
        return None, ent
    return int(m[1], 16), ent


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--clang", default="clang")
    ap.add_argument("--lld", default="ld.lld")
    ap.add_argument("--emulator", choices=["unicorn", "qemu"], default="unicorn")
    ap.add_argument("--caller", choices=["0", "1"], action="append",
                    help="EXTRACE_CFG_FTRACE_CALLER builds (default: both)")
    ap.add_argument("arches", nargs="*", default=list(ARCHES))
    a = ap.parse_args()
    images = ["emu", "rec"] if a.emulator == "qemu" else ["emu"]
    bad = 0
    with tempfile.TemporaryDirectory() as tmp:
        for arch in a.arches:
            for image in images:
                for caller in a.caller or ["0", "1"]:
                    out = pathlib.Path(tmp) / f"{arch}-{image}-{caller}"
                    out.mkdir()
                    elf, binf, addr = build(arch, image, a.emulator, caller, a, out)
                    if a.emulator == "qemu":
                        fail, ent = run_qemu(arch, elf)
                    else:
                        fail, ent = run_unicorn(arch, binf, addr)
                    ok = fail == 0 and not any(ent)
                    res = "failed" if fail is None else f"{fail:#x}"
                    print(f"{arch} {image} CALLER={caller} on {a.emulator}: "
                          f"{'PASS' if ok else 'FAIL'} (global checks {res})")
                    shapes = 2 + ARCHES[arch]["nreg"]
                    for k, e in enumerate(ent):
                        what = "refused, " if caller == "0" and k >= shapes else ""
                        print(f"  {flavor_name(arch, k):16s} "
                              f"{f'PASS ({what}' if not e else f'FAIL {e:#x} ('}{image})")
                    bad |= not ok
                    if a.emulator == "unicorn":
                        bad |= not check_table(elf, addr["traced"])
    return bad


if __name__ == "__main__":
    sys.exit(main())
