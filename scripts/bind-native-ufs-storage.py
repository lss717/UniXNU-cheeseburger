#!/usr/bin/env python3
"""Bind NativeUfsLun's inherited IOBlockStorageDevice slots to the live B3 class.

The storage headers and class metadata are pinned in scripts/data/IOStorageFamily-bindings.json,
generated from the B3 kernel's IOStorageFamily __DATA_CONST vtable.  This script only appends
absolute aliases for the undefined original storage imports; it does not change their code.
"""
import argparse
import json
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
for base in (Path.cwd(), Path.cwd().parent, ROOT):
    if (base / 'kits/NativePrelink/native_prelink.py').exists():
        sys.path.insert(0, str(base / 'kits/NativePrelink'))
        break
import native_prelink as np

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--object', type=Path, required=True)
parser.add_argument('--archive', type=Path, required=True)
parser.add_argument('--bindings', type=Path, default=ROOT / 'scripts/data/IOStorageFamily-bindings.json')
args = parser.parse_args()

bindings = json.loads(args.bindings.read_text())
slots = [int(value, 16) for value in bindings['slots']]
if len(slots) != 190:
    raise SystemExit('storage binding table is not 190 slots')
fixed = {
    '__ZN20IOBlockStorageDeviceC2EPK11OSMetaClass': int(bindings['constructor'], 16),
    '__ZN20IOBlockStorageDeviceD2Ev': int(bindings['destructor'], 16),
    '__ZN20IOBlockStorageDevice10gMetaClassE': int(bindings['metaclass'], 16),
    '__ZTV20IOBlockStorageDevice': int(bindings['vtable'], 16),
}

obj = args.object.read_bytes()
image = np.macho(obj)
symcmd = next(cmd for cmd, at, size in image['commands'] if cmd == 2)
symoff, count, stroff, strsize = struct.unpack_from('<4I', obj, symcmd + 8)
symbols = []
print(f'NativeUfs symbols: symoff={symoff} count={count} stroff={stroff} strsize={strsize} size={len(obj)}')
for index in range(count):
    name_at, kind, sect, desc, value = struct.unpack_from('<IBBHQ', obj, symoff + index * 16)
    start = stroff + name_at
    if index < 8:
        print(f'  sym[{index}] strx={name_at} kind={kind} value={value:#x} start={start}')
    if start < 0 or start >= len(obj):
        name = ''
    else:
        end = obj.find(b'\0', start)
        if end < 0:
            end = min(len(obj), start + 512)
        name = obj[start:end].decode(errors='replace')
    symbols.append((name, value, kind))

vtable = next((value for name, value, kind in symbols
               if name == '__ZTV12NativeUfsLun'), None)
if not vtable:
    raise SystemExit('NativeUfsLun vtable missing')
relocations = {}
for segment in image['segments']:
    for section in segment['sections']:
        if section['va'] <= vtable < section['va'] + section['size']:
            reloc_at, reloc_count = struct.unpack_from('<II', obj, section['command_offset'] + 56)
            for index in range(reloc_count):
                address, word = struct.unpack_from('<II', obj, reloc_at + index * 8)
                if word >> 27 & 1 and word >> 28 == 0 and word >> 25 & 3 == 3:
                    relocations[section['va'] + address] = symbols[word & 0xffffff][0]
slot_names = [relocations.get(vtable + 16 + 8 * index) for index in range(190)]
if not all(slot_names):
    raise SystemExit('NativeUfsLun vtable has missing slots')

undefined = [name for name, value, kind in symbols
             if kind == 1 and 'IOBlockStorageDevice' in name]
out = {}
for name in undefined:
    if name in fixed:
        out[name] = fixed[name]
        continue
    positions = [index for index, slot in enumerate(slot_names) if slot == name]
    targets = {slots[index] for index in positions}
    if not positions or len(targets) != 1:
        raise SystemExit(f'cannot uniquely bind {name}: slots={positions} targets={targets}')
    out[name] = targets.pop()
if not out:
    raise SystemExit('no original storage imports to bind')

assembly = args.archive.with_name('NativeUfsLun-original-bindings.S')
assembly.write_text(''.join(
    f'.globl {name}\n.set {name}, {address:#x}\n' for name, address in sorted(out.items())))
binding_object = args.archive.with_name('NativeUfsLun-original-bindings.o')
subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-Werror',
                '-Wno-incompatible-sysroot', '-c', str(assembly), '-o', str(binding_object)],
               check=True)
subprocess.run(['xcrun', 'ar', 'rcs', str(args.archive), str(args.object), str(binding_object)],
               check=True)
print(f'PASS: bound {len(out)} original IOStorageFamily imports '
      f'({len(slot_names)} NativeUfsLun slots checked)')
