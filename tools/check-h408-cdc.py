#!/usr/bin/env python3
"""Validate the H408 CDC ACM descriptors and host protocol contract."""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
DRIVER = ROOT / "drivers/CheeseburgerUsb/CheeseburgerUsb.cpp"
RECEIVER = ROOT / "tools/capture-h408-cdc.ps1"


def array(name):
    source = DRIVER.read_text()
    match = re.search(
        rf"constexpr uint8_t\s+{re.escape(name)}\[\]\s*=\s*\{{(.*?)\}};",
        source,
        re.S,
    )
    if not match:
        raise AssertionError(f"descriptor {name} not found")
    body = re.sub(r"/\*.*?\*/", "", match.group(1), flags=re.S)
    body = re.sub(r"//.*", "", body)
    return bytes(int(token, 0) for token in re.findall(
        r"0x[0-9a-fA-F]+|\b\d+\b", body
    ))


def crc32(data):
    value = 0xffffffff
    for byte in data:
        value ^= byte
        for _ in range(8):
            value = (value >> 1) ^ (0xedb88320 if value & 1 else 0)
    return (~value) & 0xffffffff


def main():
    device = array("kDeviceDescriptor")
    config = array("kConfigDescriptor")
    qualifier = array("kQualifierDescriptor")
    bos = array("kBosDescriptor")

    assert len(device) == 18 and device[1] == 1
    assert device[4:7] == bytes((0x02, 0x02, 0x01))
    assert len(config) == 67
    assert int.from_bytes(config[2:4], "little") == len(config)
    assert config[4] == 2
    assert len(qualifier) == 10 and qualifier[4:7] == bytes((2, 2, 1))
    assert bos == bytes((5, 15, 5, 0, 0))

    interfaces = []
    endpoints = []
    cdc_types = []
    offset = 0
    while offset < len(config):
        length = config[offset]
        assert 2 <= length <= len(config) - offset
        kind = config[offset + 1]
        if kind == 4:
            interfaces.append({
                "number": config[offset + 2],
                "endpoints": config[offset + 4],
                "class": config[offset + 5],
                "subclass": config[offset + 6],
                "protocol": config[offset + 7],
            })
        elif kind == 5:
            endpoints.append({
                "address": config[offset + 2],
                "attributes": config[offset + 3],
                "mps": int.from_bytes(config[offset + 4:offset + 6], "little"),
                "interval": config[offset + 6],
            })
        elif kind == 0x24:
            cdc_types.append(config[offset + 2])
        offset += length
    assert offset == len(config)
    assert interfaces == [
        {"number": 0, "endpoints": 1, "class": 2, "subclass": 2, "protocol": 1},
        {"number": 1, "endpoints": 2, "class": 0x0A, "subclass": 0, "protocol": 0},
    ]
    assert endpoints == [
        {"address": 0x82, "attributes": 3, "mps": 16, "interval": 9},
        {"address": 0x01, "attributes": 2, "mps": 512, "interval": 0},
        {"address": 0x81, "attributes": 2, "mps": 512, "interval": 0},
    ]
    assert cdc_types == [0, 1, 2, 6]
    assert crc32(b"123456789") == 0xCBF43926

    receiver = RECEIVER.read_text()
    for required in (
        "'^USB\\\\VID_1209&PID_0001'",
        "$serial.DtrEnable = $true",
        "Sequence gap",
        "CRC mismatch",
        'if ($build -ne "H408")',
    ):
        assert required in receiver, required
    print("H408 CDC descriptors, endpoint map, CRC and receiver contract: PASS")


if __name__ == "__main__":
    try:
        main()
    except AssertionError as error:
        sys.exit(f"check-h408-cdc: {error}")
