param(
    [string]$Port,
    [switch]$Reboot,
    [uint32]$RequestId = 0,
    [int]$Seconds = 0,
    [string]$TextOutput = "H410-live.log",
    [string]$FrameOutput = "H410-live.frames.bin",
    [string]$ReplayFile
)

$ErrorActionPreference = "Stop"

# H410 OnePlus 5 USB CDC live-log console.
# One exclusive COM handle runs both the UXL1 log decoder and the UXC1 command
# writer, so no second process can steal the port. There is no arbitrary text,
# memory or shell channel: the only accepted command is the 16-byte UXC1
# reboot frame.

if (-not $ReplayFile -and -not $Port) {
    $devices = @(Get-CimInstance Win32_PnPEntity |
        Where-Object { $_.PNPDeviceID -match '^USB\\VID_1209&PID_0001' })
    $ports = @($devices | ForEach-Object {
        $m = [regex]::Match("$($_.Caption) $($_.Name)", 'COM(\d+)')
        if ($m.Success) { "COM$($m.Groups[1].Value)" }
    } | Sort-Object -Unique)
    if ($ports.Count -ne 1) {
        throw "Expected exactly one CDC ACM COM port for VID_1209&PID_0001; found: $($ports -join ', ')"
    }
    $Port = $ports[0]
}

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class H410RawCom {
    [StructLayout(LayoutKind.Sequential)]
    public struct COMMTIMEOUTS {
        public uint ReadIntervalTimeout;
        public uint ReadTotalTimeoutMultiplier;
        public uint ReadTotalTimeoutConstant;
        public uint WriteTotalTimeoutMultiplier;
        public uint WriteTotalTimeoutConstant;
    }
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    public static extern SafeFileHandle CreateFile(string name, uint access, uint share,
        IntPtr security, uint creation, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool SetCommTimeouts(SafeFileHandle handle,
        ref COMMTIMEOUTS timeouts);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool ReadFile(SafeFileHandle handle, byte[] buffer,
        uint count, out uint read, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool WriteFile(SafeFileHandle handle, byte[] buffer,
        uint count, out uint written, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool EscapeCommFunction(SafeFileHandle handle, uint function);
}
"@

$textPath  = [IO.Path]::GetFullPath($TextOutput)
$framePath = [IO.Path]::GetFullPath($FrameOutput)
New-Item -ItemType Directory -Force -Path (Split-Path $textPath)  | Out-Null
New-Item -ItemType Directory -Force -Path (Split-Path $framePath) | Out-Null
[IO.File]::WriteAllBytes($textPath, @())
[IO.File]::WriteAllBytes($framePath, @())

function Get-Crc32([byte[]]$Data) {
    [uint32]$crc = [uint32]4294967295
    foreach ($v in $Data) {
        $crc = $crc -bxor [uint32]$v
        for ($bit = 0; $bit -lt 8; $bit++) {
            if (($crc -band 1) -ne 0) { $crc = ($crc -shr 1) -bxor [uint32]3988292384 }
            else { $crc = $crc -shr 1 }
        }
    }
    return [uint32]($crc -bxor [uint32]4294967295)
}

$pending    = [System.Collections.Generic.List[byte]]::new()
$frameStream = [IO.MemoryStream]::new()
$health = [ordered]@{
    frames = 0; payload_bytes = 0; crc_errors = 0; sequence_gaps = 0
    truncated = 0; boot_switches = 0; boots = @(); last_sequence = $null
}
$script:connectedBoot = $null
$script:expectedSequence = $null

function Decode-Pending {
    while ($pending.Count -ge 4) {
        $start = -1
        for ($i = 0; $i + 4 -le $pending.Count; $i++) {
            if ($pending[$i] -eq 0x55 -and $pending[$i+1] -eq 0x58 -and
                $pending[$i+2] -eq 0x4c -and $pending[$i+3] -eq 0x31) { $start = $i; break }
        }
        if ($start -lt 0) { $pending.Clear(); return }
        if ($start -gt 0) { $pending.RemoveRange(0, $start) }
        if ($pending.Count -lt 32) { return }
        $hdr = [byte[]]$pending.GetRange(0, 32).ToArray()
        if ($hdr[4] -ne 1 -or $hdr[5] -ne 32) { $pending.RemoveAt(0); continue }
        $len = [BitConverter]::ToUInt16($hdr, 24)
        if ($len -gt 1984) { $pending.RemoveAt(0); continue }
        if ($pending.Count -lt (32 + $len)) { return }
        $frame = [byte[]]$pending.GetRange(0, 32 + $len).ToArray()
        $payload = if ($len -gt 0) { [byte[]]$frame[32..(31 + $len)] } else { [byte[]]@() }
        $expected = [BitConverter]::ToUInt32($frame, 28)
        if ((Get-Crc32 $payload) -ne $expected) {
            Write-Warning "CRC mismatch at a live frame; resynchronising"
            $health.crc_errors++
            $pending.RemoveAt(0)
            continue
        }
        $bootId = [BitConverter]::ToUInt32($frame, 8)
        $seq = [BitConverter]::ToUInt32($frame, 12)
        $build = [Text.Encoding]::ASCII.GetString($frame, 20, 4)
        if ($null -eq $script:connectedBoot) {
            $script:connectedBoot = $bootId
            $health.boots += ("{0:X8}/{1}" -f $bootId, $build)
            Write-Host ("H410 connected boot={0:X8} build={1}" -f $bootId, $build)
        } elseif ($script:connectedBoot -ne $bootId) {
            $script:connectedBoot = $bootId
            $health.boot_switches++
            $script:expectedSequence = $null
            $health.boots += ("{0:X8}/{1}" -f $bootId, $build)
            Write-Warning ("boot changed to {0:X8}/{1}" -f $bootId, $build)
        }
        if ($null -ne $script:expectedSequence -and $seq -ne $script:expectedSequence) {
            $health.sequence_gaps++
            Write-Warning "sequence gap: expected $($script:expectedSequence) got $seq"
        }
        $script:expectedSequence = if ($seq -eq [uint32]::MaxValue) { [uint32]0 } else { [uint32]($seq + 1) }
        $frameStream.Write($frame, 0, $frame.Length)
        if ($len -gt 0) {
            $text = [Text.Encoding]::UTF8.GetString($payload)
            [IO.File]::AppendAllText($textPath, $text)
            if ($text.Length -gt 0) { Write-Host -NoNewline $text }
            $health.payload_bytes += $len
        }
        $health.frames++
        $health.last_sequence = $seq
        $pending.RemoveRange(0, 32 + $len)
    }
}

$handle = $null
$devicePath = $null
$buffer = New-Object byte[] 4096

if ($ReplayFile) {
    $raw = [IO.File]::ReadAllBytes([IO.Path]::GetFullPath($ReplayFile))
    for ($off = 0; $off -lt $raw.Length; $off += $buffer.Length) {
        $n = [Math]::Min($buffer.Length, $raw.Length - $off)
        $chunk = [byte[]]::new($n)
        [Array]::Copy($raw, $off, $chunk, 0, $n)
        $pending.AddRange($chunk)
        Decode-Pending
    }
} else {
    $devicePath = "\\.\$Port"
    $handle = [H410RawCom]::CreateFile($devicePath, [uint32]3221225472, 0,
        [IntPtr]::Zero, 3, 0, [IntPtr]::Zero)
    if ($handle.IsInvalid) {
        $e = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        throw "CreateFile $devicePath failed with Win32 error $e"
    }
    $to = New-Object H410RawCom+COMMTIMEOUTS
    $to.ReadIntervalTimeout = [uint32]50
    $to.ReadTotalTimeoutMultiplier = [uint32]0
    $to.ReadTotalTimeoutConstant = [uint32]500
    $to.WriteTotalTimeoutMultiplier = [uint32]0
    $to.WriteTotalTimeoutConstant = [uint32]500
    if (-not [H410RawCom]::EscapeCommFunction($handle, 5) -or
        -not [H410RawCom]::EscapeCommFunction($handle, 3)) {
        $e = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        $handle.Dispose()
        throw "SetDTR/SetRTS failed with Win32 error $e"
    }
    if (-not [H410RawCom]::SetCommTimeouts($handle, [ref]$to)) {
        $e = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        $handle.Dispose()
        throw "SetCommTimeouts failed with Win32 error $e"
    }
    Write-Host "H410 console opened $devicePath (DTR/RTS asserted)"

    if ($Reboot) {
        if ($RequestId -eq 0) {
            $RequestId = [uint32]((([DateTime]::UtcNow.Ticks) -band 0xffffffff))
            if ($RequestId -eq 0) { $RequestId = 1 }
        }
        $cmd = New-Object byte[] 16
        $cmd[0] = 0x55; $cmd[1] = 0x58; $cmd[2] = 0x43; $cmd[3] = 0x31
        $cmd[4] = 1; $cmd[5] = 1; $cmd[6] = 0; $cmd[7] = 0
        [Array]::Copy([BitConverter]::GetBytes([uint32]$RequestId), 0, $cmd, 8, 4)
        $crc = Get-Crc32 ([byte[]]$cmd[0..11])
        [Array]::Copy([BitConverter]::GetBytes([uint32]$crc), 0, $cmd, 12, 4)
        [uint32]$written = 0
        if (-not [H410RawCom]::WriteFile($handle, $cmd, 16, [ref]$written,
                [IntPtr]::Zero) -or $written -ne 16) {
            $e = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
            $handle.Dispose()
            throw "WriteFile reboot command failed with Win32 error $e"
        }
        Write-Host ("H410 reboot command sent id={0} crc={1:X8}" -f $RequestId, $crc)
    }

    $deadline = if ($Seconds -gt 0) { [DateTime]::UtcNow.AddSeconds($Seconds) } else { $null }
    try {
        while (-not $deadline -or [DateTime]::UtcNow -lt $deadline) {
            [uint32]$read = 0
            $ok = [H410RawCom]::ReadFile($handle, $buffer, 4096, [ref]$read, [IntPtr]::Zero)
            if ($read -gt 0) {
                $chunk = [byte[]]::new([int]$read)
                [Array]::Copy($buffer, 0, $chunk, 0, [int]$read)
                $pending.AddRange($chunk)
                Decode-Pending
            }
        }
    } finally {
        $handle.Dispose()
    }
}

Decode-Pending
[IO.File]::WriteAllBytes($framePath, $frameStream.ToArray())
$frameStream.Dispose()
Write-Host ""
Write-Host ("H410 frames={0} payload_bytes={1} crc_errors={2} sequence_gaps={3} boot_switches={4} last_sequence={5}" -f `
    $health.frames, $health.payload_bytes, $health.crc_errors, $health.sequence_gaps,
    $health.boot_switches, $health.last_sequence)
Write-Host "text=$textPath"
Write-Host "frames=$framePath"
