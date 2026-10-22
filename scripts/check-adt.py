#!/usr/bin/env python3
"""Parse and check the Apple-format device tree that make-cheeseburger-adt.py writes.

The format is not FDT: each node is a pair of u32 counts (properties, children), then
32-byte key + u32 length + padded data for every property, then the children.

Checks the nodes the XNU platform expert and DarwinBoot read first: /cpus, /arm-io/intc,
/chosen, /defaults, /options, /buttons and /product.
"""

import argparse
import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]


class Report:
    def __init__(self):
        self.errors = []

    def check(self, condition, message):
        if condition:
            print(f'  ok    {message}')
        else:
            self.errors.append(message)
            print(f'  FAIL  {message}')


def parse_node(data, offset):
    props, children = struct.unpack_from('<II', data, offset)
    offset += 8
    node = {'props': {}, 'children': []}
    for _ in range(props):
        key = data[offset:offset + 32].split(b'\0')[0].decode('ascii', 'replace')
        offset += 32
        length, = struct.unpack_from('<I', data, offset)
        offset += 4
        node['props'][key] = data[offset:offset + length]
        offset += (length + 3) & ~3
    for _ in range(children):
        child, offset = parse_node(data, offset)
        node['children'].append(child)
    name = node['props'].get('name', b'')
    node['name'] = name.split(b'\0')[0].decode('ascii', 'replace')
    return node, offset


def child(node, name):
    return next((item for item in node['children'] if item['name'] == name), None)


def u32(node, key):
    value = node['props'].get(key)
    return None if value is None else struct.unpack('<I', value)[0]


def u64(node, key):
    value = node['props'].get(key)
    return None if value is None else struct.unpack('<Q', value)[0]


def text(node, key):
    value = node['props'].get(key)
    return None if value is None else value.split(b'\0')[0].decode('ascii', 'replace')


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--adt', default=str(REPO / 'out/cheeseburger/boot/kernel.macho.adt'))
    parser.add_argument('--identity', default='ipad7,1', choices=('ipad7,1', 'iphone10,2'))
    parser.add_argument('--dram-size', type=lambda value: int(value, 0), default=0x200000000)
    args = parser.parse_args()

    path = Path(args.adt)
    if not path.is_file():
        raise SystemExit(f'ADT not found: {path} (run scripts/make-cheeseburger-adt.py first)')

    data = path.read_bytes()
    root, end = parse_node(data, 0)
    report = Report()
    report.check(end == len(data), f'whole file consumed ({end} of {len(data)} bytes)')
    report.check(root['name'] == 'device-tree', 'root is device-tree')

    expected_model = 'iPad7,1' if args.identity == 'ipad7,1' else 'iPhone10,2'
    report.check((text(root, 'model') or '').startswith(expected_model),
                 f'root model is {expected_model}')
    compatible = root['props'].get('compatible', b'')
    report.check(b'qcom,msm8998' in compatible, 'root compatible names qcom,msm8998')

    cpus = child(root, 'cpus')
    report.check(cpus is not None, 'has /cpus')
    if cpus:
        regs = [u32(item, 'reg') for item in cpus['children']]
        report.check(regs == [0, 1, 2, 3, 0x100, 0x101, 0x102, 0x103],
                     f'/cpus regs are the MSM8998 MPIDR set ({regs})')
        report.check(all(u32(item, 'timebase-frequency') == 19200000 for item in cpus['children']),
                     '/cpus timebase-frequency is 19200000')
        compatibles = [text(item, 'compatible') for item in cpus['children']]
        report.check(compatibles[:4] == ['arm,cortex-a73'] * 4 and compatibles[4:] == ['arm,cortex-a53'] * 4,
                     '/cpus are 4x cortex-a73 and 4x cortex-a53')
    report.check(child(root, 'psci') is not None, 'has /psci (secondary CPUs)')

    armio = child(root, 'arm-io')
    report.check(armio is not None, 'has /arm-io')
    if armio:
        intc = child(armio, 'intc')
        report.check(intc is not None, 'has /arm-io/intc')
        if intc:
            reg = struct.unpack('<4Q', intc['props'].get('reg', bytes(32)))
            report.check(reg == (0xA00000, 0x10000, 0xB00000, 0x100000),
                         f'GIC offsets under 0x17000000 are 0x17a00000/0x17b00000 ({[hex(v) for v in reg]})')
            report.check(u64(intc, 'redistributor-stride') == 0x20000, 'GICR stride is 0x20000')
        ufshc = child(armio, 'ufshc')
        report.check(ufshc is not None and text(ufshc, 'compatible') == 'qcom,msm8998-ufshc',
                     '/arm-io/ufshc is qcom,msm8998-ufshc')

    chosen = child(root, 'chosen')
    report.check(chosen is not None, 'has /chosen')
    if chosen:
        report.check(u64(chosen, 'dram-base') == 0x80000000, '/chosen dram-base is 0x80000000')
        report.check(u64(chosen, 'dram-size') == args.dram_size,
                     f'/chosen dram-size is {args.dram_size:#x}')
        report.check(text(chosen, 'crypto-hash-method') == 'sha2-384', '/chosen crypto-hash-method is sha2-384')
        report.check(u32(chosen, 'amfi-only-platform-code') == 1, '/chosen keeps AMFI enforcing')

    defaults = child(root, 'defaults')
    report.check(defaults is not None and u32(defaults, 'sleep-disabled') == 1,
                 '/defaults sleep-disabled is 1')
    report.check(child(root, 'options') is not None, 'has /options')

    product = child(root, 'product')
    report.check(product is not None and text(product, 'model') == 'oneplus,cheeseburger',
                 '/product model is oneplus,cheeseburger')
    if args.identity == 'ipad7,1':
        report.check(child(root, 'buttons') is not None, '/buttons present for the iPad fixture')

    if report.errors:
        print(f'{len(report.errors)} checks failed')
        return 1
    print('ADT checks passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
