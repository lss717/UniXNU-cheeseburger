#!/usr/bin/env python3
"""Pre-flight checks for the OnePlus 5 (cheeseburger) B1 firmware.

Validates the pieces that can be checked without a device:

  * the firmware device file is exactly 0x600000 bytes and holds the
    expected FV at +0x8000, with the UniXNU boot stack inside;
  * the boot image header points the kernel, ramdisk and tags at the
    addresses the B1 plan uses, and nothing overlaps;
  * the payload is a FAT image labelled OP5ESP with the ESP layout
    EFI/UniXNU/unixnu.toml that UniXNU reads.

The build log's "FVMAIN [100%Full]" is expected: FVMAIN is declared with
NumBlocks = 0, so GenFv sizes it to its contents.  The real budget is the
6 MiB FD, and this script reports the free bytes inside its FV.

Usage:
    python3 plans/oneplus5-cheeseburger/tools/check-cheeseburger.py
    python3 ... --fd FILE --boot FILE --payload FILE
"""

import argparse
import os
import struct
import sys
import uuid

FD_SIZE = 0x600000
FD_FV_OFFSET = 0x8000
FD_FV_LENGTH = 0x5F8000

# (NAME, FILE_GUID, required)
MODULES = (
    ("UniXNU", "9B665204-A133-47A2-9396-9E2B370A4E25", True),
    ("UniXNUDxe", "29B950CF-586D-49EA-9D71-F88645CCDAF6", True),
    ("DarwinBoot", "1B64AF87-8415-4B57-9B16-680D70913B9F", True),
    ("CheeseburgerBootMemoryDxe", "5C0E4B7A-31D2-4E8F-9A66-0B1F7D2C8E44", True),
    ("SerialDxe", "9A5163E7-5C29-453F-825C-837A46A81E15", False),
)
LINUX_IMAGE_GUID = "2A24787E-E09C-43CE-B5CF-D03066F6092F"

BOOT_KERNEL_ADDR = 0x10000000
BOOT_RAMDISK_ADDR = 0x12000000
BOOT_TAGS_ADDR = 0x18000000


class Report:
    def __init__(self):
        self.errors = []
        self.warnings = []
        self.modules = {}

    def error(self, message):
        self.errors.append(message)

    def warn(self, message):
        self.warnings.append(message)

    def ok(self, message):
        print("  ok    %s" % message)

    def summary(self):
        for warning in self.warnings:
            print("  warn  %s" % warning)
        for error in self.errors:
            print("  FAIL  %s" % error)
        return 1 if self.errors else 0


def align(value, page):
    return (value + page - 1) // page * page


def parse_fv(report, fd):
    if len(fd) != FD_SIZE:
        report.error("FD is 0x%x bytes, expected 0x%x" % (len(fd), FD_SIZE))

    base = FD_FV_OFFSET
    if fd[base + 0x28:base + 0x2C] != b"_FVH":
        report.error("no _FVH signature at 0x%x" % (base + 0x28))
        return
    fv_length, = struct.unpack_from("<Q", fd, base + 0x20)
    header_length, = struct.unpack_from("<H", fd, base + 0x30)
    if fv_length != FD_FV_LENGTH:
        report.error("FV length 0x%x, expected 0x%x" % (fv_length, FD_FV_LENGTH))
    report.ok("FD 0x%x bytes, FV 0x%x at +0x%x" % (len(fd), fv_length, base))

    offset = base + header_length
    end = base + fv_length
    modules = {}
    while offset + 24 <= end:
        guid = str(uuid.UUID(bytes_le=fd[offset:offset + 16])).upper()
        ftype = fd[offset + 18]
        size = int.from_bytes(fd[offset + 20:offset + 23], "little")
        if size in (0, 0xFFFFFF) or offset + size > end:
            break
        # EFI_FV_FILETYPE_FFS_PAD is 0xF0 and carries no module
        if ftype != 0xF0:
            modules[guid.upper()] = size
        offset += align(size, 8)

    free = fv_length - (offset - base)
    report.ok("FV free %d bytes (%.2f MiB)" % (free, free / 2 ** 20))

    report.modules = modules


def read_fv_modules(path):
    """Return {GUID: size} for an FV text dump or an uncompressed FV binary."""
    if path.endswith(".txt"):
        with open(path, "r") as handle:
            lines = handle.read().splitlines()
        entries = []
        for line in lines:
            fields = line.split()
            if len(fields) >= 2 and fields[0].startswith("0x"):
                entries.append((int(fields[0], 16), fields[1].upper()))
        total = entries[-1][0] if entries else 0
        for line in lines:
            if "TOTAL_SIZE" in line:
                total = int(line.split("=")[1], 16)
        modules = {}
        for index, (offset, guid) in enumerate(entries):
            end = entries[index + 1][0] if index + 1 < len(entries) else total
            modules[guid] = end - offset
        return modules
    with open(path, "rb") as handle:
        data = handle.read()
    if data[0x28:0x2C] == b"_FVH":
        base = 0
    elif data[FD_FV_OFFSET + 0x28:FD_FV_OFFSET + 0x2C] == b"_FVH":
        base = FD_FV_OFFSET
    else:
        raise ValueError("%s: no _FVH signature" % path)
    fv_length, = struct.unpack_from("<Q", data, base + 0x20)
    header_length, = struct.unpack_from("<H", data, base + 0x30)
    offset, end, modules = base + header_length, base + fv_length, {}
    while offset + 24 <= end:
        guid = str(uuid.UUID(bytes_le=data[offset:offset + 16])).upper()
        ftype = data[offset + 18]
        size = int.from_bytes(data[offset + 20:offset + 23], "little")
        if size in (0, 0xFFFFFF) or offset + size > end:
            break
        if ftype != 0xF0:
            modules[guid] = size
        offset += align(size, 8)
    return modules


def parse_fvmap(report, path):
    try:
        modules = read_fv_modules(path)
    except (OSError, ValueError) as error:
        report.error(str(error))
        return
    for name, guid, required in MODULES:
        if guid in modules:
            report.ok("%s present (%d bytes)" % (name, modules[guid]))
        elif required:
            report.error("%s missing from the firmware volume" % name)
        else:
            report.warn("%s missing: UniXNU logs will not reach the screen" % name)
    if LINUX_IMAGE_GUID in modules:
        report.warn("LinuxSimpleMassStorage still in the firmware; 6.9 MiB wasted")
    return modules

def parse_boot(report, path):
    with open(path, "rb") as handle:
        data = handle.read(0x800)
    if data[:8] != b"ANDROID!":
        report.error("%s: not an Android boot image" % path)
        return
    (kernel_size, kernel_addr, ramdisk_size, ramdisk_addr,
     second_size, second_addr, tags_addr, page_size,
     header_version) = struct.unpack_from("<9I", data, 8)

    if page_size < 2048 or page_size & (page_size - 1):
        report.error("page size %d is not a power of two >= 2048" % page_size)
        return
    report.ok("boot image: header v%d, page %d, kernel 0x%x+0x%x, ramdisk 0x%x+0x%x, tags 0x%x"
              % (header_version, page_size, kernel_addr, kernel_size,
                 ramdisk_addr, ramdisk_size, tags_addr))

    for label, size in (("kernel", kernel_size), ("ramdisk", ramdisk_size)):
        if size == 0:
            report.error("%s is empty" % label)
    if (kernel_addr, ramdisk_addr, tags_addr) != (
            BOOT_KERNEL_ADDR, BOOT_RAMDISK_ADDR, BOOT_TAGS_ADDR):
        report.warn("addresses differ from the B1 plan (0x%x/0x%x/0x%x)"
                    % (BOOT_KERNEL_ADDR, BOOT_RAMDISK_ADDR, BOOT_TAGS_ADDR))

    kernel_end = kernel_addr + align(kernel_size, page_size)
    ramdisk_end = ramdisk_addr + align(ramdisk_size, page_size)
    if kernel_end > ramdisk_addr:
        report.error("kernel [0x%x,0x%x) overlaps ramdisk at 0x%x"
                     % (kernel_addr, kernel_end, ramdisk_addr))
    if ramdisk_end > tags_addr:
        report.error("ramdisk [0x%x,0x%x) overlaps tags at 0x%x"
                     % (ramdisk_addr, ramdisk_end, tags_addr))
    if kernel_end <= ramdisk_addr and ramdisk_end <= tags_addr:
        report.ok("load addresses do not overlap")

    expected = page_size + align(kernel_size, page_size) + align(ramdisk_size, page_size) \
        + align(second_size, page_size)
    actual = os.path.getsize(path)
    if actual < expected:
        report.error("%s is truncated: %d bytes, header needs %d" % (path, actual, expected))
    else:
        report.ok("file %d bytes, header needs %d" % (actual, expected))


def fat_entries(image, offset, count):
    for index in range(count):
        entry = image[offset + index * 32:offset + index * 32 + 32]
        if len(entry) < 32:
            return
        name = entry[:11]
        if name[:1] in (b"\x00", b"\xe5"):
            if name[:1] == b"\x00":
                return
            continue
        if entry[11] == 0x0F:  # long file name
            continue
        yield name, entry[11], entry


def fat_name(raw):
    base = raw[:8].decode("ascii", "replace").rstrip()
    ext = raw[8:11].decode("ascii", "replace").rstrip()
    return (base + "." + ext) if ext else base


def fat_read_dir(image, bpb, cluster):
    bps, spc, reserved, fats, root_entries, fat_size, first_data = bpb
    if cluster == 0:  # FAT12/16 fixed root directory
        offset = (reserved + fats * fat_size) * bps
        return list(fat_entries(image, offset, root_entries))
    offset = (first_data + (cluster - 2) * spc) * bps
    # one cluster is enough for the small payload tree
    return list(fat_entries(image, offset, spc * bps // 32))


def parse_payload(report, path):
    with open(path, "rb") as handle:
        image = handle.read()
    if len(image) < 512 or image[510:512] != b"\x55\xaa":
        report.error("%s: no FAT boot signature" % path)
        return
    bps, = struct.unpack_from("<H", image, 11)
    spc = image[13]
    reserved, = struct.unpack_from("<H", image, 14)
    fats = image[16]
    root_entries, = struct.unpack_from("<H", image, 17)
    fat16, = struct.unpack_from("<H", image, 22)
    fat32, = struct.unpack_from("<I", image, 36)
    root_cluster, = struct.unpack_from("<I", image, 44)

    if bps != 512:
        report.warn("payload sector size %d, expected 512" % bps)
    if root_entries == 0:
        root_cluster = root_cluster or 2
    fat_size = fat16 or fat32
    first_data = reserved + fats * fat_size + (root_entries * 32 + bps - 1) // bps
    bpb = (bps, spc, reserved, fats, root_entries, fat_size, first_data)

    label = image[43:54] if root_entries else image[71:82]
    label = label.decode("ascii", "replace").strip("\x00 ")
    if label != "OP5ESP":
        report.warn("payload label is %r, expected OP5ESP" % label)
    else:
        report.ok("payload label OP5ESP")

    root = fat_read_dir(image, bpb, 0 if root_entries else root_cluster)
    efi = [entry for name, attr, entry in root
           if attr & 0x10 and fat_name(name).upper() in ("EFI", "EFI~1")]
    if not efi:
        report.error("%s: no EFI directory in the root" % path)
        return
    efi_cluster = struct.unpack_from("<H", efi[0], 20)[0] << 16 | \
        struct.unpack_from("<H", efi[0], 26)[0]
    efi_dir = fat_read_dir(image, bpb, efi_cluster)
    unixnu = [entry for name, attr, entry in efi_dir
              if attr & 0x10 and fat_name(name).upper().startswith("UNIXNU")]
    if not unixnu:
        report.error("%s: no EFI/UniXNU directory" % path)
        return
    unixnu_cluster = struct.unpack_from("<H", unixnu[0], 20)[0] << 16 | \
        struct.unpack_from("<H", unixnu[0], 26)[0]
    files = [name for name, attr, _ in fat_read_dir(image, bpb, unixnu_cluster)
             if not attr & 0x10]
    if any(fat_name(name).upper().startswith("UNIXNU") and fat_name(name).upper().endswith(".TOM")
           for name in files):
        report.ok("payload has EFI/UniXNU/unixnu.toml")
    else:
        report.error("%s: EFI/UniXNU has no unixnu.toml (found %s)"
                     % (path, ", ".join(fat_name(n) for n in files) or "nothing"))


def find_edk2(explicit):
    candidates = []
    if explicit:
        candidates.append(explicit)
    if os.environ.get("EDK2_MSM8998"):
        candidates.append(os.environ["EDK2_MSM8998"])
    candidates.append(os.path.join(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))), "..", "edk2-msm8998"))
    candidates.append("/home/ls/edk2-msm8998")
    for candidate in candidates:
        candidate = os.path.abspath(candidate)
        if os.path.isfile(os.path.join(candidate, "mkbootimg.py")):
            return candidate
    return None


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    boot_dir = os.path.join(root, "out", "cheeseburger", "boot")
    edk2 = find_edk2(None)
    build = os.path.join(edk2, "workspace", "Build", "cheeseburger",
                         "RELEASE_GCC5", "FV") if edk2 else ""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--fd", default=os.path.join(build, "MSM8998PKG_UEFI.fd") if build else None)
    parser.add_argument("--boot", default=os.path.join(boot_dir, "boot.img"))
    parser.add_argument("--payload", default=os.path.join(boot_dir, "payload.img"))
    parser.add_argument("--fvmap", default=os.path.join(build, "FVMAIN.Fv.txt") if build else None,
                        help="uncompressed FVMAIN.Fv.txt/Fv from the build, to check modules")
    args = parser.parse_args()

    report = Report()
    for path in (args.fd, args.boot, args.payload):
        if not os.path.isfile(path):
            print("FAIL  missing %s" % path)
            return 1

    print("checking %s" % args.fd)
    with open(args.fd, "rb") as handle:
        parse_fv(report, handle.read())
    if args.fvmap:
        print("checking modules in %s" % args.fvmap)
        parse_fvmap(report, args.fvmap)
    else:
        report.warn("module contents not checked: the FD stores one compressed section; "
                    "pass --fvmap <FVMAIN.Fv.txt> from the build")
    print("checking %s" % args.boot)
    parse_boot(report, args.boot)
    print("checking %s" % args.payload)
    parse_payload(report, args.payload)

    return report.summary()


if __name__ == "__main__":
    sys.exit(main())
