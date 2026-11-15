# Format 2 host reproducibility record

Date: 2026-11-15

## Fixed inputs

| Input | Revision |
| --- | --- |
| Official UniXNU base | `578ae75c8e7356d4d562bcfc294ab26dfe640a31` |
| Downstream UniXNU | `d7fafc5d45a795a3b7fc9e9ae313e704c5a1938e` |
| Downstream source tree | `2f1ea33e2b842c2529fe0698955473b2178df918` |
| DarwinBoot | `2ba9cad556f1794fb74ac8af164bc34b0e096338` |
| MSM8998 common | `8227828eb1706b51aea9762bd46006328d91db26` |
| edk2-msm8998 | `da889248ba63e3b76003e016ee8744b34aa91a01` |

All runs used test platform seed `0x12345678`; these artifacts are not suitable
for installation. Each run used the fixed downstream UniXNU commit, deleted the
previous output root first, and ran `scripts/package.sh` with the same trust
cache and Data seed. The UniXNU checkout was clean before and after every run.

## Raw results

Three independent clean builds completed with 61 ordered XNU patches and all 47
ADT checks passing.

| Run | Kernel SHA-256 | FD SHA-256 | Boot SHA-256 |
| --- | --- | --- | --- |
| 1 | `c0a7ee6b40a739ab7347c2cd5becaf0a5c369e45adea7bbf17125ee0915e2b05` | `1f60635b7e1ac467a378651e4dcd664103bcbcb3cf7b1321e01d55b7a77294ae` | `16dd1c51c312f43cb33739ff09575a3fd7f3d247e8639f9dd920d79145ace40c` |
| 2 | `85bfc4b2ca0717ad75555fc8804c08fa074727724dc4acbb739d7f1306191117` | `c1cdd6e446ef2ce99be4cdce1ecd32d5a816bf46767889fe428a0e54a2eb31b3` | `fb29d439994b1a42144047afb4febf370d878700d316f7e70d81e400c4cca5d3` |
| 3 | `0990f2bc765e4c2d3f1538fef732ad65e43787a50962e13df28bf184f779b845` | `8153faec5b3a6df507525734caab7780ca0e973b3feb1888d406c9c21690e885` | `ce31464850db77f9d95db223a72e4bcea20074ced9be4128df5ae009d72e1b61` |

The DTB was byte-identical in all runs:

```text
3809aa23a82a409735b32af07a9cbe9118257a8b0ed03062994ac5b2931f755b
```

Payload and extracted ADT hashes vary because the common generator embeds
randomized handoff data. The manifest hash also varies with randomized payload
metadata and device revision while the source inputs remain fixed.

## Normalized comparison

The raw difference between runs 2 and 3 was:

- kernel: 49 bytes in 9 ranges;
- FD: 6 bytes in 3 ranges.

The kernel ranges map to one Mach-O UUID, 960 `N_OSO` debug timestamps, one
derived `_PrelinkKCID`, and the two repeated Darwin kernel build-date strings.
The FD ranges map only to the embedded `HH:MM:SS` build time.

Applying exactly those enumerated normalizations produced:

```text
kernel normalized bytes=0 ranges=0
FD normalized bytes=0 ranges=0
```

This is not a claim that raw image bytes are identical. The exact image that is
executed must retain its own SHA-256, and the diagnostic build remains unsuitable
for hardware installation because it uses the test platform seed.
