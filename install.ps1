# Builds DM Imaging and installs it for the current user.
#   .\install.ps1              build (Release), deploy Qt runtime, install, create shortcuts
#   .\install.ps1 -NoBuild     install the existing build
#   .\install.ps1 -Uninstall   remove the installation and shortcuts (settings are kept)
param([switch]$NoBuild, [switch]$Uninstall)
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
    & (Join-Path $root 'build.ps1') -Config Release -Deploy
    if ($LASTEXITCODE -ne 0) { throw 'build failed' }
}
$bin = Join-Path $root 'build\release\bin'
if (-not (Test-Path (Join-Path $bin 'DMImaging.exe'))) { throw 'DMImaging.exe not found - build first' }

Get-Process DMImaging -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
New-Item -ItemType Directory -Force $target | Out-Null
# application + Qt runtime (skip test/tool executables)
robocopy $bin $target /E /NFL /NDL /NJH /NJS /XF lmtests.exe enginetest.exe dmctest.exe *.ilk *.pdb | Out-Null
$global:LASTEXITCODE = 0 # robocopy: 1 = files copied
# MSVC runtime next to the executable (the target PC may not have it)
$redist = Get-ChildItem "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\VC\Redist\MSVC" -Directory -ErrorAction SilentlyContinue | Where-Object Name -match '^\d' |
    Sort-Object Name -Descending | Select-Object -First 1
if ($redist) {
    Get-ChildItem (Join-Path $redist.FullName 'x64') -Recurse -Include msvcp140*.dll, vcruntime140*.dll, concrt140.dll, vcomp140.dll -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -notmatch 'debug' } | Copy-Item -Destination $target -Force
}
Copy-Item (Join-Path $root 'driver') (Join-Path $target 'driver') -Recurse -Force
Remove-Item (Join-Path $target 'driver\*.cat') -ErrorAction SilentlyContinue
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

