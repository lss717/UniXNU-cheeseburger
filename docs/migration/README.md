# Format 2 migration record

Date: 2026-11-15

This directory records the static migration from the fork-specific format 1
device file to the official format 2 profile architecture and the first
non-persistent hardware dispatch.

## Baseline revisions

| Repository | Revision | State |
| --- | --- | --- |
| `UniXNU-cheeseburger` | `0c66d9998a91da526a2de1bad0e58d4f1847c88f` | Starting device revision |
| Official `UniXNU/UniXNU` reference | `578ae75c8e7356d4d562bcfc294ab26dfe640a31` | Resolver baseline |
| `lss717/UniXNU:upstream-format2-cheeseburger` | `7d3f04dacfbffb343db936e24cb9af69877e078a` | Current downstream build input |
| `lss717/DarwinBoot:upstream-format2-cheeseburger` | `d90c9113836d61af3e90285d873cae9edd528d09` | Current submodule; 128 MiB window support based on official `5243c68e` |
| `UniXNU-msm8998-common` | `8227828eb1706b51aea9762bd46006328d91db26` | Current shared source pin |
| `lss717/edk2-msm8998` | `da889248ba63e3b76003e016ee8744b34aa91a01` | Firmware build input |

The format 1 resolver output is saved as `before-status.json`. It reports:

```text
schema_version: 1
components: []
patch_order: []
output: references/UniXNU/out/cheeseburger
```

The original board file is preserved verbatim as `board-format1.toml`.

| Preserved file | SHA-256 |
| --- | --- |
| `docs/migration/board-format1.toml` | `06B88D84CDED9EFBBBEC0793B86837DF17AB17D5C19C9B4B75A30EC6527955E1` |
| Original `configs/cheeseburger_defconfig` | `54255608CD76CA62A4473CE0A8C5E9439428BF30B875EDEAD37BF51D690A3F48` |
| `scripts/data/SystemVolume.im4p` | `559DECD34E5C73119B55706EE546016A06EE8FAAD3C2364A796C728EFE7C6192` |

The original defconfig is intentionally unchanged in this migration.

## Existing device evidence

The UniXNU working tree records the following H91 artifacts. They were not
present in this checkout on 2026-11-15 and were not rehashed during this
migration:

| Artifact | Historical SHA-256 |
| --- | --- |
| `boot-H91-stallpanic.img` | `cd81c5b8ec91f103f3e3c0fd7de7bfb26633f4fbabecd34490e5627dd6f3ff9b` |
| `payload-H91-stallpanic.img` | `e7fa5cdff58217886ebf269b30683c78da83c409ae179c356650f69869f08942` |
| Payload `kernel.macho` | `7f02891c32825b75fcb76f2969ea2c1a541662d4c369d29fc85732b19f0e532d` |
| Payload `kernel.macho.adt` | `83d733d770a9ec6961f60c1af13f4eeeb8552c776098241c0c536d13f810e54a` |

Existing checkpoints from the UniXNU plan and status record:

| Checkpoint | Existing evidence | Rechecked here |
| --- | --- | --- |
| UEFI and UniXNU menu | G1 device photos and status record | No |
| DarwinBoot handoff | G1 loader protocol and boot logs | No |
| XNU early boot and BSD/IOKit | B3 status entries | No |
| UFS controller and block publication | B5 NativeUfsProbe logs | No |
| APFS root match | B5 root UUID log | No |
| launchd exec | B5 userland handoff log | No |
| dyld shared cache | Known unresolved diagnostic | No |

These remain historical format 1 results. They are not evidence for the new
format 2 image.

The first format 2 hardware dispatch has separate evidence in
`device-run-2026-11-15-01.md`:

| Checkpoint | Format 2 result |
| --- | --- |
| BootShim / FD / UEFI | Reached |
| UniXNU graphical menu | Reached |
| `OnePlus 5 IOS` dispatch | Reached |
| DarwinBoot 4 GiB request | Failed `EFI_OUT_OF_RESOURCES`; longest run `0xFCCD0000` |
| DarwinBoot 128 MiB retry | Reached `Loader->Boot` without `NO WINDOW`; then UEFI synchronous exception at ELR `0x17ADBC378` |
| DarwinBoot handoff / XNU | Not proven |

## Migration changes

1. A shared `UniXNU-msm8998-common` skeleton now owns CPU topology, GIC, timer,
   UFS and USB resources.
2. `board.toml` uses schema 2 and selects ARM64, MSM8998, iPadOS 17.7.11 and the
   device profile.
3. The custom ADT generator is retained only as a format 1 reference. The new
   board path uses `dts/cheeseburger.dts` and the common ADT generator.
4. Component names use official catalog IDs; old runtime identifiers and
   source filenames remain unchanged.
5. The current diagnostic boot arguments and security bypasses are preserved,
   but the README now labels them explicitly.

The board DTS compiles with `dtc` 1.7.0. A generated format 2 ADT was checked
with `scripts/check-format2-adt.py`: 47 structural checks passed. With a
deterministic test seed, the format 1 ADT is 14,936 bytes
(`d2a6256b...`) and the format 2 ADT is 16,460 bytes (`0a5a9973...`).
See `adt-diff-2026-11-15.md` for the raw comparison and
`adt-diff-review-2026-11-15.md` for the ownership review.

The UFS probe needs `p8,msm8998-direct-dma`. The downstream UniXNU branch
exports it from the official device-tree converter. The branch also removes the
cheeseburger/sheng PMGR hunk from the public boot-progress patch, carries the
EDK II compatibility changes and provides the external-firmware release path.
Package builds no longer create a patched worktree or run `git apply`; the
device series applies the PMGR source patch followed by its progress marker,
producing 61 unique patch entries.

The generated format 2 ADT differs structurally from the last saved format 1
ADT:

| Item | Format 1 | Format 2 skeleton | Reason |
| --- | --- | --- | --- |
| ADT size | 14,604 bytes | 16,460 bytes | Common nodes and typed board properties |
| `arm-io` base | `0x17000000` | `0x01000000` | The exported UFS window lies below GIC |
| `arm-io` size | `0x08000000` | `0x17000000` | One window now spans UFS through GIC |
| GIC physical ranges | `0x17a00000`, `0x17b00000` | Same | Offsets were recomputed from the new window |
| UFS physical range | `0x1da4000` | Same | Offsets were recomputed from the new window |
| Timer/PHY nodes | Not exported | Exported | Common SoC ownership |

No measured physical address changed. Only the `arm-io` offset base and the set
of exported child nodes changed. The full comparison found no format 1 node
missing from format 2 and no unexplained critical property loss.

## Open storage conflict

The following state must not be reported as a single supported storage path:

- `CONFIG_XNU_NATIVE_UFS_PROBE=y` in the defconfig;
- `native-ufs-probe=0` on the command line;
- `build-native-ufs-runtime.py` is absent from the archive plan;
- `RamBlock` is bound as the current storage provider while the formal UFS
  provider remains unresolved;
- `CONFIG_XNU_NATIVE_UFS` not set.

The A/B/C/D UFS matrix from the migration plan remains required before choosing
the formal root storage implementation.

## Verification status

1. `docs/migration/clean-status.json` records a no-override resolver run with
   23 modules, 61 patches and no local source.
2. `dts/cheeseburger.dts` compiles with the pinned MSM8998 include directory.
3. The format 1/format 2 ADT comparison is recorded in the two ADT diff files.
4. `porting-record.py check` reports 13 structured areas checked.
5. The host-side macOS kernel, payload, EFI package, FD, boot image and ADT
   checks are complete. The first hardware dispatch reached the menu and
   DarwinBoot, then stopped at the 4 GiB contiguous-window allocation.

The MSM8998 common revision
`8227828eb1706b51aea9762bd46006328d91db26` is pinned by `feeds.toml` and
`unixnu.lock`. It changes all eight MSM8998 CPU compatible strings to
`qcom,kryo280` while preserving the 4+4 P/E cluster tags.

The downstream package build produced kernel, payload, CLANG38 FD and boot image
from the pinned UniXNU and edk2 revisions. Commands, inputs, raw hashes and the
normalized reproducibility result are recorded in
`host-build-2026-11-15.md` and `reproducibility-2026-11-15.md`.
