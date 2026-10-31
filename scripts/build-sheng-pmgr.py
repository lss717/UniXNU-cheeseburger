#!/usr/bin/env python3
"""Build the guarded original-ApplePMGR integration archive.

The source is the DinnerABC/cheeseburger guarded provider.  The fixed PMGR
absolute imports are rebased from the reference prelink slide (-0xc2d000) to
the current board slide (0x5000000), and the two AON methods are read from the
current prelinked ApplePMGR vtable.
"""
import hashlib
import json
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
out = ROOT / 'build/sheng-platform-power'
out.mkdir(parents=True, exist_ok=True)

header = '''// Generated exact ApplePMGR provider ABI; private fields stay opaque.
#pragma once
#include <IOKit/IOPMGR.h>
class ApplePMGR : public IOPMGR {
 OSDeclareAbstractStructors(ApplePMGR)
 unsigned char originalPrivate[0x4b8a8-sizeof(IOPMGR)];
public:
'''
guards = ''
for i in range(177, 340):
    if i == 320:
        header += ' virtual uint32_t readAon();\n'
    elif i == 321:
        header += ' virtual void writeAon(uint64_t);\n'
    else:
        header += f' virtual void opaque{i}() = 0;\n'
        guards += f' void opaque{i}() override {{ unavailable({i}); }}\n'
header += '};\nstatic_assert(sizeof(ApplePMGR)==0x4b8a8,"Original provider storage");\n'
(out / 'OriginalPMGRABI.h').write_text(header)
(out / 'GuardedPMGRSlots.inc').write_text(guards)

base = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
record = json.loads((base / 'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))


def compile_cpp(source, obj):
    args = record['arguments']
    command, index = [], 0
    while index < len(args):
        value = args[index]
        if value == record['file']:
            command.append(str(source))
        elif value == '-o':
            command += ['-o', str(obj)]
            index += 1
        elif value in ('-MF', '-MJ', '-MT'):
            index += 1
        elif value not in ('-MD', '-MMD', '-MP'):
            command.append(value)
        index += 1
    command += ['-Wno-error', '-I', str(out), '-I', str(ROOT / 'scripts'),
                '-I', str(ROOT / '.cache/xnu-work/osfmk')]
    subprocess.run(command, cwd=record['directory'], check=True)


pmgr_source = ROOT / 'scripts/CheeseburgerPlatformPower.cpp'
pmgr_raw = out / 'CheeseburgerPlatformPower.o'
compile_cpp(pmgr_source, pmgr_raw)
glue = out / 'ShengPlatformPowerGlue.o'
compile_cpp(ROOT / 'scripts/ShengPlatformPowerGlue.cpp', glue)

llvm_objcopy = Path('/opt/local/libexec/llvm-20/bin/llvm-objcopy')
if not llvm_objcopy.is_file():
    llvm_objcopy = Path('/opt/local/bin/llvm-objcopy')
pmgr_late = out / 'CheeseburgerPlatformPower.late.o'
subprocess.run([str(llvm_objcopy), '--remove-section=__DATA,__mod_init_func',
                '--globalize-symbol=__GLOBAL__sub_I_CheeseburgerPlatformPower.cpp',
                str(pmgr_raw), str(pmgr_late)], check=True)

# Reference slide -0xc2d000 -> board slide 0x5000000.
delta = 0x5000000 - (-0xc2d000)
reference = {
    '__ZTV9ApplePMGR': 0xfffffff0060b56d8 - 16,
    '__ZN9ApplePMGRC2EPK11OSMetaClass': 0xfffffff004dc303c,
    '__ZN9ApplePMGRD2Ev': 0xfffffff004dc319c,
    '__ZN9ApplePMGR10gMetaClassE': 0xfffffff006eb2490,
    '_sheng_original_pmgr_nub_create': 0xfffffff004df5568,
    '_sheng_original_pmgr_nub_read': 0xfffffff004df7adc,
    '_sheng_original_pmgr_nub_write': 0xfffffff004df60ec,
}
bindings = {name: value + delta for name, value in reference.items()}

# The two AON methods live in the original ApplePMGR vtable; read their
# current pointers from the prelinked kernel rather than fabricating them.
sys.path.insert(0, str(ROOT / 'kits/NativePrelink'))
import native_prelink as np  # noqa: E402
kernel_path = ROOT.parent / 'kernel/kernel.macho'
image_data = kernel_path.read_bytes()
image = np.macho(image_data)
vtable_slot = bindings['__ZTV9ApplePMGR'] + 16
for name, slot in (('__ZN9ApplePMGR7readAonEv', 320), ('__ZN9ApplePMGR8writeAonEy', 321)):
    va = vtable_slot + slot * 8
    offset = np.backed_offset(image['segments'], va, 8, len(image_data))
    bindings[name] = struct.unpack_from('<Q', image_data, offset)[0]

assembly = out / 'original-bindings.S'
assembly.write_text(''.join(f'.globl {name}\n.set {name}, {value:#x}\n'
                            for name, value in sorted(bindings.items())))
sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
binding_object = out / 'original-bindings.o'
subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-isysroot', sdk,
                '-c', str(assembly), '-o', str(binding_object)], check=True)

archive = out / 'libShengPlatformPower.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', str(archive), str(pmgr_late), str(glue), str(binding_object)], check=True)
report = dict(result='SHENG_PMGR_RUNTIME_BUILD_PASS',
              archive_sha256=hashlib.sha256(archive.read_bytes()).hexdigest(),
              source_sha256=hashlib.sha256(pmgr_source.read_bytes()).hexdigest(),
              bindings={name: hex(value) for name, value in bindings.items()},
              reference_slide=-0xc2d000, board_slide=0x5000000, hardware_tested=False)
(out / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
