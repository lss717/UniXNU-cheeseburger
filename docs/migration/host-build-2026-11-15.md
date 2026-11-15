# Host build record

Date: 2026-11-15

## Source revisions

| Item | Revision |
| --- | --- |
| Device repository | `10c7d82582b0d75bb31a2098012a738f270b6ab1` |
| Official UniXNU base | `578ae75c8e7356d4d562bcfc294ab26dfe640a31` |
| Downstream UniXNU | `d7fafc5d45a795a3b7fc9e9ae313e704c5a1938e` |
| DarwinBoot | `2ba9cad556f1794fb74ac8af164bc34b0e096338` |
| MSM8998 common | `8227828eb1706b51aea9762bd46006328d91db26` |
| lss717/edk2-msm8998 | `da889248ba63e3b76003e016ee8744b34aa91a01` |

The final packaging runs consumed the fixed downstream checkout directly,
verified its official merge base and kept the UniXNU checkout clean before and
after the build. No `git apply` or compatibility-patch worktree was used.

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
UNIXNU_UNIQUE_CHIP_ID=12345678 \
UNIXNU_TRUST_CACHE=~/h103/kernel.macho.trustcache \
UNIXNU_DATA_RAMDISK=~/UniXNU/out/cheeseburger/payload/data-skip-setup-9d654a549840.dbr \
UNIXNU_SKIP_SETUP=1 \
bash scripts/package.sh --os 17 --uefi ../edk2-msm8998
```

## Results

| Output | Size | SHA-256 |
| --- | ---: | --- |
| `kernel/kernel.macho` | 52,043,776 | `147eb7f4c9345c06f762f64f5c1a7d67eb1506f32c687287571669a3521d3cd1` |
| `device-tree/hardware.dtb` | 3,507 | `3809aa23a82a409735b32af07a9cbe9118257a8b0ed03062994ac5b2931f755b` |
| `payload/payload.img` | 79,691,776 | `cc7cff7cf9a12033219d14c7b18abc0f8700b07ec2ac4a12afc860d00d6cb228` |
| Extracted `kernel.macho.adt` | 16,408 | `4a03da03e495e78f19f848be8ca66aeb0a8d07402bbcef1fc2cb056abf0acd40` |
| `MSM8998PKG_UEFI.fd` | 6,291,456 | `a68b961bf8e213f06560f7c6de024a06dfd513dbc85042ef099385f86f2bde36` |
| `boot/boot.img` | 81,274,880 | `5e5ee46039f4ad47a4d7b4a636fd4dc47b071490a90510f81712e66ca5386b09` |
| `package.manifest.json` | 9,552 | `c90726503b115327d4139a40579d4d8665b64d3c473f1b5349882d66917c30e5` |

The DTB is byte-identical between clean builds. Kernel, payload, FD, manifest
and boot image contain expected identity, debug or randomized handoff metadata.
After normalizing the enumerated Mach-O and build-time fields, two independent
kernel and FD rebuilds are byte-identical. See
`reproducibility-2026-11-15.md` for exact raw hashes and differing byte ranges.

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

The manifest records downstream commit
`d7fafc5d45a795a3b7fc9e9ae313e704c5a1938e`, official base and merge base
`578ae75c8e7356d4d562bcfc294ab26dfe640a31`, source-tree digest
`2f1ea33e2b842c2529fe0698955473b2178df918`, DarwinBoot
`2ba9cad556f1794fb74ac8af164bc34b0e096338`, MSM8998 common
`8227828eb1706b51aea9762bd46006328d91db26`, and edk2 revision
`da889248ba63e3b76003e016ee8744b34aa91a01`.

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
