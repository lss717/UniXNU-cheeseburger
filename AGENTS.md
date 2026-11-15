# Device agent guide

This repository owns the OnePlus 5 board description, DTS wiring, firmware
selection, partition geometry, package integration and device packaging.

## Read first

- `docs/migration/README.md`
- `docs/NEXT-STEPS-FORMAT2-MIGRATION.md`
- UniXNU device repository, configuration and device-tree guides
- MSM8998 common `README.md`, `AGENTS.md`, `soc.toml` and DTSI
- The selected component catalog entries and their board dependencies

## Boundaries

- Shared SoC resources belong in `UniXNU-msm8998-common`.
- Apple identity and OS ABI belong in the selected UniXNU OS profile.
- Board RAM reservations, buttons, firmware and partition policy belong here.
- Reusable drivers belong in catalog component repositories.
- Diagnostic boot arguments and security bypasses must remain visibly separate
  from release-candidate claims.

## Validation

Use the official UniXNU reference checkout pinned by the migration plan:

```sh
python3 ../references/UniXNU/unixnu defconfig cheeseburger_defconfig -D .
python3 ../references/UniXNU/unixnu status --json -D .
python3 ../references/UniXNU/scripts/porting-record.py check --device .
```

Compile the board DTS with the MSM8998 include directory before changing ADT
properties. Do not report a host parse result as a hardware boot result.
