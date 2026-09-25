Add-Type -Path "$PSScriptRoot\UsbProbe.cs"
$path = '\\?\USB#VID_1711&PID_30E0#2000000518#{a5dcbf10-6530-11d2-901f-00c04fb951ed}'
$d = New-Object UsbProbe $path
function Send([byte[]]$b, [string]$label) {
    $w = $d.Write(1, $b)
    $r = $d.Read(0x81, 4096)
    "{0,-28} wrote={1} -> {2}" -f $label, $w, $(if ($r) { ([UsbProbe]::Hex($r, 64)).Trim() } else { "no reply (err $($d.LastError))" })
}
try {
    $d.SetTimeout(0x81, 500)
    $st = 0
    foreach ($i in 1..2) { $r = $d.GenCpRead(0, 64, [ref]$st); "GenCP read #$i -> " + ([UsbProbe]::Hex($r)).Trim() }
    Send ([byte[]](0x55,0x47,0x56,0x43)) 'UGVC only'
    Send ([byte[]](0,0,0,0,0,0,0,0)) '8 zero bytes'
    Send ([byte[]](0x55,0x47,0,0,0,0,0,0)) 'UG + zeros'
    Send ([byte[]](0x55,0x47,0,0,1,0,0,0)) 'UG 00 00 01 00'
    Send ([byte[]](0x55,0x47,1,0,0,0,0,0)) 'UG 01 00 ...'
    "--- interrupt EP 0x82"
    $d.SetTimeout(0x82, 300)
    $r = $d.Read(0x82, 64); if ($r) { [UsbProbe]::Hex($r) } else { "no data (err $($d.LastError))" }
    "--- vendor control IN requests (read-only)"
    foreach ($req in 0..15) {
        $r = $d.Control(0xC0, [byte]$req, 0, 0, 64)
        if ($r) { "req 0x{0:X2}: {1}" -f $req, ([UsbProbe]::Hex($r, 64)).Trim() }
    }
    foreach ($req in 0..15) {
        $r = $d.Control(0xC1, [byte]$req, 0, 0, 64)
        if ($r) { "iface req 0x{0:X2}: {1}" -f $req, ([UsbProbe]::Hex($r, 64)).Trim() }
    }
} finally { $d.Dispose() }
