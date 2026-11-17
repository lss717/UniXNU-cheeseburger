param(
    [string]$Port,
    [string]$TextOutput = "H408-live.log",
    [string]$FrameOutput = "H408-live.frames.bin",
    [int]$Seconds = 0
)

$ErrorActionPreference = "Stop"

if (-not $Port) {
    $devices = @(Get-CimInstance Win32_PnPEntity |
        Where-Object { $_.PNPDeviceID -match '^USB\\VID_1209&PID_0001' })
    $ports = @($devices | ForEach-Object {
        $match = [regex]::Match("$($_.Caption) $($_.Name)", 'COM(\d+)')
        if ($match.Success) { "COM$($match.Groups[1].Value)" }
    } | Sort-Object -Unique)
    if ($ports.Count -ne 1) {
        throw "Expected one CDC ACM COM port for VID_1209&PID_0001; found: $($ports -join ', ')"
    }
    $Port = $ports[0]
}

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class H408RawCom {
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
    public static extern bool EscapeCommFunction(SafeFileHandle handle, uint function);
}
"@

$textPath = [IO.Path]::GetFullPath($TextOutput)
$framePath = [IO.Path]::GetFullPath($FrameOutput)
$rawPath = "$framePath.raw.bin"
New-Item -ItemType Directory -Force -Path (Split-Path $textPath) | Out-Null
New-Item -ItemType Directory -Force -Path (Split-Path $framePath) | Out-Null

$devicePath = "\\.\$Port"
$handle = [H408RawCom]::CreateFile($devicePath, [uint32]3221225472, 0,
    [IntPtr]::Zero, 3, 0, [IntPtr]::Zero)
if ($handle.IsInvalid) {
    $errorCode = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    throw "CreateFile $devicePath failed with Win32 error $errorCode"
}
$timeouts = New-Object H408RawCom+COMMTIMEOUTS
$timeouts.ReadIntervalTimeout = [uint32]50
$timeouts.ReadTotalTimeoutMultiplier = [uint32]0
$timeouts.ReadTotalTimeoutConstant = [uint32]500
$timeouts.WriteTotalTimeoutMultiplier = [uint32]0
$timeouts.WriteTotalTimeoutConstant = [uint32]500
if (-not [H408RawCom]::EscapeCommFunction($handle, 5) -or
    -not [H408RawCom]::EscapeCommFunction($handle, 3)) {
    $errorCode = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    $handle.Dispose()
    throw "SetDTR/SetRTS failed with Win32 error $errorCode"
}

if (-not [H408RawCom]::SetCommTimeouts($handle, [ref]$timeouts)) {
    $errorCode = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    $handle.Dispose()
    throw "SetCommTimeouts $devicePath failed with Win32 error $errorCode"
}

function Get-Le32([byte[]]$Data, [int]$Offset) {
    return [BitConverter]::ToUInt32($Data, $Offset)
}

function Get-Crc32([byte[]]$Data) {
    [uint32]$crc = [uint32]4294967295
    foreach ($value in $Data) {
        $crc = $crc -bxor [uint32]$value
        for ($bit = 0; $bit -lt 8; $bit++) {
            if (($crc -band 1) -ne 0) {
                $crc = ($crc -shr 1) -bxor [uint32]3988292384
            } else {
                $crc = $crc -shr 1
            }
        }
    }
    return ($crc -bxor [uint32]4294967295)
}

$capture = [IO.MemoryStream]::new()
$buffer = New-Object byte[] 4096
$deadline = if ($Seconds -gt 0) { [DateTime]::UtcNow.AddSeconds($Seconds) } else { $null }
try {
    Write-Host "H408 raw CDC receiver opened $devicePath"
    while (-not $deadline -or [DateTime]::UtcNow -lt $deadline) {
        [uint32]$read = 0
        $ok = [H408RawCom]::ReadFile($handle, $buffer, 4096, [ref]$read,
            [IntPtr]::Zero)
        if ($read -gt 0) {
            $capture.Write($buffer, 0, [int]$read)
        }
    }
} finally {
    $handle.Dispose()
}

$data = $capture.ToArray()
$capture.Dispose()
[IO.File]::WriteAllBytes($rawPath, $data)
$text = [IO.MemoryStream]::new()
$frames = [IO.MemoryStream]::new()
$index = 0
$expectedSequence = $null
$lastDropped = 0
$frameCount = 0
$connectedBoot = $null
while ($index + 4 -le $data.Length) {
    if ($data[$index] -ne 0x55 -or $data[$index + 1] -ne 0x58 -or
        $data[$index + 2] -ne 0x4c -or $data[$index + 3] -ne 0x31) {
        $index++
        continue
    }
    if ($index + 32 -gt $data.Length) { break }
    $header = New-Object byte[] 32
    [Array]::Copy($data, $index, $header, 0, 32)
    if ($header[4] -ne 1 -or $header[5] -ne 32) {
        $index++
        continue
    }
    $length = [BitConverter]::ToUInt16($header, 24)
    if ($index + 32 + $length -gt $data.Length) { break }
    $payload = New-Object byte[] $length
    [Array]::Copy($data, $index + 32, $payload, 0, $length)
    $expectedCrc = Get-Le32 $header 28
    if ((Get-Crc32 $payload) -ne $expectedCrc) {
        Write-Warning "H408 CRC mismatch at byte offset $index"
        $index++
        continue
    }
    $bootId = Get-Le32 $header 8
    $sequence = Get-Le32 $header 12
    $dropped = Get-Le32 $header 16
    $build = [Text.Encoding]::ASCII.GetString($header, 20, 4)
    if ($null -eq $connectedBoot) {
        $connectedBoot = $bootId
        Write-Host ("H408 connected boot={0:X8} build={1}" -f $bootId, $build)
    } elseif ($connectedBoot -ne $bootId) {
        throw "Boot ID changed without reconnect"
    }
    if ($build -ne "H408") { throw "Unexpected build ID '$build'" }
    if ($null -ne $expectedSequence -and $sequence -ne $expectedSequence) {
        Write-Warning "Sequence gap: expected $expectedSequence, got $sequence"
    }
    $expectedSequence = $sequence + 1
    if ($dropped -ne $lastDropped) {
        Write-Warning "Device dropped $($dropped - $lastDropped) additional log bytes"
        $lastDropped = $dropped
    }
    $frames.Write($header, 0, $header.Length)
    $frames.Write($payload, 0, $payload.Length)
    $text.Write($payload, 0, $payload.Length)
    $frameCount++
    $index += 32 + $length
}
[IO.File]::WriteAllBytes($textPath, $text.ToArray())
[IO.File]::WriteAllBytes($framePath, $frames.ToArray())
$text.Dispose(); $frames.Dispose()
Write-Host "H408 frames=$frameCount bytes=$($data.Length)"
Write-Host "text=$textPath"
Write-Host "frames=$framePath"
Write-Host "raw=$rawPath"
if ($frameCount -eq 0) { throw "No valid UXL1 frames received" }
