# Power-cycles the DMC6200 camera without unplugging it, by asking the USB-C
# PD controller (through UCSI) to reset the connector the camera is on. The
# PD controller drops VBUS for that port, which reboots the camera completely
# (a plain USB reset, device restart or controller restart does not).
#
# Needs admin rights and UcsiControl.exe from the Microsoft USB Test Tool
# (MUTT) package: https://www.microsoft.com/download/details.aspx?id=51604
# (x64_ucsicontrol inside the MSI's cabinet, renamed to UcsiControl.exe).
#
# Only the connector that sources power to a USB device (the camera) is reset;
# a connector that charges the laptop is never touched.
param(
    [string]$UcsiControl = "$env:USERPROFILE\devtools\mutt\UcsiControl.exe",
    [int]$Connector = 0   # 0 = detect
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path $UcsiControl)) { throw "UcsiControl.exe not found at $UcsiControl" }
$dev = (Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'ACPI\USBC000\*' } | Select-Object -First 1).InstanceId
if (-not $dev) { throw 'no UCSI device (ACPI\USBC000) found' }
$key = "HKLM:\SYSTEM\CurrentControlSet\Enum\$dev\Device Parameters"

function Invoke-Ucsi([uint32]$low) { & $UcsiControl Send 0 ('{0:x8}' -f $low) 2>&1 | Out-String }

# the UCSI test interface is off by default; enable it only for this operation
New-ItemProperty -Path $key -Name TestInterfaceEnabled -PropertyType DWord -Value 1 -Force | Out-Null
pnputil /restart-device "$dev" | Out-Null
Start-Sleep 3
try {
    if ($Connector -eq 0) {
        $caps = Invoke-Ucsi 0x6
        $count = if ($caps -match 'bNumConnectors:\s*(\d+)') { [int]$Matches[1] } else { 4 }
        foreach ($n in 1..$count) {
            $s = Invoke-Ucsi (0x12 -bor ($n -shl 16))
            # connected, laptop is the power provider, partner is a USB device
            if ($s -match 'ConnectStatus:\s*1' -and $s -match 'PowerDirection:\s*1' -and $s -match 'Usb:\s*1') {
                "connector $n: USB device powered by this port"
                if ($Connector -ne 0) { throw 'more than one powered USB device connector; pass -Connector' }
                $Connector = $n
            }
        }
        if ($Connector -eq 0) { throw 'no connector powering a USB device was found' }
    }
    "hard reset of connector $Connector"
    Invoke-Ucsi (0x3 -bor ($Connector -shl 16) -bor (1 -shl 23)) | Select-String 'completed|Failed'
    Start-Sleep 10
} finally {
    Remove-ItemProperty -Path $key -Name TestInterfaceEnabled -ErrorAction SilentlyContinue
    pnputil /restart-device "$dev" | Out-Null
}
Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'USB\VID_1711&PID_30E0*' } | Format-Table Status, InstanceId -AutoSize
