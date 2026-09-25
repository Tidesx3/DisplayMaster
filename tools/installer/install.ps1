# Installs DisplayMaster for the current user.
#
#  * Files      -> %LOCALAPPDATA%\Programs\DisplayMaster
#  * Engine     -> scheduled task \DisplayMaster\Engine: runs elevated at logon, so pen/touch
#                  reach admin windows and there's no UAC prompt afterwards
#  * App        -> starts in the tray at logon (HKCU Run), Start menu shortcut
#  * Firewall   -> inbound TCP for the engine on *Private* networks only (Wi-Fi mode)
#
# Needs administrator rights once (task + firewall rule); re-launches itself elevated.
param(
    [switch] $NoAutostart,
    # Set when re-launched elevated, so files land in the real user's profile.
    [string] $UserLocalAppData,
    [string] $UserAppData,
    [string] $UserName
)
$ErrorActionPreference = 'Stop'

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    # Pass the real user's profile paths along: an elevated shell might run as another account.
    $argList = "-ExecutionPolicy Bypass -File `"$PSCommandPath`" -UserLocalAppData `"$env:LOCALAPPDATA`" -UserAppData `"$env:APPDATA`" -UserName `"$env:USERDOMAIN\$env:USERNAME`""
    if ($NoAutostart) { $argList += ' -NoAutostart' }
    Start-Process powershell -Verb RunAs -ArgumentList $argList -Wait
    exit
}
$userLocal = if ($UserLocalAppData) { $UserLocalAppData } else { $env:LOCALAPPDATA }
$userRoaming = if ($UserAppData) { $UserAppData } else { $env:APPDATA }
$user = if ($UserName) { $UserName } else { "$env:USERDOMAIN\$env:USERNAME" }

$source = $PSScriptRoot
$dest = Join-Path $userLocal 'Programs\DisplayMaster'
Write-Host "Installing DisplayMaster to $dest"

# Stop running copies so files can be replaced.
Get-Process DisplayMaster, DisplayMasterHost -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
New-Item -ItemType Directory -Force $dest | Out-Null
Copy-Item "$source\*" $dest -Recurse -Force -Exclude 'install.ps1'

# Engine: elevated logon task (also started on demand by the app via schtasks /Run).
$action = New-ScheduledTaskAction -Execute "$dest\DisplayMasterHost.exe" -Argument "--log `"$userLocal\DisplayMaster\host.log`"" -WorkingDirectory $dest
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $user
$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -AllowStartIfOnBatteries `
    -DontStopIfGoingOnBatteries -MultipleInstances IgnoreNew -StartWhenAvailable
$taskPrincipal = New-ScheduledTaskPrincipal -UserId $user -RunLevel Highest -LogonType Interactive
Register-ScheduledTask -TaskPath '\DisplayMaster\' -TaskName 'Engine' -Action $action -Trigger $trigger `
    -Settings $settings -Principal $taskPrincipal -Force | Out-Null
if ($NoAutostart) { Disable-ScheduledTask -TaskPath '\DisplayMaster\' -TaskName 'Engine' | Out-Null }

# Wi-Fi: allow inbound connections to the engine on private networks only.
Get-NetFirewallRule -DisplayName 'DisplayMaster (Wi-Fi)' -ErrorAction SilentlyContinue | Remove-NetFirewallRule
New-NetFirewallRule -DisplayName 'DisplayMaster (Wi-Fi)' -Direction Inbound -Action Allow -Protocol TCP `
    -Program "$dest\DisplayMasterHost.exe" -Profile Private | Out-Null

# App: tray at logon + Start menu entry. (HKCU of the installing user.)
$runKey = "Registry::HKEY_USERS\$((New-Object Security.Principal.NTAccount($user)).Translate([Security.Principal.SecurityIdentifier]).Value)\Software\Microsoft\Windows\CurrentVersion\Run"
if (-not $NoAutostart) {
    Set-ItemProperty -Path $runKey -Name 'DisplayMaster' -Value "`"$dest\DisplayMaster.exe`" --tray"
}
$shell = New-Object -ComObject WScript.Shell
$lnk = $shell.CreateShortcut((Join-Path $userRoaming 'Microsoft\Windows\Start Menu\Programs\DisplayMaster.lnk'))
$lnk.TargetPath = "$dest\DisplayMaster.exe"
$lnk.WorkingDirectory = $dest
$lnk.IconLocation = "$dest\Assets\AppIcon.ico"
$lnk.Description = 'Use your Android tablet or phone as a second screen'
$lnk.Save()

Start-ScheduledTask -TaskPath '\DisplayMaster\' -TaskName 'Engine'
Write-Host 'Done. Open DisplayMaster from the Start menu.' -ForegroundColor Green
