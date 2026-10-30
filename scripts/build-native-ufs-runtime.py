#!/usr/bin/env python3
"""Assemble the full NativeUfs + VerifiedRoot runtime archive.

The archive contains the signed owner policy inputs, the VerifiedRoot C core,
the UFS root-authorization adapter, the full NativeUfs driver and the original
IOStorageFamily ABI aliases.  It is linked only when XNU_NATIVE_UFS=1.
"""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', default='build/native-ufs')
args = parser.parse_args()
out = ROOT / args.output
out.mkdir(parents=True, exist_ok=True)
policy = out / 'policy'
policy.mkdir(parents=True, exist_ok=True)

os_manifest = ROOT / 'build/verified-root-os/manifest.bin'
systemos_manifest = ROOT / 'build/verified-root-systemos/manifest.bin'
owner_key = ROOT / 'build/verified-root-os/trusted-key.bin'
for path in (os_manifest, systemos_manifest, owner_key):
    if not path.is_file():
        raise SystemExit(f'missing signed policy input: {path}')

assembly = policy / 'inputs.S'
assembly.write_text(
    '.section __DATA_CONST,__const\n.balign 8\n'
    f'.globl _verified_root_os_manifest\n_verified_root_os_manifest:\n.incbin "{os_manifest}"\n'
    f'.globl _verified_root_systemos_manifest\n_verified_root_systemos_manifest:\n.incbin "{systemos_manifest}"\n'
    f'.balign 8\n.globl _verified_root_key\n_verified_root_key:\n.incbin "{owner_key}"\n'
    '.balign 8\n.globl _verified_root_policies,_verified_root_policy_count\n'
    '_verified_root_policies:\n'
    f'.quad _verified_root_os_manifest, {os_manifest.stat().st_size}\n'
    f'.quad _verified_root_systemos_manifest, {systemos_manifest.stat().st_size}\n'
    f'_verified_root_policy_count:\n.quad 2\n')

base = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
cpp_record = json.loads((base / 'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))
c_record = json.loads((base / 'bsd/DEVELOPMENT/bsd_init.o.json').read_text().strip().rstrip(','))

objects = []


def compile_source(source, name, record, extra):
    source = Path(source)
    obj = out / (name + '.o')
    args = record['arguments']
    command = []
    index = 0
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
    command += ['-Wno-error', *extra]
    result = subprocess.run(command, cwd=record['directory'], capture_output=True, text=True)
    (out / (name + '-compile.log')).write_text(result.stdout + result.stderr)
    if result.returncode:
        raise SystemExit(f'{name} compile failed:\n' + (result.stdout + result.stderr)[-3500:])
    objects.append(obj)
    return obj


common = ['-I', str(ROOT / 'build/storage-abi/include'),
          '-I', str(ROOT / 'kits/VerifiedRoot/include'),
          '-I', str(ROOT / 'drivers/NativeVirtioBlock'),
          '-mgeneral-regs-only']
compile_source(ROOT / 'drivers/NativeUfs/NativeUfs.cpp', 'NativeUfs', cpp_record,
               common + ['-DNATIVE_VERIFIED_BLOCK=1', '-DNATIVE_VERIFIED_BLOCK_MULTI_POLICY=1'])
compile_source(ROOT / 'runtime/RootAuth.cpp', 'RootAuth', cpp_record, common)
compile_source(ROOT / 'kits/VerifiedRoot/src/verified_root.c', 'verified_root', c_record,
               ['-I', str(ROOT / 'kits/VerifiedRoot/include')])
compile_source(ROOT / 'kits/VerifiedRoot/adapters/xnu_crypto.c', 'xnu_crypto', c_record,
               ['-I', str(ROOT / 'kits/VerifiedRoot/include')])
inputs_object = policy / 'inputs.o'
sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-isysroot', sdk, '-c',
                str(assembly), '-o', str(inputs_object)], check=True)
objects.append(inputs_object)

bindings = ROOT / 'build/native-ufs-probe/NativeUfsLun-original-bindings.o'
if not bindings.is_file():
    raise SystemExit(f'missing original storage bindings: {bindings}; build NativeUfsProbe first')
objects.append(bindings)

archive = out / 'libNativeUfs.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', str(archive), *[str(o) for o in objects]], check=True)
report = dict(result='NATIVE_UFS_RUNTIME_BUILD_PASS', objects=[str(o.relative_to(ROOT)) for o in objects],
              archive_sha256=hashlib.sha256(archive.read_bytes()).hexdigest(),
              verified_root_manifest_bytes=os_manifest.stat().st_size,
              verified_systemos_manifest_bytes=systemos_manifest.stat().st_size,
              hardware_tested=False)
(out / 'runtime-report.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
