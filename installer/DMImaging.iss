; Inno Setup script for DM Imaging (build with tools\make_installer.ps1)
;   ISCC /DAppVersion=1.0.0 /DStageDir=<staged files> /DOutDir=<output> installer\DMImaging.iss

#ifndef AppVersion
  #define AppVersion "1.0.0"
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
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir={#OutDir}
OutputBaseFilename=DMImaging-Setup-{#AppVersion}
SetupIconFile=..\resources\app.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
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
Source: "{#RedistDir}\vc_redist.x64.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"; Comment: "Leica DM2000 microscope camera"
Name: "{autoprograms}\{#AppName} User Guide"; Filename: "{app}\docs\USER_GUIDE.html"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon; Comment: "Leica DM2000 microscope camera"

[Run]
; Microsoft Visual C++ runtime (no-op when a current version is installed; 1638 = newer present)
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "Installing the Microsoft Visual C++ runtime..."; Flags: waituntilterminated
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\driver\install_driver.ps1"""; \
  StatusMsg: "Installing the camera driver..."; Flags: runhidden waituntilterminated; Tasks: installdriver
Filename: "{app}\{#AppExe}"; Description: "Start {#AppName} now"; Flags: postinstall nowait skipifsilent

[UninstallRun]
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\driver\install_driver.ps1"" -Uninstall"; \
  Flags: runhidden waituntilterminated; RunOnceId: "RemoveCameraDriver"

[UninstallDelete]
Type: files; Name: "{app}\driver\*.cat"
