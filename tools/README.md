# H408/H410 CDC ACM live-log tools

The device-side driver advertises a two-interface CDC ACM device with:

- notification endpoint `0x82`, interrupt, 16-byte maximum packet;
- data OUT endpoint `0x01`, bulk, 512-byte maximum packet;
- data IN endpoint `0x81`, bulk, 512-byte maximum packet.

Windows binds it as `usbser` with PnP ID
`USB\VID_1209&PID_0001\OP5CDCH408`.

## Live console and normal reboot

`h410-cdc-console.ps1` opens one exclusive COM handle, decodes `UXL1` live,
appends readable text and frames to disk, and can send the only supported
host command (normal reboot) through the same handle:

```powershell
.\tools\h410-cdc-console.ps1 -Port COM8 -Seconds 600 `
  -TextOutput H410-live.log -FrameOutput H410-live.frames.bin

.\tools\h410-cdc-console.ps1 -Port COM8 -Reboot -RequestId 12345 -Seconds 120
```

The reboot command is a 16-byte `UXC1` frame on Bulk-OUT:
`"UXC1"`, version `1`, opcode `1`, payload length `0`, non-zero request ID
and CRC32 over the first 12 bytes. The device only accepts exactly 16 bytes
with a valid CRC; there is no text, shell, memory or register channel. It
replies `NATIVE_USB_CMD: ack req=<id> op=reboot`, then a dedicated kernel
worker calls `halt_all_cpus(TRUE)`. This is a normal reboot only; it does not
set a recovery marker. Full Windows instructions and the COM troubleshooting
checklist are in `../UniXNU/docs/zh/devices/cheeseburger/usb-com-log-guide.md`.

## Capture

From the device repository root:

```powershell
.\tools\capture-h408-cdc.ps1 -Seconds 360 `
  -TextOutput H408-live.log `
  -FrameOutput H408-live.frames.bin
```

Without `-Port`, the receiver requires exactly one H408 CDC ACM COM port.
It opens the raw device path and asserts DTR/RTS because the firmware gates
log transmission on `SET_CONTROL_LINE_STATE( DTR | RTS )`.

The script writes:

- `.log`: concatenated frame payloads;
- `.frames.bin`: concatenated UXL1 headers and payloads;
- `.frames.bin.raw.bin`: bytes read directly from the COM handle.

The final summary reports frames, bytes, CRC errors, sequence gaps, boot
switches and truncated tails. For a normal acceptance run all three error
counters and the truncation count must be zero.

## Replay and tests

Static descriptor checks, deterministic decoder edge cases and captured
streams can be checked together:

```powershell
python .\tools\check-h408-cdc.py --self-test `
  --replay H408-live.frames.bin `
  --replay H408-reopen.frames.bin
```

The self-test covers CRC corruption and resynchronization, truncated input,
32-bit sequence wrap and boot-ID switching. A deterministic edge-case stream
can also be replayed through the PowerShell receiver:

```powershell
python .\tools\check-h408-cdc.py `
  --write-edge-fixture H408-edge.raw.bin

.\tools\capture-h408-cdc.ps1 `
  -ReplayFile H408-edge.raw.bin `
  -TextOutput H408-edge.log `
  -FrameOutput H408-edge.frames.bin
```

## UXL1 wire format

All integers are little-endian. The fixed header is 32 bytes:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | magic `UXL1` |
| 4 | 1 | version, currently 1 |
| 5 | 1 | header size, currently 32 |
| 8 | 4 | boot ID |
| 12 | 4 | monotonic sequence number |
| 16 | 4 | cumulative dropped log bytes |
| 20 | 4 | build ID, currently `H410` |
| 24 | 2 | payload length, at most 1984 |
| 28 | 4 | CRC-32/IEEE of the payload |

Sequence numbers wrap modulo 2^32. A new boot ID resets sequence continuity;
both conditions are detected and reported explicitly.

## Safety

Capture is read-only with respect to the phone. The receiver opens the COM
port, asserts modem control lines, reads bytes and closes the handle. It does
not reset, flash or clear the phone.
