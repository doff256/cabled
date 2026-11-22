; Cabled installer (D-17: Inno Setup, admin once). Built by windows/scripts/build-installer.ps1:
;   iscc installer\Cabled.iss /DAppVersion=0.1.0 /DSourceDir=<publish folder>
;
; Installs the app (self-contained .NET + WinUI), the engine and FFmpeg DLLs, and the virtual camera:
; the media source DLL is COM-registered in HKLM (Frame Server loads it as LOCAL SERVICE, so it
; lives in Program Files), then "Cabled Camera" is created for good (D-08) with cabled-vcam.
; Unsigned on purpose (D-25): SmartScreen shows "More info -> Run anyway".

#define AppName "Cabled"
#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\build\publish"
#endif

[Setup]
; Never change AppId: it's how upgrades find the installed copy.
AppId={{AAFE1706-C3ED-46AC-A08E-17812717E556}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=Cabled contributors
AppPublisherURL=https://github.com/doff256/cabled
AppSupportURL=https://github.com/doff256/cabled/issues
AppUpdatesURL=https://github.com/doff256/cabled/releases
DefaultDirName={autopf}\{#AppName}
DisableProgramGroupPage=yes
DisableDirPage=auto
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Windows 11 first (Q6): the Media Foundation virtual camera needs build 22000+.
MinVersion=10.0.22000
OutputBaseFilename=Cabled-Setup-{#AppVersion}
SetupIconFile=..\app\Cabled.App\Assets\Cabled.ico
UninstallDisplayIcon={app}\Cabled.exe
UninstallDisplayName={#AppName}
LicenseFile=..\LICENSE
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
; Restart Manager closes a running Cabled.exe (it's in the tray) before files are replaced.
CloseApplications=yes
RestartApplications=no
; The only per-user change is removing the app's own HKCU Run entry on uninstall.
UsedUserAreasWarning=no

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Excludes: "cabled_vcam_mf.dll,*.pdb"; Flags: ignoreversion recursesubdirs createallsubdirs
; The camera DLL: Frame Server may have it loaded, so it can be replaced at the next restart if needed.
Source: "{#SourceDir}\cabled_vcam_mf.dll"; DestDir: "{app}"; Flags: ignoreversion regserver restartreplace uninsrestartdelete

[Dirs]
; The camera DLL logs here; it runs as LOCAL SERVICE (a service account), so services need write access.
Name: "{commonappdata}\Cabled\logs"; Permissions: service-modify users-modify

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\Cabled.exe"

[Run]
; Replace any earlier "Cabled Camera" (dev installs, older versions) so there is exactly one.
Filename: "{app}\cabled-vcam.exe"; Parameters: "remove --all-users"; Flags: runhidden waituntilterminated; StatusMsg: "Adding Cabled Camera..."
Filename: "{app}\cabled-vcam.exe"; Parameters: "remove"; Flags: runhidden waituntilterminated
Filename: "{app}\cabled-vcam.exe"; Parameters: "register --all-users"; Flags: runhidden waituntilterminated
Filename: "{app}\Cabled.exe"; Description: "Open Cabled"; Flags: nowait postinstall skipifsilent runasoriginaluser

[UninstallRun]
Filename: "{app}\cabled-vcam.exe"; Parameters: "remove --all-users"; Flags: runhidden waituntilterminated; RunOnceId: "RemoveCameraAllUsers"
Filename: "{app}\cabled-vcam.exe"; Parameters: "remove"; Flags: runhidden waituntilterminated; RunOnceId: "RemoveCameraUser"

[Registry]
; Start at login is set by the app (HKCU Run, D-20); remove it with the app.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueName: "Cabled"; Flags: uninsdeletevalue dontcreatekey

[Code]
procedure StopService(const Name: String);
var
  Code: Integer;
begin
  { Fails harmlessly while an app has a camera open; restartreplace covers that case. }
  Exec(ExpandConstant('{sys}\net.exe'), 'stop ' + Name + ' /y', '', SW_HIDE, ewWaitUntilTerminated, Code);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  { Frame Server keeps the camera DLL loaded; stopping it lets the new one be copied in place. }
  StopService('FrameServerMonitor');
  StopService('FrameServer');
  Result := '';
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
  begin
    StopService('FrameServerMonitor');
    StopService('FrameServer');
  end;
  if (CurUninstallStep = usPostUninstall) and not UninstallSilent then
  begin
    if MsgBox('Also remove your Cabled settings and presets?', mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
      DelTree(ExpandConstant('{userappdata}\Cabled'), True, True, True);
  end;
end;
