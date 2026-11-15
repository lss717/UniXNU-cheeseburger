# Format 1 / format 2 ADT review

Date: 2026-11-15

Both ADTs were generated from the same platform identity and deterministic
random seed. The raw path-by-path result is in
`adt-diff-2026-11-15.md`.

## Inputs

| Item | Value |
| --- | --- |
| Unique chip ID | `0x12345678` |
| Chip ID | `0x8011` |
| DRAM | `0x80000000`, 8 GiB |
| Owned memory | 4 GiB |
| Root UUID | `04769D26-2CA0-4825-B443-E2E5F88300F2` |
| Data format | packed RAM disk |
| Sidecars | TrustCache, GpuAddon |
| Random seed | deterministic bytes `0x00..0xff` |
| Format 1 ADT | 14,936 bytes, SHA-256 `d2a6256b9ea332776a35e7d1e8a1a8a689f0878c3617adc500faba3c5050892d` |
| Format 2 ADT | 16,460 bytes, SHA-256 `0a5a9973519e2c3ab66e097fe74f20fa6cddb6a623bc797570efb2f58f8e4a84` |

## Result

- Old node count: 22.
- New node count: 25.
- Nodes present in format 1 but absent from format 2: 0.
- Nodes added by format 2: `/arm-io/timer`, `/arm-io/ufsphy`,
  `/product/audio`.

The CPU comparison caught and fixed one migration regression before this
result: MPIDR `0..3` must remain Cortex-A73/P and `0x100..0x103` must remain
Cortex-A53/E. After the fix, CPU compatible and cluster data no longer differ.

## Difference classification

| Difference | Classification |
| --- | --- |
| Root compatible gains `J120AP` first | Intentional OS profile identity ordering |
| `arm-io` base moves from `0x17000000` to `0x01000000` | Intentional common-window expansion to include UFS |
| GIC register offsets change | Expected recomputation from the new `arm-io` base |
| UFS reg/interrupt properties are added | Intentional common-DTS resource export |
| UFS `status=okay` is absent from the ADT | Non-runtime metadata; enabled state was already applied by FDT parsing |
| Random seed grows from 64 to 256 bytes | Intentional format 2 generator behavior |
| CPU `cluster-type` P/E is added | Intentional typed cluster metadata |
| `/arm-io/timer` and `/arm-io/ufsphy` are added | Intentional SoC ownership |
| Product ID and audio node are added | Intentional selected OS profile identity |

## Preserved critical data

The comparison found no differences for:

- CPU MPIDR values and CPU compatible strings after the mapping fix;
- DRAM base and size;
- unique chip ID, chip ID and root matching;
- SystemVolume auth blob and boot manifest placeholder;
- Wi-Fi and Bluetooth local address placeholders;
- packed RAM disk, trust cache and GPU addon placeholders;
- UFS compatible set and `p8,msm8998-direct-dma=1`;
- UFS physical address, SPI identifier and probe policy flags;
- buttons, defaults and product identity values.

No unexplained critical node or property loss remains in the host comparison.
Runtime behavior still requires a format 2 boot.
