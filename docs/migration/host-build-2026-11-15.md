# Host build record

Date: 2026-11-15

## Source revisions

| Item | Revision |
| --- | --- |
| Device repository | `4ae1deb523022a4c5cf27cb07518829339b7457e` |
| Final packaging revision | `e93cd11fd0915e9311fcdead3a3a67def891e9ee` |
| Official UniXNU | `578ae75c8e7356d4d562bcfc294ab26dfe640a31` |
| MSM8998 common | `24fcb916e359f1fc207cc09b24bfc8c2901af827` |
| lss717/edk2-msm8998 | `da889248ba63e3b76003e016ee8744b34aa91a01` |

The initial kernel and payload run started from a clean device checkout. The
final packaging run also exercised the reviewed UniXNU compatibility patches
and the external firmware path in `scripts/package.sh`.

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

The package build used standalone LLVM 20.1.8 from LLVM commit
`87f0227cb60147a26a1eeb4fb06e3b505e9c7261`. Clang and LLD were built against the
installed MacPorts LLVM 20.1.8 libraries and invoked from:

```text
/Users/lss/codex-format2-validation-20261115/build-clang20/bin/clang
/Users/lss/.local/lld-20/bin/lld-link
```

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
PATH=<llvm20-clang>:<llvm20-lld>:/opt/local/bin:$PATH \
  /opt/local/bin/python3.12 ./unixnu package -D ../../UniXNU-cheeseburger --target DEBUG
UNIXNU_UNIQUE_CHIP_ID=12345678 bash scripts/package.sh \
  --os 17 --uefi ../edk2-msm8998
```

## Results

| Output | Size | SHA-256 |
| --- | ---: | --- |
| `kernel/kernel.macho` | 52,043,776 | `2e03369c22a8ee47595e4b6ef159691a8af69faedae32c7a482896f48fa92c47` |
| `device-tree/hardware.dtb` | 3,507 | `d8c0ec4ac6f6e4b5f5c3f0daf4192bb971166fe5454e77e8fcaa3dabb94534d1` |
| `payload/payload.img` | 79,691,776 | `e3172be11b8793c6a78ad31ab33f0cd21c15dfd15155689cc85680a1e7934420` |
| Extracted `kernel.macho.adt` | 16,408 | `1da397ee4f4e74b752f46bb491540e2db8e9a2eb37b175c5444f7177313a2f1c` |
| `UniXNU.efi` | 95,232 | `5111229307666dde4e96c8e034704b09031e5f28e8f2820ef35dee7b925c07d4` |
| `UniXNUDxe.efi` | 14,336 | `5270e328bb0f656a51f1498588352599b9c6a9a1d77fb77950ef142ba23ca018` |
| `UniXNURamDiskDxe.efi` | 15,360 | `b5deaed49651171158ed02f1139835da8a0cce3bfd94095684bcb98529dcc148` |
| `MSM8998PKG_UEFI.fd` | 6,291,456 | `7ed7dd32709183b62745b8a895c42f13e8b68ea188ff67c29155123f12949eb7` |
| `boot/boot.img` | 81,274,880 | `5f2321ea0bcb4e5593fa120567b0a90426f8d80cf0522e0a5ae590171271a5c2` |
| `package.manifest.json` | 9,135 | `1c3961933e1063c99ef9d4cb99b84e4529840182b65cd6d5eb68fa026ce65f5a` |

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
9. UniXNULib needed the EmbeddedPkg GUID declaration and a legacy FdtLib shim.
10. The edk2 firmware sources needed a CLI-compatible AArch64 CLANG38/LLD path.
11. macOS case-insensitive includes and 4 KB section alignment were normalized.
12. `package.sh` now defers firmware to `lss717/edk2-msm8998` and writes the
    final boot image plus manifest.

## Package result

`unixnu package --target DEBUG` passed after switching the complete EDK II build
to matching LLVM 20 clang and LLD. The first attempt mixed Xcode clang 17 with
LLD 20 and failed while parsing LTO records with `lld-link: error: Invalid
record`.

EFI outputs:

```text
AARCH64-DEBUG/UniXNU.efi
AARCH64-DEBUG/UniXNUDxe.efi
AARCH64-DEBUG/UniXNURamDiskDxe.efi
AARCH64-DEBUG/HelloPayload.efi
AARCH64-DEBUG/SampleDriver.efi
AARCH64-DEBUG/XnuTest.efi
```

## Firmware and boot image

The independent `lss717/edk2-msm8998` checkout now builds with matched LLVM 20
CLANG38/LLD. The firmware passed FD/FV module checks, and the device script
packed the format 2 payload at the expected ramdisk address. The final boot
header, load ranges, file length and `OP5ESP` payload label all passed
`scripts/check-cheeseburger.py`.

The external-firmware release path builds kernel, package and payload while
leaving FD and boot-image assembly to the device script. The manifest covers
the final `boot.img`, `payload.img` and `kernel.macho`.

## Not verified

- Hardware boot, storage, display, input, GPU, or security path.
- Real PMGR and formal UFS runtime.
