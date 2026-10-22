#!/usr/bin/env python3
"""Package the edk2-msm8998 firmware and a UniXNU payload into a boot image.

The B line does not use "unixnu bootimg": that command wraps a Mu-Silicium BootShim, while
edk2-msm8998 boots a gzip(FV)+DTB kernel from the stock ABL (plan decision D6/D7).  This
script drives edk2-msm8998's own mkbootimg.py, then re-parses the result and writes a JSON
report next to the image.

The addresses below are the ones the phone accepted on 2026-10-22:
kernel 0x10000000, ramdisk 0x12000000, tags 0x18000000.  The 64 MiB payload ends at
0x16000000, so the tags moved up from the B1 value 0x14000000.  A non-empty ramdisk must not use
offset 0, or the 32 MiB payload overwrites the kernel.
"""

import argparse
import hashlib
import json
import os
import struct
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
DEFAULT_EDK2 = ('/home/ls/edk2-msm8998',)


def find_edk2(explicit):
    candidates = []
    if explicit:
        candidates.append(Path(explicit))
    if os.environ.get('EDK2_MSM8998'):
        candidates.append(Path(os.environ['EDK2_MSM8998']))
    candidates.append(REPO.parent / 'edk2-msm8998')
    candidates.extend(Path(p) for p in DEFAULT_EDK2)
    for candidate in candidates:
        if (candidate / 'mkbootimg.py').is_file():
            return candidate.resolve()
    raise SystemExit('edk2-msm8998 not found: pass --edk2 DIR or set EDK2_MSM8998')


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as handle:
        for block in iter(lambda: handle.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def align(value, page):
    return (value + page - 1) // page * page


def parse_header(path):
    with open(path, 'rb') as handle:
        data = handle.read(0x100)
    if data[:8] != b'ANDROID!':
        raise SystemExit(f'{path}: not an Android boot image')
    (kernel_size, kernel_addr, ramdisk_size, ramdisk_addr,
     second_size, second_addr, tags_addr, page_size,
     header_version) = struct.unpack_from('<9I', data, 8)
    return {
        'header_version': header_version,
        'page_size': page_size,
        'kernel': {'size': kernel_size, 'address': kernel_addr},
        'ramdisk': {'size': ramdisk_size, 'address': ramdisk_addr},
        'second': {'size': second_size, 'address': second_addr},
        'tags_address': tags_addr,
    }


def fat_label(path):
    with open(path, 'rb') as handle:
        sector = handle.read(512)
    if len(sector) < 512 or sector[510:512] != b'\x55\xaa':
        return None
    root_entries = int.from_bytes(sector[17:19], 'little')
    offset = 43 if root_entries else 71
    return sector[offset:offset + 11].decode('ascii', 'replace').strip('\x00 ')


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--edk2', help='edk2-msm8998 checkout (default: $EDK2_MSM8998 or ../edk2-msm8998)')
    parser.add_argument('--kernel', help='default: <edk2>/workspace/uefi-cheeseburger.img.gz-dtb')
    parser.add_argument('--payload', help='default: out/cheeseburger/boot/payload.img')
    parser.add_argument('--out', default=str(REPO / 'out/cheeseburger/boot/boot.img'))
    parser.add_argument('--base', type=lambda v: int(v, 0), default=0x10000000)
    parser.add_argument('--kernel-offset', type=lambda v: int(v, 0), default=0)
    parser.add_argument('--ramdisk-offset', type=lambda v: int(v, 0), default=0x02000000)
    parser.add_argument('--tags-offset', type=lambda v: int(v, 0), default=0x08000000)
    parser.add_argument('--pagesize', type=int, default=2048, choices=(2048, 4096, 8192, 16384))
    parser.add_argument('--header-version', type=int, default=1)
    args = parser.parse_args()

    edk2 = find_edk2(args.edk2)
    kernel = Path(args.kernel) if args.kernel else edk2 / 'workspace/uefi-cheeseburger.img.gz-dtb'
    payload = Path(args.payload) if args.payload else REPO / 'out/cheeseburger/boot/payload.img'
    out = Path(args.out)
    for path, what in ((kernel, 'firmware kernel'), (payload, 'payload')):
        if not path.is_file():
            raise SystemExit(f'{what} not found: {path}')
    # mkbootimg runs from the edk2 tree, so every path has to be absolute.
    kernel = kernel.resolve()
    payload = payload.resolve()
    out = out.resolve()

    label = fat_label(payload)
    if label != 'OP5ESP':
        print(f'warning: payload label is {label!r}, expected OP5ESP', file=sys.stderr)

    out.parent.mkdir(parents=True, exist_ok=True)
    command = [
        sys.executable, str(edk2 / 'mkbootimg.py'),
        '--kernel', str(kernel),
        '--ramdisk', str(payload),
        '--base', hex(args.base),
        '--kernel_offset', hex(args.kernel_offset),
        '--ramdisk_offset', hex(args.ramdisk_offset),
        '--tags_offset', hex(args.tags_offset),
        '--pagesize', str(args.pagesize),
        '--header_version', str(args.header_version),
        '--os_version', '12.0.0',
        '--os_patch_level', '2022-08',
        '-o', str(out),
    ]
    print('+ ' + ' '.join(command))
    subprocess.run(command, check=True, cwd=edk2)

    header = parse_header(out)
    problems = []
    if header['header_version'] != args.header_version:
        problems.append('header version mismatch')
    if header['page_size'] != args.pagesize:
        problems.append('page size mismatch')
    expected_kernel = args.base + args.kernel_offset
    expected_ramdisk = args.base + args.ramdisk_offset
    expected_tags = args.base + args.tags_offset
    if header['kernel']['address'] != expected_kernel:
        problems.append(f"kernel address 0x{header['kernel']['address']:x} != 0x{expected_kernel:x}")
    if header['ramdisk']['address'] != expected_ramdisk:
        problems.append(f"ramdisk address 0x{header['ramdisk']['address']:x} != 0x{expected_ramdisk:x}")
    if header['tags_address'] != expected_tags:
        problems.append(f"tags address 0x{header['tags_address']:x} != 0x{expected_tags:x}")

    kernel_end = header['kernel']['address'] + align(header['kernel']['size'], args.pagesize)
    ramdisk_end = header['ramdisk']['address'] + align(header['ramdisk']['size'], args.pagesize)
    if kernel_end > header['ramdisk']['address']:
        problems.append('kernel overlaps the ramdisk')
    if ramdisk_end > header['tags_address']:
        problems.append('ramdisk overlaps the tags')
    if header['ramdisk']['size'] == 0:
        problems.append('ramdisk is empty')

    needed = (args.pagesize
              + align(header['kernel']['size'], args.pagesize)
              + align(header['ramdisk']['size'], args.pagesize)
              + align(header['second']['size'], args.pagesize))
    if out.stat().st_size < needed:
        problems.append(f'image is {out.stat().st_size} bytes, header needs {needed}')

    report = {
        'edk2': str(edk2),
        'output': str(out),
        'output_sha256': sha256(out),
        'output_size': out.stat().st_size,
        'inputs': {
            'kernel': {'path': str(kernel), 'size': kernel.stat().st_size, 'sha256': sha256(kernel)},
            'payload': {'path': str(payload), 'size': payload.stat().st_size, 'sha256': sha256(payload),
                        'label': label},
        },
        'image': header,
        'load': {
            'kernel': [hex(header['kernel']['address']), hex(kernel_end)],
            'ramdisk': [hex(header['ramdisk']['address']), hex(ramdisk_end)],
            'tags': hex(header['tags_address']),
        },
        'problems': problems,
    }
    report_path = out.with_suffix('.json')
    report_path.write_text(json.dumps(report, indent=2) + '\n')

    print(json.dumps(report['load'], indent=2))
    print(f'output {out} ({report["output_size"]} bytes) {report["output_sha256"]}')
    print(f'report {report_path}')
    if problems:
        for problem in problems:
            print(f'FAIL {problem}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
