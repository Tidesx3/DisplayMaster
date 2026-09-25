; DisplayMaster installer (Inno Setup 7). Built by tools\package.ps1 from dist\DisplayMaster.
;   ISCC /DAppVersion=0.1.0 /DSourceDir=..\..\dist\DisplayMaster DisplayMaster.iss
; Silent install on other machines:  DisplayMaster-Setup.exe /VERYSILENT [/TASKS="vdd,autostart,firewall"]

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
Name: "autostart"; Description: "Start DisplayMaster when I sign in"; GroupDescription: "Components:"
Name: "firewall"; Description: "Allow devices on private Wi-Fi networks to connect"; GroupDescription: "Components:"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\DisplayMaster"; Filename: "{app}\DisplayMaster.exe"; Comment: "Use your Android tablet or phone as a second screen"

[Registry]
; Tray app at sign-in (the engine itself is started by the logon task).
Root: HKA; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "DisplayMaster"; \
  ValueData: """{app}\DisplayMaster.exe"" --tray"; Flags: uninsdeletevalue; Tasks: autostart
; Remember whether we installed the driver, so uninstall only removes what it added.
Root: HKLM; Subkey: "Software\DisplayMaster"; ValueType: dword; ValueName: "InstalledVdd"; ValueData: 1; \
  Flags: uninsdeletekey; Tasks: vdd

[Run]
Filename: "{app}\DisplayMasterHost.exe"; Parameters: "--install-vdd ""{app}\driver"" --log auto"; \
  StatusMsg: "Installing the virtual display driver (Windows may ask you to confirm)..."; Flags: runhidden waituntilterminated; Tasks: vdd
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\setup\setup-helpers.ps1"" -Action Install -AppDir ""{app}"" {code:FirewallSwitch}"; \
  StatusMsg: "Registering the DisplayMaster engine..."; Flags: runhidden waituntilterminated
Filename: "{app}\DisplayMaster.exe"; Description: "Open DisplayMaster"; Flags: postinstall nowait skipifsilent runasoriginaluser

[UninstallRun]
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\setup\setup-helpers.ps1"" -Action Uninstall -AppDir ""{app}"""; \
  Flags: runhidden waituntilterminated; RunOnceId: "RemoveTaskAndFirewall"

[UninstallDelete]
Type: filesandordirs; Name: "{app}"

[Code]
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
