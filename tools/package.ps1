# Builds everything in Release and assembles a ready-to-install folder:
#
#   dist\DisplayMaster\
#     DisplayMaster.exe (+ self-contained .NET/WinUI runtime)   tray + settings app
#     DisplayMasterHost.exe                                      engine
#     platform-tools\adb.exe (+ dlls)                            USB connections
#     android\DisplayMaster.apk                                  auto-installed on plugged-in devices
#     install.ps1 / uninstall.ps1
#
# Usage: powershell -ExecutionPolicy Bypass -File tools\package.ps1 [-SkipAndroid]
param(
    [string] $Out = (Join-Path $PSScriptRoot '..\dist\DisplayMaster'),
    [switch] $SkipAndroid
)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')

function Step($text) { Write-Host "`n==> $text" -ForegroundColor Cyan }

function Find-CMake {
    $cmd = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $bundled = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    if (Test-Path $bundled) { return $bundled }
    throw 'CMake not found - install Visual Studio 2022 with the C++ workload.'
}

function Find-Dotnet {
    foreach ($candidate in @((Get-Command dotnet -ErrorAction SilentlyContinue).Source,
                             "$env:LOCALAPPDATA\Microsoft\dotnet\dotnet.exe",
                             "$env:ProgramFiles\dotnet\dotnet.exe")) {
        if ($candidate -and (Test-Path $candidate)) {
            $sdks = & $candidate --list-sdks 2>$null
            if ($sdks -match '^10\.') { return $candidate }
        }
    }
    throw '.NET 10 SDK not found - https://dotnet.microsoft.com/download'
}

if (Test-Path $Out) { Remove-Item $Out -Recurse -Force }
New-Item -ItemType Directory -Force $Out | Out-Null

Step 'Engine (C++, Release)'
$cmake = Find-CMake
& $cmake --preset windows-x64 | Out-Null
& $cmake --build --preset release --target DisplayMasterHost
if ($LASTEXITCODE) { throw 'engine build failed' }
Copy-Item "$root\build\windows-x64\windows\host\Release\DisplayMasterHost.exe" $Out

Step 'App (WinUI 3, self-contained)'
$dotnet = Find-Dotnet
$env:DOTNET_ROOT = Split-Path $dotnet
& $dotnet publish "$root\windows\ui\DisplayMaster.App\DisplayMaster.App.csproj" -c Release -r win-x64 `
    -p:Platform=x64 --self-contained -o $Out -nologo -v quiet
if ($LASTEXITCODE) { throw 'app publish failed' }

if (-not $SkipAndroid) {
    Step 'Android app (release APK)'
    if (-not $env:JAVA_HOME) { $env:JAVA_HOME = "$env:ProgramFiles\Android\Android Studio\jbr" }
    Push-Location "$root\android"
    try {
        & .\gradlew.bat assembleRelease --console=plain -q
        if ($LASTEXITCODE) { throw 'android build failed' }
    } finally { Pop-Location }
    New-Item -ItemType Directory -Force "$Out\android" | Out-Null
    Copy-Item "$root\android\app\build\outputs\apk\release\app-release.apk" "$Out\android\DisplayMaster.apk"
}

Step 'Android platform-tools (adb)'
$zip = Join-Path $env:TEMP 'dm-platform-tools.zip'
Invoke-WebRequest -UseBasicParsing 'https://dl.google.com/android/repository/platform-tools-latest-windows.zip' -OutFile $zip
$extract = Join-Path $env:TEMP 'dm-platform-tools'
if (Test-Path $extract) { Remove-Item $extract -Recurse -Force }
Expand-Archive $zip $extract
New-Item -ItemType Directory -Force "$Out\platform-tools" | Out-Null
foreach ($f in 'adb.exe', 'AdbWinApi.dll', 'AdbWinUsbApi.dll', 'NOTICE.txt') {
    Copy-Item (Join-Path $extract "platform-tools\$f") "$Out\platform-tools\" -ErrorAction SilentlyContinue
}
Remove-Item $zip, $extract -Recurse -Force

Step 'Installer scripts'
Copy-Item "$PSScriptRoot\installer\install.ps1", "$PSScriptRoot\installer\uninstall.ps1" $Out
Copy-Item "$root\README.md" $Out

$size = [math]::Round(((Get-ChildItem $Out -Recurse | Measure-Object Length -Sum).Sum / 1MB), 1)
Write-Host "`nPackaged to $((Resolve-Path $Out).Path) ($size MB)." -ForegroundColor Green
Write-Host 'Install with: powershell -ExecutionPolicy Bypass -File install.ps1'
