param(
 [string]$Port = '',
 [uint32]$RequestId = 424243
)
$ErrorActionPreference='Stop'
$root='E:\Users\ls\Documents\OnePlus5_ios\H411-latest-main'
$serial='16149951'
$state=Join-Path $root 'H411-recovery-loop-state.txt'
function Set-State([string]$Name,[string]$Detail='') {
 "[$((Get-Date).ToString('o'))] $Name $Detail" | Add-Content -LiteralPath $state -Encoding UTF8
 Write-Output "$Name $Detail"
}
Set-Content -LiteralPath $state -Value "START candidate=H411 serial=$serial request_id=$RequestId" -Encoding UTF8
if(-not $Port){
 $devices=@(Get-CimInstance Win32_PnPEntity | Where-Object { $_.PNPDeviceID -match '^USB\\VID_1209&PID_0001' })
 $ports=@($devices | ForEach-Object { $m=[regex]::Match("$($_.Caption) $($_.Name)",'COM(\d+)'); if($m.Success){"COM$($m.Groups[1].Value)"} } | Sort-Object -Unique)
 if($ports.Count -ne 1){Set-State 'STOP_CDC_NOT_UNIQUE' ($ports -join ','); throw "Expected one CDC port; got $($ports -join ',')"}
 $Port=$ports[0]
}
$android=@(adb devices -l 2>&1 | ForEach-Object {$_.ToString()} | Where-Object { $_ -match '^'+[regex]::Escape($serial)+'\s+'})
$fastboot=@(fastboot devices 2>&1 | ForEach-Object {$_.ToString()} | Where-Object {$_ -match '\S'})
if($android.Count -ne 0 -or $fastboot.Count -ne 0){Set-State 'STOP_TRANSPORT_NOT_EXCLUSIVE' "adb=$($android.Count) fastboot=$($fastboot.Count)"; throw 'Expected no Android/Fastboot transport before UXC1'}
$console=Join-Path $root 'tools\h410-cdc-console.ps1'
if(-not (Test-Path -LiteralPath $console)){Set-State 'STOP_CONSOLE_MISSING'; throw "Missing $console"}
Set-State 'UXC1_SEND_BEGIN' "port=$Port"
$consoleOut=Join-Path $root 'H411-reboot-console.stdout.txt'
$consoleErr=Join-Path $root 'H411-reboot-console.stderr.txt'
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $console -Port $Port -Reboot -RequestId $RequestId -Seconds 45 -TextOutput (Join-Path $root 'H411-reboot.log') -FrameOutput (Join-Path $root 'H411-reboot.frames.bin') 1> $consoleOut 2> $consoleErr
$consoleCode=$LASTEXITCODE
if($consoleCode -ne 0){Set-State 'STOP_UXC1_CONSOLE_FAILED' "exit=$consoleCode"; throw "Console exited $consoleCode"}
$sentLine=@(Get-Content -LiteralPath $consoleOut -ErrorAction SilentlyContinue | Where-Object {$_ -match "reboot command sent id=$RequestId"})
if($sentLine.Count -ne 1){Set-State 'STOP_UXC1_SEND_UNCONFIRMED'; throw 'Console did not report exactly one send'}
Set-State 'UXC1_SENT_ONCE' ($sentLine -join ' | ')
$ackLine=@(Get-Content -LiteralPath (Join-Path $root 'H411-reboot.log') -ErrorAction SilentlyContinue | Where-Object {$_ -match "NATIVE_USB_CMD: ack req=$RequestId op=reboot"})
if($ackLine.Count -gt 0){Set-State 'UXC1_ACK' ($ackLine -join ' | ')}else{Set-State 'UXC1_ACK_NOT_CAPTURED' 'device may reset before UXL1 frame carrying ACK'}
$deadline=(Get-Date).AddSeconds(60)
do {
 Start-Sleep -Milliseconds 500
 $cdc=@(Get-CimInstance Win32_PnPEntity | Where-Object {$_.PNPDeviceID -match '^USB\\VID_1209&PID_0001'})
} while($cdc.Count -gt 0 -and (Get-Date) -lt $deadline)
if($cdc.Count -gt 0){Set-State 'STOP_CDC_DID_NOT_DISAPPEAR'; throw 'CDC remained after UXC1 timeout'}
Set-State 'CDC_DISAPPEARED'
$deadline=(Get-Date).AddSeconds(240)
$androidLine=$null
do {
 Start-Sleep -Seconds 1
 $lines=@(adb devices -l 2>&1 | ForEach-Object {$_.ToString()} | Where-Object {$_.Trim()})
 $targets=@($lines | Where-Object {$_ -match '^'+[regex]::Escape($serial)+'\s+device\b'})
 $others=@($lines | Where-Object {$_ -match '^\S+\s+(device|recovery|sideload|unauthorized|offline)\b' -and $_ -notmatch '^'+[regex]::Escape($serial)+'\s+'})
 if($others.Count -gt 0){Set-State 'STOP_MULTIPLE_OR_WRONG_ADB' ($others -join ' | '); throw 'Multiple or wrong ADB devices'}
 if($targets.Count -eq 1){$androidLine=$targets[0];break}
} while((Get-Date) -lt $deadline)
if(-not $androidLine){Set-State 'STOP_ANDROID_ADB_TIMEOUT'; throw 'Android ADB did not appear'}
Set-State 'ANDROID_ADB' $androidLine
$product=((adb -s $serial shell getprop ro.product.device 2>&1 | ForEach-Object {$_.ToString()}) -join '').Trim()
if($product -ne 'cheeseburger'){Set-State 'STOP_ANDROID_PRODUCT_MISMATCH' "product=$product"; throw 'Android product mismatch'}
$bootCompleted=''
$deadline=(Get-Date).AddSeconds(90)
do {
 $bootCompleted=((adb -s $serial shell getprop sys.boot_completed 2>&1 | ForEach-Object {$_.ToString()}) -join '').Trim()
 if($bootCompleted -eq '1'){break}
 Start-Sleep -Seconds 2
} while((Get-Date) -lt $deadline)
Set-State 'ANDROID_BOOT_COMPLETED' "value=$bootCompleted product=$product"
Set-State 'REBOOT_RECOVERY_BEGIN'
adb -s $serial reboot recovery 2>&1 | ForEach-Object {Write-Output $_.ToString()}
$deadline=(Get-Date).AddSeconds(90)
$recoveryLine=$null
do {
 Start-Sleep -Seconds 1
 $lines=@(adb devices -l 2>&1 | ForEach-Object {$_.ToString()} | Where-Object {$_.Trim()})
 $targets=@($lines | Where-Object {$_ -match '^'+[regex]::Escape($serial)+'\s+recovery\b'})
 if($targets.Count -eq 1){$recoveryLine=$targets[0];break}
} while((Get-Date) -lt $deadline)
if(-not $recoveryLine){Set-State 'STOP_TWRP_TIMEOUT'; throw 'TWRP ADB did not appear'}
$twrpProduct=((adb -s $serial shell getprop ro.product.device 2>&1 | ForEach-Object {$_.ToString()}) -join '').Trim()
$twrpVersion=((adb -s $serial shell getprop ro.twrp.version 2>&1 | ForEach-Object {$_.ToString()}) -join '').Trim()
$twrpBuild=((adb -s $serial shell getprop ro.build.version.incremental 2>&1 | ForEach-Object {$_.ToString()}) -join '').Trim()
Set-State 'TWRP_READY' "$recoveryLine product=$twrpProduct version=$twrpVersion build=$twrpBuild"
Set-State 'DONE'