# One-time setup (run elevated): registers the scheduled task "DMImaging Elevated Job"
# which runs tools\elevated_job.ps1 with highest privileges on demand, so the
# development agent can (re)install the camera driver without further UAC prompts.
# Trigger:  schtasks /run /tn "DMImaging Elevated Job"
# Remove:   schtasks /delete /tn "DMImaging Elevated Job" /f
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$job = Join-Path $root 'tools\elevated_job.ps1'
$log = Join-Path $root 'tools\elevated_job.log'
$user = [Security.Principal.WindowsIdentity]::GetCurrent().Name

$action = New-ScheduledTaskAction -Execute 'powershell.exe' `
    -Argument "-NoProfile -ExecutionPolicy Bypass -Command `"& '$job' *> '$log'; 'EXIT:' + `$LASTEXITCODE | Add-Content '$log'`""
$principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Hours 2)
Register-ScheduledTask -TaskName 'DMImaging Elevated Job' -Action $action -Principal $principal -Settings $settings -Force | Out-Null
Write-Host "Registered task for $user"

# install the camera driver right away
& (Join-Path $root 'driver\install_driver.ps1')
