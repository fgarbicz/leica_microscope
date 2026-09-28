; Inno Setup script for DM Imaging (build with tools\make_installer.ps1)
;   ISCC /DAppVersion=1.1.2 /DArch=x64 /DStageDir=<staged files> /DOutDir=<output> installer\DMImaging.iss

#ifndef AppVersion
  #error AppVersion is not defined - build the installer with tools\make_installer.ps1
#endif
#ifndef Arch
  #define Arch "x64"
#endif
#ifndef StageDir
  #define StageDir "..\build\package\app"
#endif
#ifndef RedistDir
  #define RedistDir "..\build\package\redist"
#endif
#ifndef OutDir
  #define OutDir "..\dist"
#endif

#define AppName "DM Imaging"
#define AppExe "DMImaging.exe"

[Setup]
AppId={{6F1C2A57-3E0B-4B8E-9C0D-5A1D2E7B9F41}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=DM Imaging
AppComments=Image acquisition and analysis for the Leica DM2000 microscope with the DMC6200 camera
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
; the camera driver needs administrator rights, and the app is shared by all users of a lab PC
PrivilegesRequired=admin
#if Arch == "arm64"
ArchitecturesAllowed=arm64
ArchitecturesInstallIn64BitMode=arm64
OutputBaseFilename=DMImaging-Setup-{#AppVersion}-arm64
#else
; x64compatible also admits Windows on ARM, which runs the x64 build under emulation
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputBaseFilename=DMImaging-Setup-{#AppVersion}
#endif
MinVersion=10.0
OutputDir={#OutDir}
SetupIconFile=..\resources\app.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
; a restart only when the Visual C++ runtime asks for one ([Code]): by default Setup
; also asks whenever anything queued a file operation meanwhile, on every upgrade
RestartIfNeededByRun=no
RestartApplications=no
VersionInfoVersion={#AppVersion}
VersionInfoDescription={#AppName} setup

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Shortcuts:"
Name: "installdriver"; Description: "Install the &camera driver (Leica DMC6200 USB camera)"; GroupDescription: "Camera:"

[InstallDelete]
; earlier per-user test installation (install.ps1) and its shortcuts
Type: filesandordirs; Name: "{localappdata}\Programs\DM Imaging"
Type: files; Name: "{userdesktop}\DM Imaging.lnk"
Type: files; Name: "{userprograms}\DM Imaging.lnk"
; driver catalog is generated and signed on each PC
Type: files; Name: "{app}\driver\*.cat"

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#RedistDir}\vc_redist.{#Arch}.exe"; Flags: dontcopy

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"; Comment: "Leica DM2000 microscope camera"
Name: "{autoprograms}\{#AppName} User Guide"; Filename: "{app}\docs\USER_GUIDE.html"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon; Comment: "Leica DM2000 microscope camera"

[Run]
Filename: "{app}\{#AppExe}"; Description: "Start {#AppName} now"; Flags: postinstall nowait skipifsilent

[UninstallRun]
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\driver\install_driver.ps1"" -Uninstall"; \
  Flags: runhidden waituntilterminated; RunOnceId: "RemoveCameraDriver"

[UninstallDelete]
Type: files; Name: "{app}\driver\*.cat"
Type: files; Name: "{app}\driver\install_driver.log"

[Code]
var
  VcRuntimeNeedsRestart: Boolean;

// Microsoft Visual C++ runtime: a no-op when a current version is installed
// (1638 = newer present); 3010 = installed, restart needed.
procedure InstallVcRuntime();
var
  ResultCode: Integer;
begin
  WizardForm.StatusLabel.Caption := 'Installing the Microsoft Visual C++ runtime...';
  ExtractTemporaryFile('vc_redist.{#Arch}.exe');
  if Exec(ExpandConstant('{tmp}\vc_redist.{#Arch}.exe'), '/install /quiet /norestart', '',
          SW_HIDE, ewWaitUntilTerminated, ResultCode) then begin
    Log('Visual C++ runtime exit code: ' + IntToStr(ResultCode));
    VcRuntimeNeedsRestart := ResultCode = 3010;
  end else
    Log('Visual C++ runtime installer could not be started: ' + SysErrorMessage(ResultCode));
end;

function NeedRestart(): Boolean;
begin
  Result := VcRuntimeNeedsRestart;
end;

// The camera driver is installed here rather than in [Run], which ignores exit
// codes: a driver that Windows refused would otherwise go unnoticed until the
// application reports that no camera was found.
procedure InstallCameraDriver();
var
  Script, LogFile: String;
  ResultCode: Integer;
begin
  Script := ExpandConstant('{app}\driver\install_driver.ps1');
  LogFile := ExpandConstant('{app}\driver\install_driver.log');
  WizardForm.StatusLabel.Caption := 'Installing the camera driver...';
  if not Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'),
              '-NoProfile -ExecutionPolicy Bypass -Command "& ''' + Script + ''' *> ''' + LogFile + '''; exit $LASTEXITCODE"',
              '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
    ResultCode := -1;
  Log('Camera driver installation exit code: ' + IntToStr(ResultCode));
  if ResultCode <> 0 then
    SuppressibleMsgBox('DM Imaging is installed, but Windows did not accept the camera driver.' + #13#10#13#10 +
                       'The details are in ' + LogFile + '.' + #13#10#13#10 +
                       'The program works without it (for example to browse and measure images), ' +
                       'but cannot use the Leica camera. Try Tools > Install / repair camera driver ' +
                       'in DM Imaging, and see "If something goes wrong" in the deployment notes.',
                       mbError, MB_OK, IDOK);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then begin
    InstallVcRuntime();
    if WizardIsTaskSelected('installdriver') then
      InstallCameraDriver();
  end;
end;
