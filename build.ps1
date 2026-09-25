# Configures and builds DM Imaging.
#   .\build.ps1                 Release build of everything
#   .\build.ps1 -Config Debug
#   .\build.ps1 -NoApp          core library, tools and tests only
#   .\build.ps1 -Test           also run the unit tests
#   .\build.ps1 -Deploy         copy Qt runtime next to the executable (windeployqt)
param(
    [string]$Config = 'Release',
    [switch]$NoApp,
    [switch]$Test,
    [switch]$Deploy,
    [string]$QtDir = "$env:USERPROFILE\devtools\Qt\6.8.3\msvc2022_64"
)
$ErrorActionPreference = 'Continue'  # native tools report errors via exit codes
$root = $PSScriptRoot
. (Join-Path $root 'tools\vcvars.ps1') -Arch x64 2>$null

$vsCmake = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake"
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) { $cmake = "$vsCmake\CMake\bin\cmake.exe" }
$ninjaDir = "$vsCmake\Ninja"
if (Test-Path $ninjaDir) { $env:PATH = "$ninjaDir;$env:PATH" }

$build = Join-Path $root "build\$($Config.ToLower())"
$args = @('-S', $root, '-B', $build, '-G', 'Ninja', "-DCMAKE_BUILD_TYPE=$Config", "-DCMAKE_PREFIX_PATH=$QtDir")
if ($NoApp) { $args += '-DBUILD_APP=OFF' } else { $args += '-DBUILD_APP=ON' }
& $cmake @args
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
& $cmake --build $build --parallel
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }

$bin = Join-Path $build 'bin'
if ($Deploy -and -not $NoApp) {
    & "$QtDir\bin\windeployqt.exe" --no-translations --no-system-d3d-compiler --no-opengl-sw (Join-Path $bin 'DMImaging.exe')
}
if ($Test) {
    & (Join-Path $bin 'lmtests.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Unit tests failed' }
}
Write-Host "Build output: $bin"
