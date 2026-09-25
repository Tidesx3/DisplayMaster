# Removes everything install.ps1 set up. Re-launches itself elevated.
# -Purge also deletes settings and the allowed-devices list.
param(
    [switch] $Purge,
    [string] $UserLocalAppData,
    [string] $UserAppData,
    [string] $UserName
)
$ErrorActionPreference = 'Continue'

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $argList = "-ExecutionPolicy Bypass -File `"$PSCommandPath`" -UserLocalAppData `"$env:LOCALAPPDATA`" -UserAppData `"$env:APPDATA`" -UserName `"$env:USERDOMAIN\$env:USERNAME`""
    if ($Purge) { $argList += ' -Purge' }
    Start-Process powershell -Verb RunAs -Wait -ArgumentList $argList
    exit
}
$userLocal = if ($UserLocalAppData) { $UserLocalAppData } else { $env:LOCALAPPDATA }
$userRoaming = if ($UserAppData) { $UserAppData } else { $env:APPDATA }
$user = if ($UserName) { $UserName } else { "$env:USERDOMAIN\$env:USERNAME" }

Get-Process DisplayMaster, DisplayMasterHost -ErrorAction SilentlyContinue | Stop-Process -Force
Unregister-ScheduledTask -TaskPath '\DisplayMaster\' -TaskName 'Engine' -Confirm:$false -ErrorAction SilentlyContinue
Get-NetFirewallRule -DisplayName 'DisplayMaster (Wi-Fi)' -ErrorAction SilentlyContinue | Remove-NetFirewallRule
$sid = (New-Object Security.Principal.NTAccount($user)).Translate([Security.Principal.SecurityIdentifier]).Value
Remove-ItemProperty -Path "Registry::HKEY_USERS\$sid\Software\Microsoft\Windows\CurrentVersion\Run" -Name 'DisplayMaster' -ErrorAction SilentlyContinue
Remove-Item (Join-Path $userRoaming 'Microsoft\Windows\Start Menu\Programs\DisplayMaster.lnk') -ErrorAction SilentlyContinue
Remove-Item (Join-Path $userLocal 'Programs\DisplayMaster') -Recurse -Force -ErrorAction SilentlyContinue
# Settings and allowed devices live in %LOCALAPPDATA%\DisplayMaster; kept unless -Purge.
if ($Purge) { Remove-Item (Join-Path $userLocal 'DisplayMaster') -Recurse -Force -ErrorAction SilentlyContinue }
Write-Host 'DisplayMaster was removed.' -ForegroundColor Green
