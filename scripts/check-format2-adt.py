#!/usr/bin/env python3
"""Check the format 2 MSM8998 ADT against the cheeseburger hardware inputs."""

import argparse
import hashlib
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]


class Report:
    def __init__(self):
        self.errors = []

    def check(self, condition, message):
        if condition:
            print(f"  ok    {message}")
        else:
            self.errors.append(message)
            print(f"  FAIL  {message}")


def parse_node(data, offset):
    props, children = struct.unpack_from("<II", data, offset)
    offset += 8
    node = {"props": {}, "children": []}
    for _ in range(props):
        key = data[offset:offset + 32].split(b"\0")[0].decode("ascii", "replace")
        offset += 32
        length, = struct.unpack_from("<I", data, offset)
        offset += 4
        node["props"][key] = data[offset:offset + length]
        offset += (length + 3) & ~3
    for _ in range(children):
        child, offset = parse_node(data, offset)
        node["children"].append(child)
    name = node["props"].get("name", b"")
    node["name"] = name.split(b"\0")[0].decode("ascii", "replace")
    return node, offset


def child(node, name):
    return next((item for item in node["children"] if item["name"] == name), None)


def u32(node, key):
    value = node["props"].get(key)
    if value is None or len(value) != 4:
        return None
    return struct.unpack("<I", value)[0]


def u64(node, key):
    value = node["props"].get(key)
    if value is None or len(value) != 8:
        return None
    return struct.unpack("<Q", value)[0]


def text(node, key):
    value = node["props"].get(key)
    return None if value is None else value.split(b"\0")[0].decode("ascii", "replace")


def strings(node, key):
    value = node["props"].get(key)
    return None if value is None else value.rstrip(b"\0").decode("ascii").split("\0")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--adt", type=Path, required=True)
    parser.add_argument("--require", action="append", default=None,
                        help="required /chosen/memory-map sidecar placeholder")
    parser.add_argument("--system-volume",
                        type=Path, default=ROOT / "scripts/data/SystemVolume.im4p")
    args = parser.parse_args()

    data = args.adt.read_bytes()
    root, end = parse_node(data, 0)
    report = Report()
    report.check(end == len(data), f"whole ADT consumed ({end} of {len(data)} bytes)")
    report.check(root["name"] == "device-tree", "root is device-tree")
    report.check(text(root, "model") == "iPad7,1", "root model is iPad7,1")
    compatible = strings(root, "compatible") or []
    report.check(compatible == ["J120AP", "oneplus,cheeseburger", "qcom,msm8998"],
                 "root compatible order preserves OS, board and SoC identity")
    report.check(text(root, "unixnu,board-id") == "oneplus,cheeseburger",
                 "root board marker is oneplus,cheeseburger")

    cpus = child(root, "cpus")
    report.check(cpus is not None, "has /cpus")
    if cpus:
        regs = [u32(cpu, "reg") for cpu in cpus["children"]]
        report.check(regs == [0, 1, 2, 3, 0x100, 0x101, 0x102, 0x103],
                     "CPU MPIDR set matches MSM8998")
        report.check(all(u32(cpu, "timebase-frequency") == 19200000 for cpu in cpus["children"]),
                     "CPU timebase is 19200000")
        kinds = [u32(cpu, "cluster-type") for cpu in cpus["children"]]
        report.check(kinds == [ord("P")] * 4 + [ord("E")] * 4,
                     "CPU P/E cluster tags match the verified 4+4 topology")
        architectures = [text(cpu, "compatible") for cpu in cpus["children"]]
        report.check(architectures == ["qcom,kryo280"] * 8,
                     "CPU compatible strings describe MSM8998 Kryo 280")

    psci = child(root, "psci")
    report.check(psci is not None and text(psci, "method") == "smc",
                 "has PSCI with method smc")

    armio = child(root, "arm-io")
    report.check(armio is not None, "has /arm-io")
    base = size = 0
    if armio:
        ranges = armio["props"].get("ranges", b"")
        report.check(len(ranges) == 24, "/arm-io has a 64-bit ranges tuple")
        if len(ranges) == 24:
            _, base, size = struct.unpack("<QQQ", ranges)
            report.check(base == 0x01000000 and size == 0x17000000,
                         "arm-io covers UFS through GIC at 0x01000000+0x17000000")

        intc = child(armio, "intc")
        report.check(intc is not None, "has /arm-io/intc")
        if intc and len(intc["props"].get("reg", b"")) == 32:
            reg = struct.unpack("<4Q", intc["props"]["reg"])
            report.check(reg == (0x17A00000 - base, 0x10000,
                                 0x17B00000 - base, 0x100000),
                         "GICD/GICR offsets translate to the MSM8998 physical ranges")
            report.check(u64(intc, "redistributor-stride") == 0x20000,
                         "GIC redistributor stride is 0x20000")

        report.check(child(armio, "timer") is not None,
                     "exports the ARM generic timer")

        ufshc = child(armio, "ufshc")
        report.check(ufshc is not None, "has /arm-io/ufshc")
        if ufshc:
            report.check((strings(ufshc, "compatible") or []) ==
                         ["qcom,msm8998-ufshc", "qcom,ufshc", "jedec,ufs-2.0"],
                         "UFS compatible set is preserved")
            reg = ufshc["props"].get("reg", b"")
            report.check(len(reg) == 16 and
                         struct.unpack("<QQ", reg) == (0x1DA4000 - base, 0x2500),
                         "UFS host controller register window is preserved")
            report.check(u32(ufshc, "p8,msm8998-direct-dma") == 1,
                         "UFS probe direct-DMA profile is exported")
            report.check(u64(ufshc, "p8,hc-physical") == 0x1DA4000,
                         "UFS physical controller address is exported")
            report.check(u32(ufshc, "p8,spi-intid") == 297,
                         "UFS SPI interrupt identifier is preserved")
            report.check("p8,read-only" in ufshc["props"] and
                         "p8,warm-link-only" in ufshc["props"],
                         "UFS probe policy flags are exported")
            report.check(u32(ufshc, "lanes-per-direction") == 2,
                         "UFS has two lanes per direction")
            report.check(strings(ufshc, "phy-names") == ["ufsphy"],
                         "UFS PHY reference name is exported")

        ufsphy = child(armio, "ufsphy")
        report.check(ufsphy is not None and u32(ufsphy, "#phy-cells") == 0,
                     "UFS PHY node and phandle contract are exported")

    chosen = child(root, "chosen")
    report.check(chosen is not None, "has /chosen")
    if chosen:
        report.check(u64(chosen, "dram-base") == 0x80000000,
                     "DRAM base is 0x80000000")
        report.check(u64(chosen, "dram-size") == 0x200000000,
                     "DRAM size is 8 GiB")
        report.check(text(chosen, "crypto-hash-method") == "sha2-384",
                     "crypto hash method is sha2-384")
        report.check(u32(chosen, "amfi-only-platform-code") == 1,
                     "AMFI platform-code policy is present")
        report.check(u32(chosen, "debug-enabled") == 1,
                     "diagnostic debug-enabled marker is explicit")
        report.check(len(chosen["props"].get("boot-manifest-hash", b"")) == 48,
                     "boot manifest hash placeholder is 48 bytes")
        report.check(chosen["props"].get("mac-address-wifi0") == bytes.fromhex("020000000001"),
                     "Wi-Fi local address placeholder is preserved")
        report.check(chosen["props"].get("mac-address-bluetooth0") ==
                     bytes.fromhex("020000000002"),
                     "Bluetooth local address placeholder is preserved")
        volume = chosen["props"].get("system-volume-auth-blob", b"")
        expected = args.system_volume.read_bytes()
        report.check(volume == expected, "SystemVolume auth blob matches the pinned input")
        report.check(len(volume) == 229 and
                     hashlib.sha256(volume).hexdigest() ==
                     "559decd34e5c73119b55706ee546016a06ee8faad3c2364a796c728efe7c6192",
                     "SystemVolume auth blob hash is preserved")
        memory_map = child(chosen, "memory-map")
        report.check(memory_map is not None, "has /chosen/memory-map")
        if memory_map:
            names = set(memory_map["props"]) - {"name"}
            required = args.require or ["PackedRAMDisk", "TrustCache"]
            report.check(set(required) <= names,
                         f"payload sidecar placeholders are present: {', '.join(required)}")

    defaults = child(root, "defaults")
    report.check(defaults is not None and u32(defaults, "sleep-disabled") == 1,
                 "/defaults keeps sleep disabled")
    report.check(child(root, "options") is not None, "has /options")

    product = child(root, "product")
    report.check(product is not None and text(product, "model") == "oneplus,cheeseburger",
                 "/product model is oneplus,cheeseburger")
    report.check(product is not None and u32(product, "artwork-device-subtype") == 1920,
                 "/product display subtype preserves the 1080x1920 panel")
    buttons = child(root, "buttons")
    report.check(buttons is not None and strings(buttons, "button-names") ==
                 ["hold", "voldown", "volup"],
                 "/buttons preserves the no-Home-button route")
    report.check(buttons is not None and u32(buttons, "home-button-type") == 2,
                 "/buttons home-button-type is 2")

    if report.errors:
        print(f"{len(report.errors)} checks failed")
        return 1
    print("format 2 ADT checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
