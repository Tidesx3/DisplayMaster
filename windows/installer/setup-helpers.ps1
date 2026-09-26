# Called by DisplayMaster-Setup.exe (elevated) to register what Inno Setup can't express directly.
#
#   -Action Install  : on-demand task \DisplayMaster\Engine (no trigger: the DisplayMaster app
#                      runs it, so the engine starts and stops with the app; elevated for the
#                      installing user: pen/touch reach admin windows, no UAC prompt later)
#                      [+ firewall rule for Wi-Fi when -Firewall]
#   -Action Uninstall: removes both
param(
    [Parameter(Mandatory)] [ValidateSet('Install', 'Uninstall')] [string] $Action,
    [Parameter(Mandatory)] [string] $AppDir,
    [switch] $Firewall
)
$ErrorActionPreference = 'Stop'
$taskPath = '\DisplayMaster\'
$taskName = 'Engine'
$ruleName = 'DisplayMaster (Wi-Fi)'
$engine = Join-Path $AppDir 'DisplayMasterHost.exe'

function Remove-Registrations {
    Unregister-ScheduledTask -TaskPath $taskPath -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue
    Get-NetFirewallRule -DisplayName $ruleName -ErrorAction SilentlyContinue | Remove-NetFirewallRule
}

if ($Action -eq 'Uninstall') {
    Get-Process DisplayMasterHost -ErrorAction SilentlyContinue | Stop-Process -Force
    Remove-Registrations
    exit 0
}

Remove-Registrations
$user = [Security.Principal.WindowsIdentity]::GetCurrent().Name
$task = New-ScheduledTask `
    -Action (New-ScheduledTaskAction -Execute $engine -Argument '--log auto' -WorkingDirectory $AppDir) `
    -Principal (New-ScheduledTaskPrincipal -UserId $user -RunLevel Highest -LogonType Interactive) `
    -Settings (New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -AllowStartIfOnBatteries `
        -DontStopIfGoingOnBatteries -MultipleInstances IgnoreNew -Priority 4)
Register-ScheduledTask -TaskPath $taskPath -TaskName $taskName -InputObject $task -Force | Out-Null

if ($Firewall) {
    # Private networks only: devices at home / work can connect, cafe Wi-Fi can't.
    New-NetFirewallRule -DisplayName $ruleName -Direction Inbound -Action Allow -Protocol TCP `
        -Program $engine -Profile Private | Out-Null
}
