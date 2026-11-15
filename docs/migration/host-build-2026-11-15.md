# Host build record

Date: 2026-11-15

## Source revisions

| Item | Revision |
| --- | --- |
| Device repository | `4ae1deb523022a4c5cf27cb07518829339b7457e` |
| Official UniXNU | `578ae75c8e7356d4d562bcfc294ab26dfe640a31` |
| MSM8998 common | `24fcb916e359f1fc207cc09b24bfc8c2901af827` |

The Mac checkout contained only the reviewed UniXNU compatibility patches
applied by `scripts/package.sh`. The device checkout itself was clean.

## Host

| Item | Value |
| --- | --- |
| OS | macOS 15.8.2, build 24H232 |
| Architecture | x86_64 |
| Xcode | 26.3, build 17C529 |
| Python | 3.12.16 from `/opt/local/bin/python3.12` |
| CMake | 3.31.12 |
| Ninja | 1.13.2 |
| dtc | 1.8.1 |

## Inputs

| Input | SHA-256 |
| --- | --- |
| Kernelcache | `f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7` |
| Static trust cache | `7416ca4f8377146626bb284e2133c76bee6cb4e3ee0af5d648b186235a7323a7` |
| Source Data seed | `d474b36616dca3227beb09243276e20818e32b11f55ad0cc2c3f1d5c0a656d41` |

The build used test platform seed `0x12345678`. Artifacts are not suitable for
installation on the phone.

## Commands

```sh
/opt/local/bin/python3.12 ./unixnu defconfig cheeseburger_defconfig -D ../../UniXNU-cheeseburger
/opt/local/bin/python3.12 ./unixnu tree -D ../../UniXNU-cheeseburger
/opt/local/bin/python3.12 ./unixnu kernel -D ../../UniXNU-cheeseburger \
  --kernelcache ~/UniXNU/out/ipsw/21H461/kernelcache.decompressed \
  --unique-chip-id 12345678
/opt/local/bin/python3.12 ./unixnu payload -D ../../UniXNU-cheeseburger \
  --kernelcache ~/UniXNU/out/ipsw/21H461/kernelcache.decompressed \
  --trust-cache ~/h103/kernel.macho.trustcache \
  --data-ramdisk ~/UniXNU/out/cheeseburger/payload/data-skip-setup-9d654a549840.dbr \
  --skip-setup --unique-chip-id 12345678
```

## Results

| Output | Size | SHA-256 |
| --- | ---: | --- |
| `kernel/kernel.macho` | 52,043,776 | `2e03369c22a8ee47595e4b6ef159691a8af69faedae32c7a482896f48fa92c47` |
| `device-tree/hardware.dtb` | 3,507 | `d8c0ec4ac6f6e4b5f5c3f0daf4192bb971166fe5454e77e8fcaa3dabb94534d1` |
| `payload/payload.img` | 79,691,776 | `e8c2d1aface25116c0e922633221400ce87d10128bb2c40551a78a4e8e3e91c2` |
| Extracted `kernel.macho.adt` | 16,408 | `1da397ee4f4e74b752f46bb491540e2db8e9a2eb37b175c5444f7177313a2f1c` |

The kernel hash was identical across two builds. Payload and ADT hashes change
between builds because the common generator includes a random seed.

Payload contents:

```text
EFI/UniXNU/Payloads/kernel.macho
EFI/UniXNU/Payloads/kernel.macho.adt
EFI/UniXNU/Payloads/kernel.macho.trustcache
EFI/UniXNU/Payloads/scratch-data.img
EFI/UniXNU/unixnu.toml
```

The extracted payload ADT passed `scripts/check-format2-adt.py`, including CPU
mapping, GIC, timer, UFS/UFS PHY, RAM, root UUID, SystemVolume blob, buttons,
and PackedRAMDisk/TrustCache placeholders.

## Build issues fixed during the run

1. The public boot-progress patch carried an undeclared PMGR dependency.
2. The device defconfig overrode the OS profile's prelinked crypto bootstrap.
3. The device `build-arm-driver.py` shadowed the canonical-aware official
   builder.
4. Xcode 26.3 promoted an unknown warning option in the official builder to an
   error.
5. Non-catalog device probes were expected under `tree/drivers`.
6. The signed-policy UFS runtime was present in a configuration that did not
   select `CONFIG_XNU_NATIVE_UFS`.
7. PMGR had a post-prelink builder in the pre-link archive list.
8. NativeRamBlock needed IOStorageFamily bindings and explicit UFS-off stubs.

## Remaining package blocker

`unixnu package --target DEBUG` currently stops with:

```text
unixnu: lld-link not found on PATH
```

The installed MacPorts LLVM 20 package provides `llvm-lib`, `llvm-rc`,
`llvm-nm`, and `llvm-objcopy`, but not `lld-link`. No boot image was produced.

## Not verified

- Package and firmware build.
- Boot image assembly.
- Hardware boot, storage, display, input, GPU, or security path.
- Real PMGR and formal UFS runtime.
