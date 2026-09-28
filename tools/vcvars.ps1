# Imports the MSVC build environment into the current PowerShell session.
# usage: . tools\vcvars.ps1 [-Arch x64|arm64|x86]
# -Arch is the architecture to build FOR. The host is detected, so an x64 build on a
# Windows-on-ARM machine uses the ARM64-hosted cross compiler (and vice versa).
param([ValidateSet('x64', 'arm64', 'x86')][string]$Arch = 'x64')
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = if (Test-Path $vswhere) { & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath } else { $null }
if (-not $vs -and (Test-Path $vswhere)) { $vs = & $vswhere -latest -products * -property installationPath }
if (-not $vs) { $vs = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools" }
$bat = Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat'
if (-not (Test-Path $bat)) { throw "Visual Studio C++ build tools not found (looked for $bat)" }

# OSArchitecture reports the machine, even from an emulated x64 PowerShell
$hostArch = if ([System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture -eq 'Arm64') { 'arm64' } else { 'amd64' }
$targetArch = @{ x64 = 'amd64'; arm64 = 'arm64'; x86 = 'x86' }[$Arch]
$spec = if ($hostArch -eq $targetArch) { $hostArch } else { "${hostArch}_$targetArch" }
cmd /c "`"$bat`" $spec >nul 2>nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
}
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) { throw "vcvarsall $spec did not provide cl.exe - is the $Arch compiler component installed?" }
