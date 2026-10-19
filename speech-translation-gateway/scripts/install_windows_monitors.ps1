$ErrorActionPreference = 'Stop'
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$userName = [Security.Principal.WindowsIdentity]::GetCurrent().Name
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $userName
$principal = New-ScheduledTaskPrincipal -UserId $userName -LogonType Interactive -RunLevel Limited
$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1)

foreach ($item in @(
    @{ Name = 'OrangePiSpeechLiveCollector'; File = 'collect_live_board.ps1' },
    @{ Name = 'OrangePiSpeechHealthMonitor'; File = 'monitor_board.ps1' }
)) {
    $path = Join-Path $scriptDir $item.File
    $arguments = '-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File "{0}"' -f $path
    $action = New-ScheduledTaskAction -Execute (Get-Command powershell.exe).Source -Argument $arguments
    Register-ScheduledTask -TaskName $item.Name -Action $action -Trigger $trigger `
        -Principal $principal -Settings $settings -Description 'Orange Pi speech monitoring; records are written to E drive' -Force | Out-Null
    Write-Output "registered $($item.Name)"
}
