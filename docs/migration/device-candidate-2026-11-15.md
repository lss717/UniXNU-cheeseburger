# TWRP device candidate record

Date: 2026-11-15

## Seed identity

`UNIXNU_UNIQUE_CHIP_ID` is a synthetic 64-bit software identity seed used for
XNU `IOPlatformUUID`; it is not an Apple or Qualcomm hardware identifier.

A new seed was generated with the operating-system CSPRNG. It is exactly eight
raw bytes. The plaintext seed is not stored in this repository, this document,
the manifest, the build log, or Git.

| Item | Value |
| --- | --- |
| Seed fingerprint | `1e8a3854f9c16f2ab693c0a31793d80f07a9671efa53b22514d53f2284c2beae` |
| Windows primary | `E:\WindowsData\Project\ios-backups\op5-20261115-twrp\secrets\unique-chip-id.seed` |
| Independent backup | `192.168.10.127:/home/ls/private/op5-platform-seed/unique-chip-id.seed` |
| Mac build copy | `/Users/lss/private/op5-platform-seed/unique-chip-id.seed` |
| Private build log | `/Users/lss/private/op5-platform-seed/candidate-build.log` |

The Windows file and directory ACLs allow only the current user, SYSTEM, and
Administrators. The remote and Mac copies are mode `600`.

## Fixed build inputs

| Input | Revision or hash |
| --- | --- |
| Device | `85490ba682ab1e178a35f40fd80f42ea3aac08c9` |
| UniXNU downstream | `d7fafc5d45a795a3b7fc9e9ae313e704c5a1938e` |
| UniXNU upstream base | `578ae75c8e7356d4d562bcfc294ab26dfe640a31` |
| DarwinBoot | `2ba9cad556f1794fb74ac8af164bc34b0e096338` |
| MSM8998 common | `8227828eb1706b51aea9762bd46006328d91db26` |
| edk2-msm8998 | `da889248ba63e3b76003e016ee8744b34aa91a01` |
| OS profile | `ipados-17.7.11-21H461` |
| Trust cache | `7416ca4f8377146626bb284e2133c76bee6cb4e3ee0af5d648b186235a7323a7` |
| Data seed | `d474b36616dca3227beb09243276e20818e32b11f55ad0cc2c3f1d5c0a656d41` |

## Candidate

The Windows candidate bundle is outside Git at:

```text
E:\WindowsData\Project\ios-backups\op5-20261115-candidate
```

| Output | Size | SHA-256 |
| --- | ---: | --- |
| `boot.img` | 81,274,880 | `935a54dfe1afa622e4bfad6cdd9ed4cd8a04190fa9021d7f2cae7e3e1eb36c06` |
| `payload.img` | 79,691,776 | `fab288c84e9ef9586dfdf080ee065e54563f46bdba3e9365b9a8419d120bad66` |
| `kernel.macho` | 52,043,776 | `a2059efa18cb9de12f4b2131eed5606c628f5ad72e1fd7f5cfb4b1bb499b0117` |
| `hardware.dtb` | 3,507 | `3809aa23a82a409735b32af07a9cbe9118257a8b0ed03062994ac5b2931f755b` |
| `kernel.macho.adt` | 16,408 | `d090c075149ab7ca47625a15de3d322c4df9375726c169de3fc32928035d2fec` |
| `MSM8998PKG_UEFI.fd` | 6,291,456 | `733084c21f3e6e62b319a491386a8eae3220d5447c137abf72c56ab9999f31c6` |
| `package.manifest.json` | 9,552 | `f323772656ba477fae25e04af3396f986d36f14ad70f030c072194fe69714751` |

All 47 ADT checks passed. FD/FV, boot header/load ranges, payload label, payload
members, manifest revisions and SHA-256 checks passed. The seed was explicitly
checked to be absent from the build log and manifest.

The candidate has not been booted on hardware.

## Recovery state

The current device `boot` and `recovery` partitions were read without writing to
the phone and are stored at:

```text
E:\WindowsData\Project\ios-backups\op5-20261115-twrp
```

The verified 2026-10-24 full Android system backup remains at:

```text
192.168.10.127:/home/ls/UniXNU/.cache/firmware/backups/20261024-125857
```

It contains all named system partitions and sda-sdf GPTs, but does not contain
`userdata`, `userdata_ios`, or `cache`.

## Proposed first boot

Execution still requires a new user confirmation. The preferred non-persistent
path is:

```sh
adb reboot bootloader
fastboot devices
fastboot boot E:\WindowsData\Project\ios-backups\op5-20261115-candidate\boot.img
```

This does not flash or modify `boot`, `recovery`, GPT, slots, `userdata`, or
`userdata_ios`.
