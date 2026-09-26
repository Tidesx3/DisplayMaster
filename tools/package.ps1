# Builds everything in Release and produces:
#
#   dist\DisplayMaster-Setup-<version>.exe   single-file installer (Inno Setup) - use this
#   dist\DisplayMaster\                      the same files, unpacked:
#     DisplayMaster.exe (+ self-contained .NET/WinUI runtime)   tray + settings app
#     DisplayMasterHost.exe                                      engine
#     platform-tools\adb.exe (+ dlls)                            USB connections
#     android\DisplayMaster.apk                                  auto-installed on plugged-in devices
#     driver\                                                    virtual display driver (signed, MIT)
#     setup\setup-helpers.ps1                                    used by the installer
#
# Usage: powershell -ExecutionPolicy Bypass -File tools\package.ps1 [-Version 0.1.0] [-SkipAndroid] [-Publish]
#   -Publish  also creates GitHub release v<Version> with the installer (needs `gh auth login`).
#             Installed apps find it through their update check (Services\UpdateService.cs).
param(
    [string] $Out = (Join-Path $PSScriptRoot '..\dist\DisplayMaster'),
    [string] $Version = '0.1.0',
    [switch] $SkipAndroid,
    [switch] $Publish
)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
Set-Location $root  # CMake presets are resolved from the working directory

# Virtual Display Driver release bundled in the installer (pinned + verified).
$VddUrl = 'https://github.com/VirtualDrivers/Virtual-Display-Driver/releases/download/25.7.23/VirtualDisplayDriver-x86.Driver.Only.zip'
$VddSha256 = 'e24210692b442b39af763536330ce78b423f19342b7a7792c26de3944e418b3a'

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

function Find-Iscc {
    @((Get-Command ISCC.exe -ErrorAction SilentlyContinue).Source,
      "$env:LOCALAPPDATA\Programs\Inno Setup 7\ISCC.exe",
      "${env:ProgramFiles(x86)}\Inno Setup 7\ISCC.exe",
      "$env:ProgramFiles\Inno Setup 7\ISCC.exe",
      "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe") | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
}

function Get-Zip($url, $name) {
    $zip = Join-Path $env:TEMP "$name.zip"
    $dir = Join-Path $env:TEMP $name
    Invoke-WebRequest -UseBasicParsing $url -OutFile $zip
    if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
    Expand-Archive $zip $dir
    return @{ Zip = $zip; Dir = $dir }
}

if (Test-Path $Out) { Remove-Item $Out -Recurse -Force }
New-Item -ItemType Directory -Force $Out | Out-Null

Step 'Engine (C++, Release)'
$cmake = Find-CMake
& $cmake --preset windows-x64 "-DDM_VERSION=$Version" | Out-Null
& $cmake --build --preset release --target DisplayMasterHost
if ($LASTEXITCODE) { throw 'engine build failed' }
Copy-Item "$root\build\windows-x64\windows\host\Release\DisplayMasterHost.exe" $Out

Step 'App (WinUI 3, self-contained)'
$dotnet = Find-Dotnet
$env:DOTNET_ROOT = Split-Path $dotnet
& $dotnet publish "$root\windows\ui\DisplayMaster.App\DisplayMaster.App.csproj" -c Release -r win-x64 `
    -p:Platform=x64 "-p:Version=$Version" --self-contained -o $Out -nologo -v quiet
if ($LASTEXITCODE) { throw 'app publish failed' }
# Without its PRI (compiled XAML) the app crashes at startup - never ship that.
if (-not (Test-Path "$Out\DisplayMaster.pri")) { throw 'app publish is missing DisplayMaster.pri' }

if (-not $SkipAndroid) {
    Step 'Android app (release APK)'
    if (-not $env:JAVA_HOME) { $env:JAVA_HOME = "$env:ProgramFiles\Android\Android Studio\jbr" }
    Push-Location "$root\android"
    try {
        & .\gradlew.bat assembleRelease --console=plain -q "-PdmVersion=$Version"
        if ($LASTEXITCODE) { throw 'android build failed' }
    } finally { Pop-Location }
    New-Item -ItemType Directory -Force "$Out\android" | Out-Null
    Copy-Item "$root\android\app\build\outputs\apk\release\app-release.apk" "$Out\android\DisplayMaster.apk"

    # Native code looks up Kotlin callbacks by name; one stripped by R8 crashes the app at launch.
    $jni = Select-String -Path "$root\android\app\src\main\cpp\jni_bridge.cpp" -Pattern 'GetMethodID\(cls, "(\w+)"' -AllMatches |
        ForEach-Object { $_.Matches } | ForEach-Object { $_.Groups[1].Value }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead("$Out\android\DisplayMaster.apk")
    $dex = ''
    foreach ($entry in $zip.Entries | Where-Object Name -like 'classes*.dex') {
        $reader = New-Object IO.StreamReader($entry.Open(), [Text.Encoding]::GetEncoding(28591))
        $dex += $reader.ReadToEnd()
        $reader.Dispose()
    }
    $zip.Dispose()
    $missing = @($jni | Where-Object { -not $dex.Contains($_) })
    if (-not $jni) { throw 'found no JNI callbacks in jni_bridge.cpp - update this check' }
    if ($missing) { throw "release APK lost JNI callbacks: $($missing -join ', ') - keep them in android\app\proguard-rules.pro" }
}

Step 'Android platform-tools (adb)'
$pt = Get-Zip 'https://dl.google.com/android/repository/platform-tools-latest-windows.zip' 'dm-platform-tools'
New-Item -ItemType Directory -Force "$Out\platform-tools" | Out-Null
foreach ($f in 'adb.exe', 'AdbWinApi.dll', 'AdbWinUsbApi.dll', 'NOTICE.txt') {
    Copy-Item (Join-Path $pt.Dir "platform-tools\$f") "$Out\platform-tools\" -ErrorAction SilentlyContinue
}
Remove-Item $pt.Zip, $pt.Dir -Recurse -Force

Step 'Virtual display driver'
$vdd = Get-Zip $VddUrl 'dm-vdd'
$hash = (Get-FileHash $vdd.Zip -Algorithm SHA256).Hash.ToLower()
if ($hash -ne $VddSha256) { throw "Driver download hash mismatch ($hash)" }
New-Item -ItemType Directory -Force "$Out\driver" | Out-Null
Copy-Item (Join-Path $vdd.Dir 'VirtualDisplayDriver\*') "$Out\driver\"
Remove-Item $vdd.Zip, $vdd.Dir -Recurse -Force

Step 'Setup files'
New-Item -ItemType Directory -Force "$Out\setup" | Out-Null
Copy-Item "$root\windows\installer\setup-helpers.ps1" "$Out\setup\"
Copy-Item "$root\README.md", "$root\LICENSE" $Out

Step 'Installer (Inno Setup)'
$iscc = Find-Iscc
if ($iscc) {
    & $iscc /Q "/DAppVersion=$Version" "/DSourceDir=$((Resolve-Path $Out).Path)" "$root\windows\installer\DisplayMaster.iss"
    if ($LASTEXITCODE) { throw 'installer build failed' }
    $setup = Get-Item "$root\dist\DisplayMaster-Setup-$Version.exe"
    Write-Host "`nInstaller: $($setup.FullName) ($([math]::Round($setup.Length / 1MB, 1)) MB)" -ForegroundColor Green
} else {
    Write-Warning 'Inno Setup not found - skipped the installer (winget install JrSoftware.InnoSetup)'
}
$size = [math]::Round(((Get-ChildItem $Out -Recurse | Measure-Object Length -Sum).Sum / 1MB), 1)
Write-Host "Unpacked files: $((Resolve-Path $Out).Path) ($size MB)" -ForegroundColor Green

if ($Publish) {
    Step "GitHub release v$Version"
    if (-not $setup) { throw 'no installer to publish' }
    # GitHub records a SHA-256 digest per asset; the app refuses updates without one.
    & gh release create "v$Version" $setup.FullName --title "DisplayMaster $Version" --generate-notes
    if ($LASTEXITCODE) { throw 'gh release create failed' }
}
