Add-Type -Path "$PSScriptRoot\UsbProbe.cs"
$path = '\\?\USB#VID_1711&PID_30E0#2000000518#{a5dcbf10-6530-11d2-901f-00c04fb951ed}'
$d = New-Object UsbProbe $path
try {
    foreach ($i in 1..3) { $s = $d.Descriptor(3, $i, 0x409, 255); "STR$i : " + $(if ($s) { [Text.Encoding]::Unicode.GetString($s, 2, $s.Length - 2) } else { "err $($d.LastError)" }) }
    "--- GenCP READMEM 0x0 (64 bytes)"
    $st = 0
    $r = $d.GenCpRead(0, 64, [ref]$st)
    "status=0x{0:X4} len={1}" -f $st, $(if ($r) { $r.Length } else { -1 })
    [UsbProbe]::Hex($r)
    [UsbProbe]::Ascii($r)
    "lastError=$($d.LastError)"
} finally { $d.Dispose() }
