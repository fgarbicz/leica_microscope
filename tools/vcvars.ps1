# Imports the MSVC build environment into the current PowerShell session.
# usage: . tools\vcvars.ps1 [-Arch x64|x86]
param([string]$Arch = 'x64')
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = if (Test-Path $vswhere) { & $vswhere -latest -products * -property installationPath } else { $null }
if (-not $vs) { $vs = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools" }
$bat = Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat'
$target = if ($Arch -eq 'x86') { 'x64_x86' } else { 'x64' }
cmd /c "`"$bat`" $target >nul 2>nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
}
