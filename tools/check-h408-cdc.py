#!/usr/bin/env python3
"""Validate the H408 CDC ACM descriptors and host protocol contract."""

import argparse
import json
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
DRIVER = ROOT / "drivers/CheeseburgerUsb/CheeseburgerUsb.cpp"
RECEIVER = ROOT / "tools/capture-h408-cdc.ps1"
MAGIC = b"UXL1"
HEADER_BYTES = 32
MAX_PAYLOAD = 1984
CMD_MAGIC = b"UXC1"
CMD_BYTES = 16
CMD_VERSION = 1
CMD_REBOOT = 1
BUILD = b"H410"


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


def parse_stream(data):
    summary = {
        "frames": 0,
        "payload_bytes": 0,
        "boots": [],
        "boot_switches": 0,
        "crc_errors": 0,
        "sequence_gaps": 0,
        "truncated_frames": 0,
        "first_sequence": None,
        "last_sequence": None,
    }
    index = 0
    connected_boot = None
    expected_sequence = None
    while index + len(MAGIC) <= len(data):
        if data[index:index + len(MAGIC)] != MAGIC:
            index += 1
            continue
        if index + HEADER_BYTES > len(data):
            summary["truncated_frames"] += 1
            break
        header = data[index:index + HEADER_BYTES]
        if header[4] != 1 or header[5] != HEADER_BYTES:
            index += 1
            continue
        length = int.from_bytes(header[24:26], "little")
        if length > MAX_PAYLOAD:
            index += 1
            continue
        end = index + HEADER_BYTES + length
        if end > len(data):
            summary["truncated_frames"] += 1
            break
        payload = data[index + HEADER_BYTES:end]
        if crc32(payload) != int.from_bytes(header[28:32], "little"):
            summary["crc_errors"] += 1
            index += 1
            continue
        build = header[20:24]
        if build != BUILD:
            raise AssertionError(
                f"unexpected build ID {build!r} at byte offset {index}"
            )
        boot = int.from_bytes(header[8:12], "little")
        sequence = int.from_bytes(header[12:16], "little")
        if connected_boot is None:
            connected_boot = boot
            summary["boots"].append(f"{boot:08X}")
        elif connected_boot != boot:
            connected_boot = boot
            summary["boot_switches"] += 1
            summary["boots"].append(f"{boot:08X}")
            expected_sequence = None
        if expected_sequence is not None and sequence != expected_sequence:
            summary["sequence_gaps"] += 1
        if summary["first_sequence"] is None:
            summary["first_sequence"] = sequence
        summary["last_sequence"] = sequence
        expected_sequence = (sequence + 1) & 0xFFFFFFFF
        summary["frames"] += 1
        summary["payload_bytes"] += length
        index = end
    return summary


def frame(payload, sequence, boot, corrupt=False):
    header = bytearray(HEADER_BYTES)
    header[0:4] = MAGIC
    header[4] = 1
    header[5] = HEADER_BYTES
    header[8:12] = boot.to_bytes(4, "little")
    header[12:16] = sequence.to_bytes(4, "little")
    header[20:24] = BUILD
    header[24:26] = len(payload).to_bytes(2, "little")
    header[28:32] = crc32(payload).to_bytes(4, "little")
    if corrupt:
        payload = bytes((payload[0] ^ 0xFF,)) + payload[1:]
    return bytes(header) + payload


def command_frame(opcode=CMD_REBOOT, request=0x12345678, payload_len=0,
                  corrupt=False, version=CMD_VERSION, magic=CMD_MAGIC):
    body = bytearray(16)
    body[0:4] = magic
    body[4] = version
    body[5] = opcode
    body[6:8] = payload_len.to_bytes(2, "little")
    body[8:12] = request.to_bytes(4, "little")
    crc = crc32(bytes(body[0:12]))
    if corrupt:
        crc ^= 0xFFFFFFFF
    body[12:16] = crc.to_bytes(4, "little")
    return bytes(body)


def decode_command(frame):
    if len(frame) != CMD_BYTES:
        return None
    if frame[0:4] != CMD_MAGIC or frame[4] != CMD_VERSION:
        return None
    opcode = frame[5]
    payload_len = int.from_bytes(frame[6:8], "little")
    request = int.from_bytes(frame[8:12], "little")
    want = int.from_bytes(frame[12:16], "little")
    if payload_len != 0 or request == 0:
        return None
    if crc32(bytes(frame[0:12])) != want:
        return None
    return (opcode, request)


def command_self_test():
    assert decode_command(command_frame()) == (CMD_REBOOT, 0x12345678)
    assert decode_command(command_frame(corrupt=True)) is None
    assert decode_command(command_frame(magic=b"UXC2")) is None
    assert decode_command(command_frame(version=2)) is None
    assert decode_command(command_frame(payload_len=1)) is None
    assert decode_command(command_frame(request=0)) is None
    assert decode_command(command_frame()[:-1]) is None


def edge_fixture():
    data = b"transport-noise"
    data += frame(b"old-one", 0xFFFFFFFE, 0x12345678)
    data += frame(b"old-two", 0xFFFFFFFF, 0x12345678)
    data += frame(b"old-wrap", 0, 0x12345678)
    data += frame(b"crc-bad", 1, 0x12345678, corrupt=True)
    data += b"\x00\xff"
    data += frame(b"new-boot", 0, 0x87654321)
    data += MAGIC + b"\x01\x20"
    return data


def replay_self_test():
    valid = frame(b"first", 10, 0x12345678) + frame(b"second", 11, 0x12345678)
    result = parse_stream(valid)
    assert result["frames"] == 2 and result["payload_bytes"] == 11

    resynchronized = b"noise" + frame(b"one", 40, 0x12345678)
    resynchronized += b"\x00\xff" + frame(b"two", 41, 0x12345678)
    result = parse_stream(resynchronized)
    assert result["frames"] == 2 and result["crc_errors"] == 0

    result = parse_stream(edge_fixture())
    assert result["frames"] == 4
    assert result["crc_errors"] == 1
    assert result["sequence_gaps"] == 0
    assert result["boot_switches"] == 1
    assert result["truncated_frames"] == 1


def validate_contract():
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
        "CreateFile",
        "SetCommTimeouts",
        "ReadFile",
        "No valid UXL1 frames received",
        "Sequence gap",
        "CRC mismatch",
        'if ($build -ne "H408")',
    ):
        assert required in receiver, required
    source = DRIVER.read_text()
    for required in (
        "constexpr uint32_t kCommandBytes = 16;",
        "constexpr uint32_t kCommandOpReboot = 1;",
        "parseCommand",
        "native_usb_reboot_requested",
        "native_usb_status_started",
        "native_usb_status_connected",
        "native_usb_status_active",
        "halt_all_cpus(TRUE)",
        "usbCommandWorkerThread",
    ):
        assert required in source, required

    console = ROOT / "tools/h410-cdc-console.ps1"
    assert console.exists(), console
    text = console.read_text()
    for required in (
        "UXC1",
        "[switch]$Reboot",
        "WriteFile",
        "EscapeCommFunction",
        "CRC mismatch",
        "sequence gap",
        "H410 frames=",
    ):
        assert required in text, required

    print("H410 CDC descriptors, endpoint map, CRC, UXC1 command and receiver contract: PASS")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--replay",
        action="append",
        metavar="PATH",
        help="validate a captured H408 wire stream (may be repeated)",
    )
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument(
        "--write-edge-fixture",
        metavar="PATH",
        help="write a deterministic replay fixture covering all recovery cases",
    )
    args = parser.parse_args()

    validate_contract()
    if args.write_edge_fixture:
        path = Path(args.write_edge_fixture)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(edge_fixture())
        print(f"H408 edge replay fixture: {path}")
    if args.self_test:
        replay_self_test()
        command_self_test()
        print("H410 receiver replay and UXC1 command edge cases: PASS")
    for path in args.replay or ():
        result = parse_stream(Path(path).read_bytes())
        print(f"{path}: {json.dumps(result, sort_keys=True)}")
        failures = []
        if result["frames"] == 0:
            failures.append("no valid frames")
        if result["crc_errors"]:
            failures.append(f"{result['crc_errors']} CRC errors")
        if result["sequence_gaps"]:
            failures.append(f"{result['sequence_gaps']} sequence gaps")
        if result["boot_switches"]:
            failures.append(f"{result['boot_switches']} boot switches")
        if result["truncated_frames"]:
            failures.append(f"{result['truncated_frames']} truncated frames")
        if failures:
            raise AssertionError(f"{path}: {', '.join(failures)}")
    if args.replay:
        print("H410 captured-stream replay: PASS")


if __name__ == "__main__":
    try:
        main()
    except AssertionError as error:
        sys.exit(f"check-h408-cdc: {error}")
