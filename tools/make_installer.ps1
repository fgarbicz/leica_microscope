# Builds the release, runs the tests and creates the Windows installer
# dist\DMImaging-Setup-<version>.exe (Inno Setup 6).
#   .\tools\make_installer.ps1
#   .\tools\make_installer.ps1 -SkipBuild      package the existing build
#   .\tools\make_installer.ps1 -Iscc <path>    location of ISCC.exe
param(
    [switch]$SkipBuild,
    [string]$Iscc = ''
)
# native tools report failure through exit codes (some print harmless text to stderr,
# e.g. the image I/O test's deliberately corrupt files), so don't stop on stderr
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot

# version from main.cpp (setApplicationVersion("x.y.z"))
$main = Get-Content (Join-Path $root 'src\app\main.cpp') -Raw
if ($main -notmatch 'setApplicationVersion\(QStringLiteral\("([0-9.]+)"\)\)') { throw 'version not found in src/app/main.cpp' }
$version = $Matches[1]
Write-Host "DM Imaging $version"

if (-not $SkipBuild) {
    & (Join-Path $root 'build.ps1') -Config Release -Deploy -Test
    if (-not $? -or ($LASTEXITCODE -and $LASTEXITCODE -ne 0)) { throw 'build or tests failed' }
    foreach ($t in 'enginetest.exe', 'iotest.exe') {
        & (Join-Path $root "build\release\bin\$t")
        if ($LASTEXITCODE -ne 0) { throw "$t failed" }
    }
}
$bin = Join-Path $root 'build\release\bin'
if (-not (Test-Path (Join-Path $bin 'DMImaging.exe'))) { throw 'DMImaging.exe not found - build first' }

# stage the files that go into the installer
$stage = Join-Path $root 'build\package\app'
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null
robocopy $bin $stage /E /NFL /NDL /NJH /NJS /XF lmtests.exe enginetest.exe iotest.exe dmctest.exe *.ilk *.pdb *.exp *.lib | Out-Null
if ($LASTEXITCODE -ge 8) { throw 'copying the build failed' }
$global:LASTEXITCODE = 0

# Visual C++ runtime: Microsoft's redistributable installer (placed next to the
# executable by windeployqt), run silently by Setup; it does nothing if a current
# runtime is already installed
$vcredist = Join-Path $bin 'vc_redist.x64.exe'
if (-not (Test-Path $vcredist)) { throw 'vc_redist.x64.exe not found in the build (windeployqt should place it there)' }
$redistStage = Join-Path $root 'build\package\redist'
New-Item -ItemType Directory -Force $redistStage | Out-Null
Copy-Item $vcredist $redistStage -Force
Remove-Item (Join-Path $stage 'vc_redist.x64.exe') -ErrorAction SilentlyContinue

# driver (the catalog is generated and signed on each PC by install_driver.ps1), documentation
New-Item -ItemType Directory -Force (Join-Path $stage 'driver') | Out-Null
Copy-Item (Join-Path $root 'driver\LeicaUsb3Cam.inf'), (Join-Path $root 'driver\install_driver.ps1') (Join-Path $stage 'driver')
Copy-Item (Join-Path $root 'docs') (Join-Path $stage 'docs') -Recurse
Copy-Item (Join-Path $root 'README.md'), (Join-Path $root 'RELEASE_NOTES.md') $stage

# user guide as HTML for the Start menu (needs Python with the 'markdown' package; else the .md is used)
# (the 'python' on PATH may be the Microsoft Store stub, so prefer a real install that has 'markdown')
$python = $null
foreach ($c in @("$env:USERPROFILE\devtools\python\python.exe", (Get-Command python -ErrorAction SilentlyContinue).Source)) {
    if ($c -and (Test-Path $c)) {
        & $c -c 'import markdown' 2>$null
        if ($LASTEXITCODE -eq 0) { $python = $c; break }
    }
}
$guideHtml = Join-Path $stage 'docs\USER_GUIDE.html'
if ($python) { & $python (Join-Path $root 'tools\make_guide.py') (Join-Path $root 'docs\USER_GUIDE.md') $guideHtml }
if (-not (Test-Path $guideHtml)) {
    Write-Warning 'HTML user guide not generated; the Start menu entry opens the Markdown file'
    Copy-Item (Join-Path $root 'docs\USER_GUIDE.md') $guideHtml
}

# sanity checks: files the application cannot start without
foreach ($f in 'DMImaging.exe', 'Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll', 'platforms\qwindows.dll',
               'imageformats\qjpeg.dll', 'driver\LeicaUsb3Cam.inf') {
    if (-not (Test-Path (Join-Path $stage $f))) { throw "missing in package: $f" }
}

if (-not $Iscc) {
    foreach ($c in "$env:USERPROFILE\devtools\inno\IS6\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
                   "$env:ProgramFiles\Inno Setup 6\ISCC.exe", "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe") {
        if (Test-Path $c) { $Iscc = $c; break }
    }
}
if (-not $Iscc) { throw 'Inno Setup 6 (ISCC.exe) not found - install it or pass -Iscc' }
$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force $dist | Out-Null
& $Iscc "/DAppVersion=$version" "/DStageDir=$stage" "/DRedistDir=$redistStage" "/DOutDir=$dist" (Join-Path $root 'installer\DMImaging.iss')
if ($LASTEXITCODE -ne 0) { throw 'Inno Setup failed' }
$setup = Join-Path $dist "DMImaging-Setup-$version.exe"
Write-Host ("Installer: {0} ({1:N1} MB)" -f $setup, ((Get-Item $setup).Length / 1MB))
