param([string]$Run, [string]$Dir, [string[]]$Specs, [string]$Adt = 'C:\Users\Vasyl\Desktop\Claude\Projects\Android-iOS\test\m1\lavender-silver.adt')
# Specs: "name|extra boot-args"
$P = 'C:\Users\Vasyl\Desktop\Claude\Projects\Android-iOS'
$gh = 'C:\Program Files\GitHub CLI\gh.exe'
& $gh run watch $Run -R zaken9090/ios-topaz --exit-status --interval 60 | Out-Null
if ($LASTEXITCODE -ne 0) { Write-Host "CI FAILED $Run"; exit 1 }
& $gh run download $Run -R zaken9090/ios-topaz -n xnu-kernel -D "$P\ci\run-$Run"
$k = "$P\ci\run-$Run\kernel.development.sweet"
New-Item -ItemType Directory -Force "$P\test\$Dir" | Out-Null
$mk = 'C:\Users\Vasyl\AppData\Local\Temp\claude\880cf9603a2f840c\b1095f33-e9d6-4b97-bcfc-5f4a08d2a19a\scratchpad\mkimg.ps1'
foreach ($s in $Specs) {
	$name, $extra = $s -split '\|', 2
	& $mk -Kernel $k -Adt $Adt -Out "$P\test\$Dir\$name.img" -Extra $extra | Out-Null
	if (Test-Path "$P\test\$Dir\$name.img") { Write-Host "built $name" } else { Write-Host "FAILED $name" }
}
Write-Host 'IMAGES READY'
