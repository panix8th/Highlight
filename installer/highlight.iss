; Highlight installer (Inno Setup 6). Built by CI from dist\Highlight:
;   iscc /DAppVersion=0.1.2 installer\highlight.iss
; Installs per user by default (no admin prompt); "all users" is offered as an option.
; Clips (Videos\Highlight) and settings (%APPDATA%\Highlight) are never touched.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif

[Setup]
AppId={{6C1F3B9E-4F2A-4C8D-9B7E-2D5A8E41C3F7}
AppName=Highlight
AppVersion={#AppVersion}
AppVerName=Highlight {#AppVersion}
AppPublisher=Highlight contributors
AppPublisherURL=https://github.com/panix8th/Highlight
AppSupportURL=https://github.com/panix8th/Highlight/issues
AppUpdatesURL=https://github.com/panix8th/Highlight/releases
DefaultDirName={autopf}\Highlight
DisableProgramGroupPage=yes
DisableDirPage=auto
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.19041
OutputDir=..\dist
OutputBaseFilename=Highlight-{#AppVersion}-setup
SetupIconFile=..\assets\icons\highlight.ico
UninstallDisplayIcon={app}\Highlight.exe
UninstallDisplayName=Highlight
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
CloseApplications=no
VersionInfoVersion={#AppVersion}
VersionInfoProductName=Highlight

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; Flags: unchecked

[Files]
Source: "..\dist\Highlight\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\Highlight"; Filename: "{app}\Highlight.exe"
Name: "{autodesktop}\Highlight"; Filename: "{app}\Highlight.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\Highlight.exe"; Description: "Start Highlight"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{sys}\taskkill.exe"; Parameters: "/F /IM highlight-ui.exe"; Flags: runhidden; RunOnceId: "StopUI"
Filename: "{sys}\taskkill.exe"; Parameters: "/F /IM highlight.exe"; Flags: runhidden; RunOnceId: "StopCore"

[Registry]
; "Start with Windows" entry written by the app itself: remove it on uninstall.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: none; ValueName: "Highlight"; Flags: uninsdeletevalue

[Code]
{ An upgrade replaces files the running app holds open: stop it first. }
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Code: Integer;
begin
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM highlight-ui.exe', '', SW_HIDE, ewWaitUntilTerminated, Code);
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM highlight.exe', '', SW_HIDE, ewWaitUntilTerminated, Code);
  Result := '';
end;
