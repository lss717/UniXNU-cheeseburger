# Drivers

Device-local drivers kept out of the shared UniXNU tree:

- `CheeseburgerUsb` contains the current DWC3 handoff probe and opt-in EP0 path.
- `NativeUfsProbe` remains in the diagnostic UniXNU fork and is not yet a
  format 2 component in this repository.
- Shared SoC resources belong in `UniXNU-msm8998-common`; reusable drivers
  belong in official catalog component repositories.
