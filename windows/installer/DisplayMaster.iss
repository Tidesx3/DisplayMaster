; DisplayMaster installer (Inno Setup 7). Built by tools\package.ps1 from dist\DisplayMaster.
;   ISCC /DAppVersion=0.1.0 /DSourceDir=..\..\dist\DisplayMaster DisplayMaster.iss
; Silent install on other machines:  DisplayMaster-Setup.exe /VERYSILENT [/TASKS="vdd,firewall"]
; In-app updates run it with /SILENT /relaunch=1 (keeps the previous choices, starts the app again).

#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\..\dist\DisplayMaster"
#endif

[Setup]
AppId={{8C1B7E2A-4F63-4D3B-9A3E-6E2B1D5C7A90}
AppName=DisplayMaster
AppVersion={#AppVersion}
AppVerName=DisplayMaster {#AppVersion}
AppPublisher=DisplayMaster contributors
AppComments=Use an Android tablet or phone as a second screen
DefaultDirName={autopf}\DisplayMaster
DefaultGroupName=DisplayMaster
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Windows 10 1809+: synthetic pen/touch injection and the built-in mDNS responder.
MinVersion=10.0.17763
OutputDir=..\..\dist
OutputBaseFilename=DisplayMaster-Setup-{#AppVersion}
SetupIconFile=..\ui\DisplayMaster.App\Assets\AppIcon.ico
UninstallDisplayIcon={app}\DisplayMaster.exe
UninstallDisplayName=DisplayMaster
WizardStyle=modern
Compression=lzma2/ultra64
SolidCompression=yes
CloseApplications=force
RestartApplications=no
LicenseFile=..\..\LICENSE

[Tasks]
Name: "vdd"; Description: "Install the virtual display driver (needed to extend the desktop)"; GroupDescription: "Components:"
Name: "firewall"; Description: "Allow devices on private Wi-Fi networks to connect"; GroupDescription: "Components:"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\DisplayMaster"; Filename: "{app}\DisplayMaster.exe"; Comment: "Use your Android tablet or phone as a second screen"

[Registry]
; Start at sign-in is a switch in the app (Settings; per user, HKCU). Earlier installers set it
; machine-wide: drop that entry. Uninstalling removes the per-user one.
Root: HKLM; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueName: "DisplayMaster"; Flags: deletevalue
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: none; ValueName: "DisplayMaster"; \
  Flags: uninsdeletevalue dontcreatekey
; Remember whether we installed the driver, so uninstall only removes what it added.
Root: HKLM; Subkey: "Software\DisplayMaster"; ValueType: dword; ValueName: "InstalledVdd"; ValueData: 1; \
  Flags: uninsdeletekey; Tasks: vdd

[Run]
Filename: "{app}\DisplayMasterHost.exe"; Parameters: "--install-vdd ""{app}\driver"" --log auto"; \
  StatusMsg: "Installing the virtual display driver (Windows may ask you to confirm)..."; Flags: runhidden waituntilterminated; Tasks: vdd
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\setup\setup-helpers.ps1"" -Action Install -AppDir ""{app}"" {code:FirewallSwitch}"; \
  StatusMsg: "Registering the DisplayMaster engine..."; Flags: runhidden waituntilterminated
Filename: "{app}\DisplayMaster.exe"; Description: "Open DisplayMaster"; Flags: postinstall nowait skipifsilent runasoriginaluser
Filename: "{app}\DisplayMaster.exe"; Flags: nowait runasoriginaluser; Check: RelaunchRequested

[UninstallRun]
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\setup\setup-helpers.ps1"" -Action Uninstall -AppDir ""{app}"""; \
  Flags: runhidden waituntilterminated; RunOnceId: "RemoveTaskAndFirewall"

[UninstallDelete]
Type: filesandordirs; Name: "{app}"

[Code]
function RelaunchRequested: Boolean;
begin
  Result := ExpandConstant('{param:relaunch|0}') = '1';
end;

function FirewallSwitch(Param: String): String;
begin
  if WizardIsTaskSelected('firewall') then Result := '-Firewall' else Result := '';
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Installed: Cardinal;
  Code: Integer;
begin
  if CurUninstallStep <> usUninstall then Exit;
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/IM DisplayMaster.exe /F', '', SW_HIDE, ewWaitUntilTerminated, Code);
  // The driver may be shared with other tools (e.g. game streaming): only remove it if we added it, and ask.
  if RegQueryDWordValue(HKLM, 'Software\DisplayMaster', 'InstalledVdd', Installed) and (Installed = 1) then
    if UninstallSilent or
       (MsgBox('Also remove the virtual display driver?' + #13#10 +
               'Keep it if other apps (for example game streaming) use virtual displays.',
               mbConfirmation, MB_YESNO) = IDYES) then
      Exec(ExpandConstant('{app}\DisplayMasterHost.exe'), '--uninstall-vdd', '', SW_HIDE, ewWaitUntilTerminated, Code);
end;
