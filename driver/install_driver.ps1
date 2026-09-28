# Installs the WinUSB binding for the Leica USB3 camera.
#  1. creates (once) a local code-signing certificate "DM Imaging Driver Signing"
#  2. generates and signs the catalog for LeicaUsb3Cam.inf
#  3. trusts the certificate (LocalMachine Root + TrustedPublisher)
#  4. installs the driver package with pnputil and binds it to the camera
# Run from any PowerShell; it elevates itself through UAC when necessary.
param([switch]$Uninstall)

$ErrorActionPreference = 'Stop'
# Everything is written with Write-Host so that a caller redirecting this script's
# output (Setup's log, the application's "Install / repair camera driver") gets all
# of it: Out-Host bypasses redirection, and an uncaught error would end the script
# without its message reaching the log.
trap {
    Write-Host "ERROR: $_"
    exit 1
}
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    # re-run elevated, capturing the output in a log we can show here
    $log = Join-Path $env:TEMP 'dmimaging_driver_install.log'
    $self = $MyInvocation.MyCommand.Path
    $flag = if ($Uninstall) { ' -Uninstall' } else { '' }
    $cmd = "& '$self'$flag *> '$log'; exit `$LASTEXITCODE"
    $p = Start-Process powershell -Verb RunAs -Wait -PassThru -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', $cmd)
    Get-Content $log -ErrorAction SilentlyContinue
    exit $p.ExitCode
}

$certName = 'DM Imaging Driver Signing'
$inf = Join-Path $here 'LeicaUsb3Cam.inf'
$cat = Join-Path $here 'LeicaUsb3Cam.cat'

if ($Uninstall) {
    $pkgs = pnputil /enum-drivers | Out-String
    $blocks = $pkgs -split "(\r?\n){2,}"
    foreach ($b in $blocks) {
        if ($b -match 'Original Name:\s+leicausb3cam\.inf' -and $b -match 'Published Name:\s+(oem\d+\.inf)') {
            Write-Host "Removing $($Matches[1])"
            pnputil /delete-driver $Matches[1] /uninstall /force
        }
    }
    exit 0
}

function Find-SdkTool([string]$name) {
    $roots = @("${env:ProgramFiles(x86)}\Windows Kits\10\bin", "$env:ProgramFiles\Windows Kits\10\bin")
    foreach ($r in $roots) {
        if (Test-Path $r) {
            $hit = Get-ChildItem $r -Recurse -Filter $name -ErrorAction SilentlyContinue |
                Where-Object { $_.FullName -match '\\x64\\' } | Sort-Object FullName -Descending | Select-Object -First 1
            if ($hit) { return $hit.FullName }
        }
    }
    throw "$name not found (install the Windows 10/11 SDK)"
}

# already installed and in use? then nothing to do (an app upgrade must not disturb the camera)
function Get-OurPublishedInfs {
    $names = @()
    foreach ($block in ((pnputil /enum-drivers | Out-String) -split "(\r?\n){2,}")) {
        if ($block -match 'Original Name:\s+leicausb3cam\.inf' -and $block -match 'Published Name:\s+(oem\d+\.inf)') { $names += $Matches[1] }
    }
    return $names
}
$camera = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'USB\VID_1711&PID_30E0*' } | Select-Object -First 1
if ($camera -and $camera.Status -eq 'OK') {
    $inUse = (Get-PnpDeviceProperty -InstanceId $camera.InstanceId -KeyName DEVPKEY_Device_DriverInfPath -ErrorAction SilentlyContinue).Data
    if ($inUse -and (Get-OurPublishedInfs) -contains $inUse) {
        Write-Host "The camera driver is already installed and in use ($inUse)."
        exit 0
    }
}

# 1. certificate
$cert = Get-ChildItem Cert:\LocalMachine\My | Where-Object { $_.Subject -eq "CN=$certName" } | Select-Object -First 1
if (-not $cert) {
    Write-Host 'Creating code signing certificate'
    $cert = New-SelfSignedCertificate -Subject "CN=$certName" -Type CodeSigningCert -CertStoreLocation Cert:\LocalMachine\My `
        -KeyUsage DigitalSignature -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(20)
}
foreach ($store in 'Root', 'TrustedPublisher') {
    $s = New-Object System.Security.Cryptography.X509Certificates.X509Store($store, 'LocalMachine')
    $s.Open('ReadWrite')
    if (-not ($s.Certificates | Where-Object Thumbprint -eq $cert.Thumbprint)) { $s.Add($cert) }
    $s.Close()
}

# 2. catalog
$makecat = $null; $signtool = $null
try { $makecat = Find-SdkTool 'makecat.exe'; $signtool = Find-SdkTool 'signtool.exe' } catch { }
if (-not $makecat) {
    Write-Host 'SDK tools not found - using New-FileCatalog / Set-AuthenticodeSignature'
    if (Test-Path $cat) { Remove-Item $cat -Force }
    $stage = Join-Path $env:TEMP 'dmimaging_cat'
    Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory $stage | Out-Null
    Copy-Item $inf $stage
    New-FileCatalog -Path $stage -CatalogFilePath $cat -CatalogVersion 2 | Out-String | Write-Host
    $sig = Set-AuthenticodeSignature -FilePath $cat -Certificate $cert -HashAlgorithm SHA256
    Write-Host "Catalog signature: $($sig.Status)"
} else {
$cdf = Join-Path $env:TEMP 'LeicaUsb3Cam.cdf'
@"
[CatalogHeader]
Name=LeicaUsb3Cam.cat
ResultDir=$here
PublicVersion=0x0000001
EncodingType=0x00010001
CATATTR1=0x10010001:OSAttr:2:6.1,2:6.2,2:6.3,2:10.0

[CatalogFiles]
<hash>LeicaUsb3Cam.inf=$inf
<hash>LeicaUsb3Cam.infATTR1=0x10010001:OSAttr:2:6.1,2:6.2,2:6.3,2:10.0
"@ | Set-Content -Path $cdf -Encoding ASCII
if (Test-Path $cat) { Remove-Item $cat -Force }
& $makecat -v $cdf | Out-String | Write-Host
if (-not (Test-Path $cat)) { throw 'makecat failed' }
& $signtool sign /v /fd SHA256 /sm /s My /sha1 $cert.Thumbprint $cat | Out-String | Write-Host
if ($LASTEXITCODE -ne 0) { throw 'signtool failed' }
}

# 3. install + bind
# pnputil /install also binds the driver to a connected camera, so the camera's USB
# device is not restarted here (restarting it can leave the camera without its sensor)
Write-Host 'Installing driver package'
pnputil /add-driver $inf /install | Out-String | Write-Host
$rc = $LASTEXITCODE
# 0 = ok, 259 = already up to date / no matching device, 3010 = ok, restart needed
if ($rc -eq 3010) {
    Write-Host 'Driver installed. Windows asks for a restart before the camera can use it.'
} elseif ($rc -ne 0 -and $rc -ne 259) {
    throw "pnputil failed with exit code $rc"
}
Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'USB\VID_1711&PID_30E0*' } | Format-Table Status, Class, FriendlyName -AutoSize | Out-String | Write-Host
Write-Host 'Camera driver ready.'
exit 0
