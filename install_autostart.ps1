# ติดตั้ง/ถอนการรัน bridge_server.js อัตโนมัติแบบ background ทุกครั้งที่ล็อกอิน Windows
#   ติดตั้ง:  powershell -ExecutionPolicy Bypass -File install_autostart.ps1
#   ถอนออก:  powershell -ExecutionPolicy Bypass -File install_autostart.ps1 -Uninstall
param([switch]$Uninstall)

$TaskName = 'AIToken Bridge Server'
$Vbs = Join-Path $PSScriptRoot 'start_bridge_hidden.vbs'

if ($Uninstall) {
    Stop-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
    Write-Host "Removed scheduled task '$TaskName'."
    return
}

$action = New-ScheduledTaskAction -Execute 'wscript.exe' -Argument "`"$Vbs`"" -WorkingDirectory $PSScriptRoot
$trigger = New-ScheduledTaskTrigger -AtLogOn -User "$env:USERDOMAIN\$env:USERNAME"
$settings = New-ScheduledTaskSettingsSet `
    -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -ExecutionTimeLimit ([TimeSpan]::Zero) `
    -RestartCount 999 -RestartInterval (New-TimeSpan -Minutes 1) `
    -StartWhenAvailable `r
    -MultipleInstances IgnoreNew
$principal = New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" -LogonType Interactive -RunLevel Limited

Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger `
    -Settings $settings -Principal $principal -Force | Out-Null

Write-Host "Registered scheduled task '$TaskName' (runs at logon, hidden)."
Write-Host "Start now:  Start-ScheduledTask -TaskName '$TaskName'"
