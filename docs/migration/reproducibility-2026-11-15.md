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
for installation. Each run used the fixed downstream UniXNU commit and the same
trust cache and Data seed. The final two runs also used device commit
`10c7d82582b0d75bb31a2098012a738f270b6ab1` from a clean clone. The UniXNU and
device checkouts were clean before and after both final runs.

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

## Final clean-commit validation

Two additional builds used clean device commit
`10c7d82582b0d75bb31a2098012a738f270b6ab1`:

| Run | Kernel SHA-256 | FD SHA-256 |
| --- | --- | --- |
| A | `147eb7f4c9345c06f762f64f5c1a7d67eb1506f32c687287571669a3521d3cd1` | `a68b961bf8e213f06560f7c6de024a06dfd513dbc85042ef099385f86f2bde36` |
| B | `0e9827283217ca8e89999911c51e0605baa52ad1a8acace7d96146b9d582b5c6` | `361a0395b0548bbd8cd8eaf401661102f0c0540b6342a4284f5cf6c8cc92c594` |

The DTB remained byte-identical at
`3809aa23a82a409735b32af07a9cbe9118257a8b0ed03062994ac5b2931f755b`.
The two kernel files differed in 44 bytes across 6 ranges; the FD differed in
4 bytes inside the embedded `HH:MM:SS` build time. After the same enumerated
normalization, both comparisons again reported:

```text
kernel normalized bytes=0 ranges=0
FD normalized bytes=0 ranges=0
```

The final run's boot image and payload hashes are
`c5d78fac2d0d03d1a72880c3ff2f188ece077a43ecc63c65ce3b572d1564bb10`
and
`a983534d60d8cb038a5de05617e9d767416aa0233e46e24677cc13c861a1eaac`.
Their differences from run A are expected because the kernel identity fields and
the randomized Data handoff are embedded in those containers.
