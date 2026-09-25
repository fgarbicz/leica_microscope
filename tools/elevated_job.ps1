# Job executed with admin rights by the "DMImaging Elevated Job" scheduled task.
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
& (Join-Path $root 'driver\install_driver.ps1')
