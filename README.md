# UniXNU-cheeseburger

Format 2 device repository for the OnePlus 5 (cheeseburger, MSM8998).

The port uses the official UniXNU format 2 ARM64, MSM8998, iPadOS 17.7.11 and
device layers. The selected OS identity is iPad7,1 / iPadOS 17.7.11 (21H461).

## Status

| Area | State |
| --- | --- |
| Format 2 configuration and source roles | Migrated and host-parseable |
| MSM8998 common DTSI | Host DTS compile passed |
| Board DTS and common ADT path | DTS compile and full format 1/format 2 comparison passed |
| Component and patch resolution | Verified by the official resolver |
| Kernel and payload | Built on macOS; payload ADT checks passed |
| Package | Built on macOS with matched LLVM 20 clang/lld |
| Firmware and boot image | Built on macOS with CLANG38 + LLD; boot-image checks passed |
| Hardware boot checkpoint | Not re-qualified after the migration |
| UFS formal root path | Unresolved; A/B/C/D matrix pending |
| Release security chain | Not complete |

The format 1 source and resolver output are preserved under
`docs/migration/`. Historical device results remain format 1 evidence and are
not presented as format 2 results.

## Repositories

Use these checkouts side by side:

```text
UniXNU/                    lss717/UniXNU d7fafc5 (upstream base 578ae75c)
UniXNU-cheeseburger/       this repository
UniXNU-msm8998-common/     board-independent MSM8998 support at 8227828e
edk2-msm8998/              lss717/edk2-msm8998 firmware
```

`feeds.toml` pins the MSM8998 common revision. `local.toml` can point the same
source at a local checkout while it is being developed.

The common pin is published at
`https://github.com/lss717/UniXNU-msm8998-common`, so a clean checkout and the
GitHub workflow can resolve it without a local override.

The format 2 build uses the fixed
`lss717/UniXNU:upstream-format2-cheeseburger` commit
`d7fafc5d45a795a3b7fc9e9ae313e704c5a1938e`. The branch is based directly on
official `578ae75c8e7356d4d562bcfc294ab26dfe640a31`; its DarwinBoot submodule
is pinned to `2ba9cad556f1794fb74ac8af164bc34b0e096338`. The package script
verifies the commit, clean worktree and merge base, but never applies patches at
build time.

Official catalog components use the canonical-aware builder from UniXNU.
Device-local `NativeUfsProbe` and `CheeseburgerUsb` use
`scripts/build-cheeseburger-driver.py`, so the device repository no longer
shadows `scripts/build-arm-driver.py` during tree assembly. The local builder
reads those sources from `UNIXNU_DEVICE`; they intentionally do not appear as
catalog modules in `tree/drivers`.

## Resolve

From this repository, with the official reference checkout at
`../references/UniXNU`:

```sh
python3 ../references/UniXNU/unixnu defconfig cheeseburger_defconfig -D .
python3 ../references/UniXNU/unixnu status --json -D .
python3 ../references/UniXNU/unixnu tree -D .
python3 ../references/UniXNU/scripts/porting-record.py check --device .
```

The expected output root is:

```text
out/cheeseburger/ipados-17.7.11-21H461/cheeseburger/
```

## Build

The final build needs macOS and Xcode for the kernel, or the documented Linux
toolchain. The board package entry point is:

```sh
bash scripts/package.sh --check --os 17
UNIXNU_UNIQUE_CHIP_ID=<device-seed> \
UNIXNU_TRUST_CACHE=/path/to/kernel.macho.trustcache \
UNIXNU_DATA_RAMDISK=/path/to/data-seed.dbr \
UNIXNU_SKIP_SETUP=1 \
bash scripts/package.sh --os 17 --uefi ../edk2-msm8998
```

The packaging entry point builds kernel, payload and package first, then uses
the pinned `lss717/edk2-msm8998` checkout for the external firmware and boot
image. It consumes the fixed downstream checkout directly, requires it to remain
clean, and records the downstream commit, official merge base, source-tree
digest and edk2 revision in the manifest. The device tree selected by
`board.toml` is compiled by the common UniXNU payload path:

```text
dts/cheeseburger.dts + UniXNU-msm8998-common/dts/msm8998.dtsi
-> hardware.dtb
-> common ADT generator
-> kernel.macho.adt
```

Check a generated ADT with:

```sh
python3 scripts/check-format2-adt.py --adt path/to/kernel.macho.adt
```

## Diagnostic Builds

`board.toml` currently carries the format 1 diagnostic command line. It includes
root authentication, AMFI, execution-signature, dyld cache and debugger
bypasses. These arguments are retained only to avoid changing several variables
during the profile migration.

Do not describe an image built from this file as a release candidate. The
release security path still needs VerifiedRoot, trust-cache, AMFI and SystemOS
policy verification.

## Storage

The current UFS configuration is intentionally considered unresolved:

- `CONFIG_XNU_NATIVE_UFS_PROBE=y`;
- `native-ufs-probe=0` in the kernel arguments;
- `build-native-ufs-runtime.py` is not in the archive plan;
- `CONFIG_XNU_NATIVE_UFS` disabled.

The next storage task is the four single-path configurations in
`docs/NEXT-STEPS-FORMAT2-MIGRATION.md`, not another mixed-path boot.

## Inputs

Private IPSW members, trust caches, Data seeds and firmware binaries stay under
`.cache/firmware` and are never committed. `proprietary-files.txt` records the
source and known hashes. `local.toml.example` documents local source overrides.

The package workflow downloads `UNIXNU_TRUST_CACHE_URL` and
`UNIXNU_DATA_RAMDISK_URL` from secrets into the runner temp directory and checks
the Data seed against `UNIXNU_DATA_RAMDISK_SHA256`. It does not publish an image
when either private input is missing.

Detailed migration evidence and open work are in `docs/migration/README.md`.
The first macOS build command, input hashes and output hashes are recorded in
`docs/migration/host-build-2026-11-15.md`.
