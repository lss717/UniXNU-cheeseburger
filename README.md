# UniXNU-cheeseburger

Device repository for the OnePlus 5 (cheeseburger, MSM8998 / Snapdragon 835), B line:
the UniXNU loader and DarwinBoot from the firmware volume, a 4 KiB XNU, and the stock ABL.

Final target: iPhone10,2 / iOS 16.7.16 (20H392).  B3 uses iPad7,1 iPadOS 17.7.11 as a
temporary fixture to isolate the MSM8998 platform; it is not the delivery identity.

## Status

| Milestone | State |
| --- | --- |
| B1 — UEFI + UniXNU + payload | ✅ G1 passed on hardware, 2026-10-22 |
| B2 — this repository, ADT, payload, boot image | 🔄 skeleton (board file, defconfig, scripts) |
| B3 — 4 KiB XNU boot chain | ⬜ needs macOS or the GitHub Actions runner |

G1 evidence and the measured board values live in the UniXNU tree, under
`plans/oneplus5-cheeseburger/` (`STATUS.md`, `PLAN.md`, `assets/`, `patches/`).

## Measured board values (2026-10-22)

| Item | Value |
| --- | --- |
| RAM | 8 GB installed, 7039 MiB visible to UEFI |
| UFS logical block | 4096 bytes |
| UFS LUN0 | 29,763,584 blocks, 116,264 MiB |
| userdata | LBA `0x14F40`..`0x1C627FA`, 29,677,755 blocks |
| Frame buffer | `0x9D400000`, 1080x1920, stride 1080, BGRX, `0x7E9000` bytes |
| Keys | volume up/down and power, handled by Qualcomm `ButtonsDxe` |
| Boot image | header v1, 2048-byte pages, kernel `0x10000000`, ramdisk `0x12000000`, tags `0x18000000` (64 MiB payload) |

## Layout

```text
board.toml                 board, IPSW fixture, userdata, kernel, payload
board.iphone10-2.toml      B6 target profile, unused until the switch
configs/cheeseburger_defconfig
scripts/make-bootimg.py    edk2-msm8998 mkbootimg + header report
scripts/check-cheeseburger.py  FD / boot image / payload preflight
scripts/make-cheeseburger-adt.py  Apple-format device tree for the B3 boot
scripts/check-adt.py         parse and check /cpus, /arm-io/intc, /chosen
package/ drivers/ patches/ own modules, added by later milestones
```

## Build

Firmware is built by `edk2-msm8998/build.sh`, not by `unixnu build firmware`: the B line
keeps the GCC5 DSC/FDF and its own boot image layout (plan decisions D6/D7).  Clone
UniXNU, this repository and `edk2-msm8998` side by side.

```sh
# 1. firmware (UniXNU + DarwinBoot + initrd block device)
cd /home/ls/edk2-msm8998
UNIXNU_TREE=/home/ls/UniXNU bash build.sh --device cheeseburger --release RELEASE

# 2. payload (B1: just the UniXNU config; B3 adds the kernel, ADT and trust cache)
#    out/cheeseburger/boot/payload.img, FAT label OP5ESP

# 3. boot image
python3 scripts/make-bootimg.py \
  --payload out/cheeseburger/boot/payload.img \
  --out out/cheeseburger/boot/boot.img

# 4. ADT (B3; B1-B2 payloads only need the UniXNU config)
python3 scripts/make-cheeseburger-adt.py \
  --output out/cheeseburger/boot/kernel.macho.adt \
  --dram-size 0x200000000 --owned-memory-size 0x80000000 \
  --cpu-regs 0,1,2,3,0x100,0x101,0x102,0x103 --ipad-identity
python3 scripts/check-adt.py --adt out/cheeseburger/boot/kernel.macho.adt

# 5. preflight and run
python3 scripts/check-cheeseburger.py
fastboot boot out/cheeseburger/boot/boot.img
```

`fastboot boot` only loads the image into RAM; nothing is written to a partition.

## macOS CI

The UniXNU repository carries `.github/workflows/macos-cheeseburger.yml` for the steps that
need Xcode.  After pushing this repository:

```sh
# in the UniXNU checkout
gh workflow run macos-cheeseburger.yml \
  -f device_repository=<owner>/UniXNU-cheeseburger \
  -f device_ref=main
```

It runs `unixnu defconfig cheeseburger_defconfig`, fetches the XNU sources, builds the kernel
and the payload, and uploads `out/cheeseburger/{kernel,payload,logs}`.  The boot image is still
assembled from the edk2-msm8998 firmware with `scripts/make-bootimg.py` (the workflow does not
build the firmware).

Secrets: `UNIXNU_DEVICE_TOKEN` for a private device repository, `UNIXNU_UNIQUE_CHIP_ID` for the
payload ADT seed.

`panic_screen` is on: `scripts/patch-xnu-entry-probe.py` redirects `panic()` into
`scripts/xnu-panic-screen.c`, which draws the message with `scripts/Font.h` and stops, so the
panorama stays on the panel for a photograph.  `--no-reset` keeps the early stages from resetting.

The first kernel builds without the legacy display and boot-progress kits: sheng's
`build-native-legacy-display.py` / `build-boot-progress.py` are tied to its Adreno, touch and
backlight probes.  That does not hide the B3 console: DarwinBoot passes the GOP frame buffer
through `boot_args->Video`, so XNU's own verbose/panic console draws on the panel.  The legacy
display kit is for `IOMobileFramebuffer` and the userland UI (B7), not for B3.

## B3 inputs that are not in this repository

`unixnu ipsw fetch` (on macOS) caches `KernelCache`, `DeviceTree`, `StaticTrustCache` and
`Cryptex1,SystemTrustCache` under `.cache/firmware/`; the kernel and prelink steps also
need Xcode.  The `kernelcache_sha256` and the prelink roots in `board.toml` are filled in
when that first cache exists.
