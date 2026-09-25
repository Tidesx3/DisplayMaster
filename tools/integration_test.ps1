# End-to-end engine test without any Android device: starts DisplayMasterHost on a spare
# port and connects simulated devices (tools/testclient) in several configurations.
# Usage: powershell -ExecutionPolicy Bypass -File tools\integration_test.ps1 [-Config Debug|Release]
param([string] $Config = 'Debug', [int] $Port = 47899)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$hostExe = "$root\build\windows-x64\windows\host\$Config\DisplayMasterHost.exe"
$client = "$root\build\windows-x64\$Config\dm_testclient.exe"
foreach ($f in $hostExe, $client) { if (-not (Test-Path $f)) { throw "Build first: $f missing" } }

$log = Join-Path $env:TEMP 'dm-integration-host.log'
$engine = Start-Process $hostExe -ArgumentList "--port $Port --no-adb --no-input --log `"$log`"" -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 2
$failed = 0
try {
    $cases = @(
        @{ Name = 'single device, HEVC';               Args = '--clients 1 --seconds 4 --size 2800x1752 --codec hevc' },
        @{ Name = 'single device, H.264, phone size';   Args = '--clients 1 --seconds 4 --size 1080x2400 --codec h264' },
        @{ Name = 'three devices mirroring at once';    Args = '--clients 3 --seconds 5 --size 1600x1000' }
    )
    foreach ($c in $cases) {
        Write-Host "`n--- $($c.Name)" -ForegroundColor Cyan
        & $client --port $Port @($c.Args -split ' ')
        if ($LASTEXITCODE) { $failed++ ; Write-Host 'FAILED' -ForegroundColor Red } else { Write-Host 'ok' -ForegroundColor Green }
    }
} finally {
    Stop-Process -Id $engine.Id -Force -ErrorAction SilentlyContinue
}
if ($failed) { Write-Host "`n$failed case(s) failed - engine log: $log" -ForegroundColor Red; exit 1 }
Write-Host "`nAll integration cases passed." -ForegroundColor Green
