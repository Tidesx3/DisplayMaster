# End-to-end engine test without any Android device: starts DisplayMasterHost on a spare
# port and connects simulated devices (tools/testclient) in several configurations.
# A second engine in --test-mode (own data folder and control pipe) checks encrypted
# Wi-Fi connections: pairing codes, remembered devices and old apps being turned away.
# Usage: powershell -ExecutionPolicy Bypass -File tools\integration_test.ps1 [-Config Debug|Release]
param([string] $Config = 'Debug', [int] $Port = 47899, [int] $SecurePort = 47898)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$hostExe = "$root\build\windows-x64\windows\host\$Config\DisplayMasterHost.exe"
$client = "$root\build\windows-x64\$Config\dm_testclient.exe"
foreach ($f in $hostExe, $client) { if (-not (Test-Path $f)) { throw "Build first: $f missing" } }

$failed = 0
function Check($name, [scriptblock] $test) {
    Write-Host "`n--- $name" -ForegroundColor Cyan
    try {
        & $test
        Write-Host 'ok' -ForegroundColor Green
    } catch {
        $script:failed++
        Write-Host "FAILED: $_" -ForegroundColor Red
    }
}
function Run-Client([string] $clientArgs) {
    $out = & $client @($clientArgs -split ' ') 2>&1 | Out-String
    Write-Host $out.TrimEnd()
    return @{ Output = $out; Code = $LASTEXITCODE }
}

# ---------------------------------------------------------------- USB-style (plain) connections
$log = Join-Path $env:TEMP 'dm-integration-host.log'
Remove-Item $log -ErrorAction SilentlyContinue
$engine = Start-Process $hostExe -ArgumentList "--port $Port --no-adb --no-input --test-frames --log `"$log`"" -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 2
try {
    $cases = @(
        @{ Name = 'single device, HEVC';               Args = '--clients 1 --seconds 4 --size 2800x1752 --codec hevc' },
        @{ Name = 'single device, H.264, phone size';   Args = '--clients 1 --seconds 4 --size 1080x2400 --codec h264' },
        @{ Name = 'three devices mirroring at once';    Args = '--clients 3 --seconds 5 --size 1600x1000' },
        @{ Name = 'USB device choosing encryption';     Args = '--clients 1 --seconds 3 --size 1280x800 --secure' }
    )
    foreach ($c in $cases) {
        Check $c.Name {
            $r = Run-Client "--port $Port $($c.Args)"
            if ($r.Code) { throw 'client failed' }
        }
    }
    Check 'window list; pulling a window needs extend mode' {
        $r = Run-Client "--port $Port --clients 1 --seconds 3 --size 1280x800 --windows"
        if ($r.Code) { throw 'client failed' }
        if ($r.Output -notmatch 'windows: (\d+) listed') { throw 'no window list' }
        if ($r.Output -notmatch 'pull last window: not extended') { throw 'a mirroring device must not move windows' }
    }
} finally {
    Stop-Process -Id $engine.Id -Force -ErrorAction SilentlyContinue
}

# ---------------------------------------------------------------- encrypted Wi-Fi (test mode)
$data = Join-Path $env:TEMP 'DisplayMaster-test'
Remove-Item $data -Recurse -Force -ErrorAction SilentlyContinue
$slog = Join-Path $env:TEMP 'dm-integration-secure.log'
Remove-Item $slog -ErrorAction SilentlyContinue
$engine = Start-Process $hostExe -ArgumentList "--port $SecurePort --no-adb --no-input --test-mode --log `"$slog`"" -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 2
$key = '7f' * 32  # fixed device key: the second connection must be recognized
try {
    Check 'Wi-Fi: new device pairs, both sides show the same code' {
        $r = Run-Client "--port $SecurePort --clients 1 --seconds 3 --secure --key $key"
        if ($r.Code) { throw 'client failed' }
        if ($r.Output -notmatch 'pairing code (\d{3} \d{3})') { throw 'client did not report a pairing code' }
        $code = $Matches[1]
        if ($r.Output -match 'already paired') { throw 'new device was not asked to pair' }
        $engineCodes = Select-String -Path $slog -Pattern 'approved pairing code (\d{3} \d{3})' | ForEach-Object { $_.Matches[0].Groups[1].Value }
        if ($engineCodes -notcontains $code) { throw "engine code(s) '$engineCodes' != device code '$code'" }
        Write-Host "codes match: $code"
    }
    Check 'Wi-Fi: paired device reconnects without approval' {
        $before = @(Select-String -Path $slog -Pattern 'approved pairing code').Count
        $r = Run-Client "--port $SecurePort --clients 1 --seconds 3 --secure --key $key"
        if ($r.Code) { throw 'client failed' }
        if ($r.Output -notmatch 'already paired') { throw 'paired device was asked again' }
        $after = @(Select-String -Path $slog -Pattern 'approved pairing code').Count
        if ($after -ne $before) { throw 'engine approved again' }
    }
    Check 'Wi-Fi: video over UDP' {
        $r = Run-Client "--port $SecurePort --clients 1 --seconds 3 --secure --key $key --udp"
        if ($r.Code) { throw 'client failed' }
    }
    Check 'Wi-Fi: UDP with 10 % packet loss (parity repairs, keyframes recover)' {
        $r = Run-Client "--port $SecurePort --clients 1 --seconds 5 --secure --key $key --udp --loss 10"
        if ($r.Code) { throw 'client failed' }
    }
    Check 'Wi-Fi: lost frames repaired by recovery frames, not keyframes (HEVC)' {
        $r = Run-Client "--port $SecurePort --clients 1 --seconds 8 --secure --key $key --udp --loss 15 --codec hevc"
        if ($r.Code) { throw 'client failed' }
        if ($r.Output -notmatch 'repaired by (\d+) recovery frames') { throw 'no repair summary' }
        if ([int]$Matches[1] -eq 0) { throw 'no recovery frames (NVENC only; an AMD PC falls back to keyframes)' }
    }
    Check 'Wi-Fi: UDP blocked -> falls back to TCP' {
        $r = Run-Client "--port $SecurePort --clients 1 --seconds 5 --secure --key $key --udp --udp-blocked"
        if ($r.Code) { throw 'client failed' }
    }
    Check 'Wi-Fi: app without encryption is told to update' {
        $r = Run-Client "--port $SecurePort --clients 1 --seconds 2"
        if (-not $r.Code) { throw 'plain connection was accepted' }
        if ($r.Output -notmatch 'Update the DisplayMaster app') { throw 'no update hint' }
    }
} finally {
    Stop-Process -Id $engine.Id -Force -ErrorAction SilentlyContinue
    Remove-Item $data -Recurse -Force -ErrorAction SilentlyContinue
}

if ($failed) { Write-Host "`n$failed case(s) failed - engine logs: $log, $slog" -ForegroundColor Red; exit 1 }
Write-Host "`nAll integration cases passed." -ForegroundColor Green
