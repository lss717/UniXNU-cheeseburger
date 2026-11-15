# Format 2 migration record

Date: 2026-11-15

This directory records the static migration from the fork-specific format 1
device file to the official format 2 profile architecture. It does not record a
new hardware boot.

## Baseline revisions

| Repository | Revision | State |
| --- | --- | --- |
| `UniXNU-cheeseburger` | `0c66d9998a91da526a2de1bad0e58d4f1847c88f` | Starting device revision |
| Official `UniXNU/UniXNU` reference | `578ae75c8e7356d4d562bcfc294ab26dfe640a31` | Resolver baseline |
| `lss717/UniXNU` fork | `d04fad7c2ed4e8d35743142d3f89d37222a2c13b` | Existing working UniXNU tree |
| `UniXNU-msm8998-common` | `24fcb916e359f1fc207cc09b24bfc8c2901af827` | New shared skeleton |
| `edk2-msm8998` | Not available | Workspace checkout has no Git metadata |

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

The UFS probe needs `p8,msm8998-direct-dma`. The official converter at the
selected revision drops that property. A reviewed compatibility patch is kept
at `patches/unixnu/0001-devicetree-export-msm8998-ufs-profile.patch`; the
package script applies it and verification confirmed the property survives as
a little-endian u32 value of 1.

The public `zzz-boot-progress.patch` also carried a cheeseburger/sheng PMGR
hunk whose source patch was not in the public series. A second compatibility
patch removes that ownership leak. The device series then applies the PMGR
source patch followed by a device progress-marker patch. The resulting patch
order contains 61 unique entries.

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
- `build-native-ufs-runtime.py` in the archive plan;
- `CONFIG_XNU_NATIVE_UFS` not set.

The A/B/C/D UFS matrix from the migration plan remains required before choosing
the formal root storage implementation.

## Next verification

1. Run `unixnu status --json` against the official resolver and save it beside
   this file.
2. Compile `dts/cheeseburger.dts` with the pinned MSM8998 include directory.
3. Generate and compare format 1 and format 2 ADTs node by node.
4. Run `porting-record.py check`.
5. Record the first hardware checkpoint and failure; the host-side macOS build
   is complete.

The MSM8998 common revision
`24fcb916e359f1fc207cc09b24bfc8c2901af827` was published to
`https://github.com/lss717/UniXNU-msm8998-common` on 2026-11-15. A clean
checkout and CI can resolve the SoC source from `feeds.toml`.

The first successful format 2 macOS build produced a kernel and payload from
device commit `4ae1deb523022a4c5cf27cb07518829339b7457e`. The final packaging
run also produced the CLANG38 FD and boot image from
`lss717/edk2-msm8998` commit
`da889248ba63e3b76003e016ee8744b34aa91a01`. Commands, inputs and output hashes
are recorded in `host-build-2026-11-15.md`.
