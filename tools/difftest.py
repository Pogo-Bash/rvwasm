#!/usr/bin/env python3
"""Differential test: run a riscv64 binary under qemu-riscv64 and under rv,
then compare the register state before every instruction.

Stack addresses differ between the two (QEMU places the guest stack high in a
sparse 64-bit address space, rv at the top of its arena), so values that point
into the stack on both sides are compared as offsets from the initial stack
pointer instead of as absolute values.
"""

import argparse
import itertools
import os
import re
import subprocess
import sys
import tempfile

REG_RE = re.compile(r"[xf]\d+/\w+\s+([0-9a-f]{16})")
PC_RE = re.compile(r"^ pc\s+([0-9a-f]{16})")

# How far below / above the initial stack pointer still counts as "the stack".
STACK_BELOW = 8 << 20
STACK_ABOVE = 1 << 20

REG_NAMES = (
    "zero ra sp gp tp t0 t1 t2 s0 s1 a0 a1 a2 a3 a4 a5 a6 a7 "
    "s2 s3 s4 s5 s6 s7 s8 s9 s10 s11 t3 t4 t5 t6"
).split()


def qemu_states(path):
    """Yield (pc, [x1..x31], [f0..f31]) from a `qemu -d cpu,fpu` log."""
    pc = None
    regs = []
    with open(path) as log:
        for line in log:
            m = PC_RE.match(line)
            if m:
                pc = int(m.group(1), 16)
                regs = []
                continue
            if pc is None:
                continue
            found = REG_RE.findall(line)
            if not found:
                continue
            regs.extend(int(v, 16) for v in found)
            if len(regs) == 64:
                yield pc, regs[1:32], regs[32:]
                pc = None


def rv_states(path):
    """Yield (pc, [x1..x31], [f0..f31]) from an `rv -t` trace."""
    with open(path) as trace:
        for line in trace:
            vals = [int(v, 16) for v in line.split()]
            yield vals[0], vals[1:32], vals[32:]


def in_stack(value, sp0):
    return sp0 - STACK_BELOW <= value < sp0 + STACK_ABOVE


def same(q, o, q_sp0, o_sp0, regions, ids=()):
    if q == o:
        return True
    # Process ids: each emulator is its own process.
    if (q, o) in ids:
        return True
    # Pointers into a mapping both sides made with the same mmap call. A page
    # of slack past the end is allowed: optimised loops step an induction
    # pointer beyond the mapping before the bounds check. Checked before the
    # stack rule because either side may place a mapping close to its stack.
    for q_base, o_base, length in reversed(regions):
        if q_base <= q <= q_base + length + PAGE and o_base <= o <= o_base + length + PAGE:
            return q - q_base == o - o_base
    if in_stack(q, q_sp0) and in_stack(o, o_sp0):
        # The live stack must match exactly, relative to the initial sp. The
        # argv/auxv area above it is laid out differently by each loader.
        if q < q_sp0 or o < o_sp0:
            return q - q_sp0 == o - o_sp0
        return True
    return False


# Syscalls that return the address of a mapping, and the index (into
# [x1..x31]) of the argument register holding its length.
MAPPING_SYSCALLS = {
    222: 10,  # mmap(addr, len, ...): len in a1
    216: 11,  # mremap(old, old_len, new_len, ...): new_len in a2
}
PAGE = 4096

# Syscalls whose result is a process or thread id (getpid, getppid, gettid).
ID_SYSCALLS = {172, 173, 178}


def is_error(value):
    """Linux syscalls return -errno as a value in the top 4095 addresses."""
    return value >= (1 << 64) - 4095


def mmap_region(prev_regs, q_regs, o_regs):
    """If the previous instruction was a successful mmap or mremap on both
    sides, return the (qemu base, rv base, length) pair it created."""
    a0, a7 = 9, 16  # indices into [x1..x31]
    if prev_regs is None or prev_regs[a7] not in MAPPING_SYSCALLS:
        return None
    if is_error(q_regs[a0]) or is_error(o_regs[a0]):
        return None
    length = (prev_regs[MAPPING_SYSCALLS[prev_regs[a7]]] + PAGE - 1) & ~(PAGE - 1)
    return q_regs[a0], o_regs[a0], length


def disassembly(prog):
    lines = {}
    try:
        out = subprocess.run(
            ["riscv64-linux-gnu-objdump", "-d", prog],
            capture_output=True, text=True, check=True,
        ).stdout
    except (OSError, subprocess.CalledProcessError):
        return lines
    for line in out.splitlines():
        m = re.match(r"^\s*([0-9a-f]+):\s+(.*)$", line)
        if m:
            lines[int(m.group(1), 16)] = m.group(2).strip()
    return lines


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--rv", default="rv", help="path to the rv binary")
    ap.add_argument("--qemu", default="qemu-riscv64")
    ap.add_argument("--keep", action="store_true", help="keep the trace files")
    ap.add_argument("--output-only", action="store_true",
                    help="compare only output and exit status, not every instruction")
    ap.add_argument("prog")
    ap.add_argument("args", nargs="*")
    opts = ap.parse_args()

    tmp = tempfile.mkdtemp(prefix="difftest-")
    q_log = os.path.join(tmp, "qemu.log")
    o_log = os.path.join(tmp, "rv.trace")
    guest = [opts.prog] + opts.args

    if opts.output_only:
        q = subprocess.run([opts.qemu] + guest, capture_output=True, env={})
        o = subprocess.run([opts.rv] + guest, capture_output=True, env={})
        for path in (q_log, o_log):
            open(path, "w").close()
    else:
        q = subprocess.run(
            [opts.qemu, "-one-insn-per-tb", "-d", "cpu,fpu,nochain", "-D", q_log] + guest,
            capture_output=True, env={},
        )
        o = subprocess.run([opts.rv, "-t", o_log] + guest, capture_output=True, env={})

    asm = disassembly(opts.prog)
    failed = False
    steps = 0
    q_sp0 = o_sp0 = None
    prev_pc = None
    prev_regs = None
    prev_was_ecall = False
    regions = []
    ids = set()

    for qs, os_ in itertools.zip_longest(qemu_states(q_log), rv_states(o_log)):
        if qs is None or os_ is None:
            which = "rv" if qs is None else "qemu"
            print(f"FAIL: {which} executed more instructions (diverged after step {steps})")
            if prev_pc is not None:
                print(f"  last common instruction {prev_pc:#x}: {asm.get(prev_pc, '?')}")
            failed = True
            break
        (q_pc, q_regs, q_fregs), (o_pc, o_regs, o_fregs) = qs, os_
        if q_sp0 is None:
            q_sp0, o_sp0 = q_regs[1], o_regs[1]

        if prev_was_ecall:
            region = mmap_region(prev_regs, q_regs, o_regs)
            if region:
                regions.append(region)
            if prev_regs[16] in ID_SYSCALLS:  # a7
                ids.add((q_regs[9], o_regs[9]))  # a0

        bad = []
        # The pc can be inside a mapping too (code generated at run time).
        if not same(q_pc, o_pc, q_sp0, o_sp0, regions, ids):
            bad.append(f"  pc: qemu {q_pc:#x}, rv {o_pc:#x}")
        for i, (qv, ov) in enumerate(zip(q_regs, o_regs), start=1):
            if not same(qv, ov, q_sp0, o_sp0, regions, ids):
                bad.append(f"  x{i}/{REG_NAMES[i]}: qemu {qv:#018x}, rv {ov:#018x}")
        # Floating-point registers hold values, never addresses: exact match.
        for i, (qv, ov) in enumerate(zip(q_fregs, o_fregs)):
            if qv != ov:
                bad.append(f"  f{i}: qemu {qv:#018x}, rv {ov:#018x}")
        if bad:
            print(f"FAIL: state differs before step {steps} (pc {q_pc:#x})")
            if prev_pc is not None:
                print(f"  after executing {prev_pc:#x}: {asm.get(prev_pc, '?')}")
            print("\n".join(bad))
            failed = True
            break
        prev_pc = q_pc
        prev_regs = q_regs
        prev_was_ecall = asm.get(q_pc, "").split("\t")[-1].strip() == "ecall"
        steps += 1

    if q.stdout != o.stdout:
        print("FAIL: stdout differs")
        print(f"  qemu: {q.stdout[:200]!r}")
        print(f"  rv:   {o.stdout[:200]!r}")
        failed = True
    if q.returncode != o.returncode:
        print(f"FAIL: exit status differs: qemu {q.returncode}, rv {o.returncode}")
        if o.stderr:
            print(f"  rv stderr: {o.stderr.decode(errors='replace').strip()[:300]}")
        failed = True

    if not failed:
        what = "output identical" if opts.output_only else f"{steps} instructions identical"
        print(f"ok: {os.path.basename(opts.prog)}: {what}, "
              f"exit status {q.returncode}, {len(q.stdout)} bytes of output")

    if opts.keep:
        print(f"traces kept in {tmp}")
    else:
        for path in (q_log, o_log):
            if os.path.exists(path):
                os.unlink(path)
        os.rmdir(tmp)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
