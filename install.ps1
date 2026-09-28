# Builds DM Imaging and installs it for the current user.
#   .\install.ps1              build (Release), deploy Qt runtime, install, create shortcuts
#   .\install.ps1 -Arch arm64  the same for Windows on ARM
#   .\install.ps1 -NoBuild     install the existing build
#   .\install.ps1 -Uninstall   remove the installation and shortcuts (settings are kept)
# For the lab PC use the installer instead (tools\make_installer.ps1): it installs for all
# users and sets up the camera driver.
param([ValidateSet('x64', 'arm64')][string]$Arch = 'x64', [switch]$NoBuild, [switch]$Uninstall)
$ErrorActionPreference = 'Continue'
$root = $PSScriptRoot
$target = Join-Path $env:LOCALAPPDATA 'Programs\DM Imaging'
$startMenu = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\DM Imaging.lnk'
$desktop = Join-Path ([Environment]::GetFolderPath('Desktop')) 'DM Imaging.lnk'

if ($Uninstall) {
    Get-Process DMImaging -ErrorAction SilentlyContinue | Stop-Process -Force
    foreach ($p in $target, $startMenu, $desktop) { if (Test-Path $p) { Remove-Item $p -Recurse -Force } }
    Write-Host 'DM Imaging removed.'
    exit 0
}

if (-not $NoBuild) {
    & (Join-Path $root 'build.ps1') -Config Release -Arch $Arch -Deploy
    if (-not $?) { throw 'build failed' }
}
$bin = Join-Path $root $(if ($Arch -eq 'x64') { 'build\release\bin' } else { "build\release-$Arch\bin" })
if (-not (Test-Path (Join-Path $bin 'DMImaging.exe'))) { throw 'DMImaging.exe not found - build first' }

Get-Process DMImaging -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
# start from an empty folder: copying a folder onto an existing one nests it (driver\driver)
# and would leave files from an older version behind
if (Test-Path $target) { Remove-Item $target -Recurse -Force }
New-Item -ItemType Directory -Force $target | Out-Null
# application + Qt runtime (skip test/tool executables)
robocopy $bin $target /E /NFL /NDL /NJH /NJS /XF lmtests.exe enginetest.exe iotest.exe uitest.exe dmctest.exe vc_redist.*.exe *.ilk *.pdb *.exp *.lib | Out-Null
if ($LASTEXITCODE -ge 8) { throw 'copying the build failed' }
$global:LASTEXITCODE = 0 # robocopy: 1 = files copied
# MSVC runtime next to the executable (the target PC may not have it)
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = if (Test-Path $vswhere) { & $vswhere -latest -products * -property installationPath } else { $null }
$redist = if ($vs) {
    Get-ChildItem (Join-Path $vs 'VC\Redist\MSVC') -Directory -ErrorAction SilentlyContinue | Where-Object Name -match '^\d' |
        Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
}
if ($redist) {
    Get-ChildItem (Join-Path $redist.FullName $Arch) -Recurse -Include msvcp140*.dll, vcruntime140*.dll, concrt140.dll, vcomp140.dll -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -notmatch 'debug' } | Copy-Item -Destination $target -Force
} else {
    Write-Warning 'Visual C++ runtime DLLs not found; the app needs the Microsoft Visual C++ Redistributable on this PC'
}
New-Item -ItemType Directory -Force (Join-Path $target 'driver') | Out-Null
Copy-Item (Join-Path $root 'driver\LeicaUsb3Cam.inf'), (Join-Path $root 'driver\install_driver.ps1') (Join-Path $target 'driver') -Force
Copy-Item (Join-Path $root 'docs') (Join-Path $target 'docs') -Recurse -Force
Copy-Item (Join-Path $root 'README.md') $target -Force

$shell = New-Object -ComObject WScript.Shell
foreach ($lnk in $startMenu, $desktop) {
    $s = $shell.CreateShortcut($lnk)
    $s.TargetPath = Join-Path $target 'DMImaging.exe'
    $s.WorkingDirectory = $target
    $s.IconLocation = (Join-Path $target 'DMImaging.exe') + ',0'
    $s.Description = 'DM Imaging - Leica DM2000 microscope camera'
    $s.Save()
}
Write-Host "Installed to $target"
Write-Host 'Shortcuts: Start menu and desktop ("DM Imaging")'
