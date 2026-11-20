param([string]$Kernel, [string]$Out, [string]$Extra, [string]$Adt = 'C:\Users\Vasyl\Desktop\Claude\Projects\Android-iOS\test\m1\lavender-silver.adt')
$ErrorActionPreference = 'Continue'
$P = 'C:\Users\Vasyl\Desktop\Claude\Projects\Android-iOS'
$base = 'debug=0xc serial=3 cpus=4 keepsyms=1 -noprogress rd=ufsroot0 ufsrw=1 rootrw=1 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin launchd_unsecure_cache=1 wdt=-1 sweetcpufreq=0 sweetfb_direct=0 sweetfb_stayon=1 sweetchg=0 sweetbattery=0 sweetfb_hz=30 sweettouchdiag=1'
if ($Extra -match '(^| )cpus=') { $base = $base -replace ' cpus=4', '' }
$cmd = "$base $Extra"
Write-Host "cmdline ($($cmd.Length)): $cmd"
$payload = [IO.Path]::ChangeExtension($Out, '.bin')
python "$P\ios-lavender\lavender\tools\mkpayload.py" $Kernel $Adt $payload --cmdline $cmd
Push-Location "$P\ios-lavender\lavender\shim"
try { & .\build.ps1 -Payload $payload } finally { Pop-Location }
Copy-Item "$P\ios-lavender\lavender\shim\out\shim-boot.img" $Out -Force
Get-Item $Out | Select-Object Name, Length
