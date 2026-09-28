# Configures and builds DM Imaging.
#   .\build.ps1                 Release build of everything (x64)
#   .\build.ps1 -Config Debug
#   .\build.ps1 -Arch arm64     build for Windows on ARM (needs the msvc2022_arm64 Qt)
#   .\build.ps1 -NoApp          core library, tools and tests only
#   .\build.ps1 -Test           also run the tests (all four, through ctest)
#   .\build.ps1 -Deploy         copy the Qt runtime next to the executable (windeployqt)
#   .\build.ps1 -Clean          delete the build directory first
param(
    [string]$Config = 'Release',
    [ValidateSet('x64', 'arm64')][string]$Arch = 'x64',
    [switch]$NoApp,
    [switch]$Test,
    [switch]$Deploy,
    [switch]$Clean,
    [string]$QtDir = ''
)
$ErrorActionPreference = 'Continue'  # native tools report errors via exit codes
$root = $PSScriptRoot
if (-not $QtDir) {
    $qtKit = if ($Arch -eq 'arm64') { 'msvc2022_arm64' } else { 'msvc2022_64' }
    $QtDir = "$env:USERPROFILE\devtools\Qt\6.8.3\$qtKit"
}
if (-not $NoApp -and -not (Test-Path (Join-Path $QtDir 'lib\cmake\Qt6'))) {
    throw "Qt not found at $QtDir - install Qt 6.8 for $Arch (see README) or pass -QtDir"
}
. (Join-Path $root 'tools\vcvars.ps1') -Arch $Arch

# prefer cmake and ninja from PATH, else the copies bundled with Visual Studio
$vsCmake = Join-Path $env:VSINSTALLDIR 'Common7\IDE\CommonExtensions\Microsoft\CMake'
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) { $cmake = Join-Path $vsCmake 'CMake\bin\cmake.exe' }
$ctest = Join-Path (Split-Path $cmake) 'ctest.exe'
if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) { $env:PATH = "$(Join-Path $vsCmake 'Ninja');$env:PATH" }

# x64 keeps the historical build\release path; other architectures get their own tree
$buildName = $Config.ToLower()
if ($Arch -ne 'x64') { $buildName += "-$Arch" }
$build = Join-Path $root "build\$buildName"
if ($Clean -and (Test-Path $build)) { Remove-Item $build -Recurse -Force }

$cmakeArgs = @('-S', $root, '-B', $build, '-G', 'Ninja', "-DCMAKE_BUILD_TYPE=$Config", "-DCMAKE_PREFIX_PATH=$QtDir")
if ($NoApp) { $cmakeArgs += '-DBUILD_APP=OFF' } else { $cmakeArgs += '-DBUILD_APP=ON' }
& $cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
& $cmake --build $build --parallel
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }

$bin = Join-Path $build 'bin'
if ($Deploy -and -not $NoApp) {
    & "$QtDir\bin\windeployqt.exe" --no-translations --no-system-d3d-compiler --no-opengl-sw (Join-Path $bin 'DMImaging.exe')
    if ($LASTEXITCODE -ne 0) { throw 'windeployqt failed' }
}
if ($Test) {
    # The Qt-linked tests need the Qt DLLs, which are only next to them after -Deploy,
    # and run without a screen. windeployqt's platforms\ folder holds only qwindows.dll,
    # so point Qt at its own platform plugins, where 'offscreen' is.
    $saved = @{ PATH = $env:PATH; QT_QPA_PLATFORM = $env:QT_QPA_PLATFORM; QT_QPA_PLATFORM_PLUGIN_PATH = $env:QT_QPA_PLATFORM_PLUGIN_PATH }
    $env:PATH = "$QtDir\bin;$env:PATH"
    $env:QT_QPA_PLATFORM = 'offscreen'
    $env:QT_QPA_PLATFORM_PLUGIN_PATH = "$QtDir\plugins\platforms"
    & $ctest --test-dir $build --output-on-failure
    $testExit = $LASTEXITCODE
    foreach ($k in $saved.Keys) { Set-Item "env:$k" $(if ($null -eq $saved[$k]) { '' } else { $saved[$k] }) }
    if ($testExit -ne 0) { throw 'Tests failed' }
}
Write-Host "Build output: $bin"
