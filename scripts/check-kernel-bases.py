#!/usr/bin/env python3
"""Check the B3 kernel segment bases, DarwinBoot physBase arithmetic and reset probes.

The prelinked kernel has two interesting virtual bases:

  Base      lowest file-backed segment (__PRELINK_TEXT)
  VirtBase  __TEXT segment

DarwinBoot maps Base to the allocated window start (Address) and the kernel text
lives at VirtBase, so the matching physical base is

  physBase = Address + (VirtBase - Base)

The bootstrap page tables build 2 MiB blocks.  The low 21 bits of physBase must
therefore equal the low 21 bits of VirtBase, or the first instruction after
SCTLR_EL1 faults.  AllocateWindow already rounds the window start to

  ALIGN_VALUE(conventional_start, 2MiB) + (Base & (2MiB - 1))

and this tool re-checks the resulting congruence from the Mach-O and the
DARWINBOOT log line.

It also finds the "wait N seconds, then PSCI SYSTEM_RESET" caves used by
scripts/patch-xnu-entry-probe.py, so a recorded reboot can be compared with the
actual delay instead of being called a hang.

Example:

  scripts/check-kernel-bases.py kernel.macho --address 0x1B007C000
"""

import argparse
import struct
from pathlib import Path

MASK_2M = (1 << 21) - 1
CNTPCT_X1 = 0xD53BE021       # mrs x1, CNTPCT_EL0
SMC_0 = 0xD4000003           # smc #0
LDR_X2 = 0x58000000          # ldr x2, #imm19
TEXT_NAME = b'__TEXT'


def load_commands(data):
    magic, _, _, _, ncmds = struct.unpack_from('<IiiII', data, 0)
    if magic != 0xFEEDFACF:
        raise SystemExit('not a 64-bit Mach-O')
    offset = 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, offset)
        yield cmd, offset
        offset += size


def segments(data):
    result = []
    for cmd, offset in load_commands(data):
        if cmd != 0x19:
            continue
        name = data[offset + 8:offset + 24].rstrip(b'\0').decode('ascii', 'replace')
        vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<QQQQ', data, offset + 24)
        if filesize:
            result.append((name, vmaddr, vmsize, fileoff, filesize))
    return result


def entry_pc(data):
    for cmd, offset in load_commands(data):
        if cmd == 0x5:
            return struct.unpack_from('<Q', data, offset + 16 + 32 * 8)[0]
    raise SystemExit('no LC_UNIXTHREAD in the kernel')


def file_offset(segs, va, size=4):
    for _, vmaddr, _, fileoff, filesize in segs:
        if vmaddr <= va and va + size <= vmaddr + filesize:
            return fileoff + va - vmaddr
    return None


def reset_probes(data, segs, timebase):
    """Find mrs CNTPCT_EL0 ... ldr x2, literal ... smc #0 sequences."""
    probes = []
    for name, vmaddr, _, fileoff, filesize in segs:
        start = fileoff
        end = fileoff + filesize
        offset = start
        while offset + 4 <= end:
            word = struct.unpack_from('<I', data, offset)[0]
            if word != CNTPCT_X1:
                offset += 4
                continue
            va = vmaddr + offset - fileoff
            # The ldr x2 literal follows within the next few instructions.
            literal = None
            for step in range(1, 8):
                at = offset + step * 4
                if at + 4 > end:
                    break
                insn = struct.unpack_from('<I', data, at)[0]
                if (insn & 0xFF000000) == LDR_X2:
                    imm = (insn >> 5) & 0x1FFFF
                    imm -= (1 << 18) if imm & (1 << 17) else 0
                    target = va + step * 4 + imm * 4
                    target_offset = file_offset(segs, target)
                    if target_offset is not None:
                        literal = struct.unpack_from('<I', data, target_offset)[0]
                    break
            # A PSCI SYSTEM_RESET soon after the compare loop.
            has_smc = any(struct.unpack_from('<I', data, offset + step * 4)[0] == SMC_0
                          for step in range(1, 24)
                          if offset + step * 4 + 4 <= end)
            if literal is not None and has_smc and 0 < literal < 1 << 32:
                probes.append({'va': va, 'ticks': literal, 'seconds': literal / timebase})
            offset += 4
    return probes


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('kernel', type=Path)
    parser.add_argument('--address', type=lambda v: int(v, 0),
                        help='DarwinBoot window start (pa) from the DARWINBOOT log')
    parser.add_argument('--base', type=lambda v: int(v, 0), help='override the lowest segment')
    parser.add_argument('--virt', type=lambda v: int(v, 0), help='override the __TEXT segment')
    parser.add_argument('--timebase', type=int, default=19_200_000, help='CNTFRQ in Hz (default 19.2 MHz)')
    args = parser.parse_args()

    data = args.kernel.read_bytes()
    segs = segments(data)
    if not segs:
        raise SystemExit('no file-backed segments')
    base = args.base or min(seg[1] for seg in segs)
    text = next((seg for seg in segs if seg[0].encode() == TEXT_NAME), None)
    if text is None:
        raise SystemExit('no __TEXT segment')
    virt = args.virt or text[1]

    print(f'kernel   {args.kernel}')
    print(f'entry    {entry_pc(data):#x}')
    print(f'Base     {base:#x}  (lowest file-backed segment)')
    print(f'VirtBase {virt:#x}  (__TEXT)')
    print(f'delta    {virt - base:#x}')
    print(f'Base & 2MiB-1   {base & MASK_2M:#x}')
    print(f'Virt & 2MiB-1   {virt & MASK_2M:#x}')

    if args.address is not None:
        phys = args.address + virt - base
        print(f'Address  {args.address:#x}  (DarwinBoot window start)')
        print(f'physBase {phys:#x}  = Address + (VirtBase - Base)')
        print(f'phys & 2MiB-1   {phys & MASK_2M:#x}')
        congruent = (phys & MASK_2M) == (virt & MASK_2M)
        print(f'2 MiB offset congruent: {"yes" if congruent else "NO"}')
        if not congruent:
            return 1

    probes = reset_probes(data, segs, args.timebase)
    if probes:
        print('reset probes:')
        for probe in probes:
            print(f'  {probe["va"]:#x}  {probe["ticks"]} ticks  {probe["seconds"]:.3f} s')
    else:
        print('reset probes: none')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
