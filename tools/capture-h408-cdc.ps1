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
        $text = "$($_.Caption) $($_.Name)"
        $match = [regex]::Match($text, '\(COM(\d+)\)')
        if ($match.Success) { "COM$($match.Groups[1].Value)" }
    } | Sort-Object -Unique)
    if ($ports.Count -ne 1) {
        throw "Expected one CDC ACM COM port for VID_1209&PID_0001; found: $($ports -join ', ')"
    }
    $Port = $ports[0]
}

$textPath = [IO.Path]::GetFullPath($TextOutput)
$framePath = [IO.Path]::GetFullPath($FrameOutput)
New-Item -ItemType Directory -Force -Path (Split-Path $textPath) | Out-Null
New-Item -ItemType Directory -Force -Path (Split-Path $framePath) | Out-Null

$serial = [IO.Ports.SerialPort]::new($Port, 115200, [IO.Ports.Parity]::None, 8, [IO.Ports.StopBits]::One)
$serial.DtrEnable = $true
$serial.RtsEnable = $true
$serial.ReadTimeout = 500
$serial.Open()
$stream = $serial.BaseStream
$text = [IO.File]::Open($textPath, [IO.FileMode]::Append, [IO.FileAccess]::Write, [IO.FileShare]::Read)
$frames = [IO.File]::Open($framePath, [IO.FileMode]::Append, [IO.FileAccess]::Write, [IO.FileShare]::Read)

function Read-Byte {
    while ($true) {
        try { return $stream.ReadByte() }
        catch [TimeoutException] { }
    }
}

function Read-Exact([int]$Length) {
    $buffer = [byte[]]::new($Length)
    $offset = 0
    while ($offset -lt $Length) {
        try {
            $count = $stream.Read($buffer, $offset, $Length - $offset)
            if ($count -le 0) { throw "CDC stream closed" }
            $offset += $count
        } catch [TimeoutException] {
            if ($script:deadline -and [DateTime]::UtcNow -ge $script:deadline) { throw "capture deadline reached" }
        }
    }
    return $buffer
}

function Get-Le32([byte[]]$Data, [int]$Offset) {
    return [BitConverter]::ToUInt32($Data, $Offset)
}

function Get-Crc32([byte[]]$Data) {
    [uint32]$crc = 0xffffffff
    foreach ($value in $Data) {
        $crc = $crc -bxor [uint32]$value
        for ($bit = 0; $bit -lt 8; $bit++) {
            $mask = [uint32](0 - ($crc -band 1))
            $crc = ($crc -shr 1) -bxor (0xedb88320 -band $mask)
        }
    }
    return [uint32](-bnot $crc)
}

$deadline = if ($Seconds -gt 0) { [DateTime]::UtcNow.AddSeconds($Seconds) } else { $null }
$expectedSequence = $null
$lastDropped = 0
$frameCount = 0
$bytesWritten = 0
$connectedBoot = $null

try {
    Write-Host "H408 CDC receiver opened $Port"
    while (-not $deadline -or [DateTime]::UtcNow -lt $deadline) {
        if ((Read-Byte) -ne 0x55) { continue }
        if ((Read-Byte) -ne 0x58) { continue }
        if ((Read-Byte) -ne 0x4c) { continue }
        if ((Read-Byte) -ne 0x31) { continue }

        $header = [byte[]]::new(32)
        $header[0] = 0x55; $header[1] = 0x58; $header[2] = 0x4c; $header[3] = 0x31
        $rest = Read-Exact 28
        [Array]::Copy($rest, 0, $header, 4, 28)
        if ($header[4] -ne 1 -or $header[5] -ne 32) { throw "Unsupported UXL1 header" }

        $bootId = Get-Le32 $header 8
        $sequence = Get-Le32 $header 12
        $dropped = Get-Le32 $header 16
        $build = [Text.Encoding]::ASCII.GetString($header, 20, 4)
        $length = [BitConverter]::ToUInt16($header, 24)
        $expectedCrc = Get-Le32 $header 28
        $payload = if ($length) { Read-Exact $length } else { [byte[]]::new(0) }
        $actualCrc = Get-Crc32 $payload
        if ($actualCrc -ne $expectedCrc) { throw "CRC mismatch seq=$sequence" }

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
        if ($dropped -ne $lastDropped) {
            Write-Warning "Device dropped $($dropped - $lastDropped) additional log bytes"
            $lastDropped = $dropped
        }
        $expectedSequence = $sequence + 1
        $frames.Write($header, 0, $header.Length)
        $frames.Write($payload, 0, $payload.Length)
        $text.Write($payload, 0, $payload.Length)
        $text.Flush(); $frames.Flush()
        $frameCount++
        $bytesWritten += $payload.Length
    }
} finally {
    $text.Dispose(); $frames.Dispose()
    if ($serial.IsOpen) { $serial.Close() }
    $serial.Dispose()
    Write-Host "H408 frames=$frameCount log_bytes=$bytesWritten"
    Write-Host "text=$textPath"
    Write-Host "frames=$framePath"
}
