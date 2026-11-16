# USB integration on latest device main

Date: 2026-11-16

## Purpose

Carry the verified DWC3 device-mode and EP0 fixes onto the latest format 2
device `main` without replacing newer board configuration with the old format 1
file.

## Baselines

| Item | Revision |
| --- | --- |
| Device `origin/main` | `5d65005a6a4ffe49e6f234c2a57619dd43bc1049` |
| Old USB development base | `0c66d9998a91da526a2de1bad0e58d4f1847c88f` |
| Integration branch | `usb-main-integration-5d65` |

The old USB branch has three commits on top of the old device base, plus one
later completion-status fix. The latest device main uses format 2 but still
contains the same pre-fix `drivers/CheeseburgerUsb/CheeseburgerUsb.cpp`.

## Cherry-pick audit

| Source commit | Integration commit | Result |
| --- | --- | --- |
| `50dbb2177dd01a09f8b2d5986e80b172c9cdcfe7` | `310492c1476a90823f286312c1efd895508089b4` | Applied cleanly |
| `413c7e6f6b3c014c48ceb0c0ef4ac80e83f9b337` | none | Already integrated semantically; skipped |
| `ca6e41706f005f1110545dd93fa0999b78510440` | `1b5d60e20e27193558bd74e2938cfc34c9141fd4` | Applied cleanly |
| `6884c7854eae8b039f2242846136c9de0120c4af` | `d4ff6292c9d84ed45e4522ea0a54182e68e95187` | Applied cleanly |

### Conflict and resolution

`413c7e6` modifies the format 1 `archives` list at the position where
`build-native-ufs-runtime.py` was present. Latest device main conflicts because
the archive architecture has already moved to format 2:

```text
component:RamBlock
bind-native-ufs-storage.py ... --class NativeRamBlock
```

The outdated builder is already absent, so the semantic result of `413c7e6`
is present in the newer format 2 list. The conflict was resolved by retaining
the latest main archive list. The old format 1 driver builder entries were not
restored.

## Additional validation repair

`scripts/porting-record.py check` failed on latest main because five device
evidence entries used abbreviated revisions. Commit
`532c2e16965d7b22769fc6b0a9bccccde87a7852` expands those values to full
40-character commits without changing their recorded findings.

## Static result

Relative to device `origin/main`, the USB code change is limited to:

```text
drivers/CheeseburgerUsb/CheeseburgerUsb.cpp
16 insertions(+), 4 deletions(-)
```

The selected changes are:

- DWC3 `GCTL.PrtCapDir = 2` for device mode;
- corrected `DEPCMDPAR0/PAR2` offsets;
- only `DEPEVT_STATUS_BUSERR` is treated as a transfer failure, so normal
  XferComplete status bits such as `IOC|LST` remain successful.

`porting-record.py check --device .` reports 13 structured areas checked.
`unixnu defconfig` and `unixnu status --json` resolve schema 2 with seven
selected components, 23 modules and the format 2 MSM8998 profile.

Package and documentation workflows remain `workflow_dispatch` only at this
baseline. No automatic package run is triggered by pushing this feature
branch.
