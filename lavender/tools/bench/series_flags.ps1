param([string]$Kernel, [string[]]$Specs, [switch]$FirstNow)
# each spec "tag|extra boot-args": build image, boot it over USB/fastboot, bench, next.
# -FirstNow: the first spec's image is already running (bench it without booting).
$P = 'C:\Users\Vasyl\Desktop\Claude\Projects\Android-iOS'
$S = $PSScriptRoot
$live = "$P\dump\bench-live.txt"
function Say($m) { $l = "$(Get-Date -Format HH:mm:ss) [series] $m"; Write-Host $l; Add-Content -Path $live -Value $l -Encoding utf8 }
$base = 'lv_gold=96 lv_usb=1 sweetusb=2 sweetfps_osd=3 sweetfb_noconsole=1 sweetfb_offy=24 gpu=1 gpu_mhz=600 lv_ddr_khz=1804800 lv_rpm_scale=1 topaz_inject=/topaz/lib/libtopaz-inject.dylib,backboardd'
New-Item -ItemType Directory -Force "$P\test\flags" | Out-Null
$n = 0
foreach ($spec in $Specs) {
	$n++
	$tag, $extra = $spec -split '\|', 2
	Say "($n/$($Specs.Count)) $tag : $extra"
	if ($FirstNow -and $n -eq 1) {
		& 'C:\Program Files\Git\bin\bash.exe' "$S\bench.sh" $tag 0 now | Select-String 'RESULT'
		continue
	}
	Say "building the image"
	& "$S\mkimg.ps1" -Kernel $Kernel -Adt "$P\test\m1\lavender-gold4k.adt" -Out "$P\test\flags\$tag.img" -Extra "$base $extra" | Out-Null
	if (-not (Test-Path "$P\test\flags\$tag.img")) { Say "image FAILED"; continue }
	Say "phone to fastboot, booting $tag.img"
	& "$S\cycle.ps1" -Img "$P\test\flags\$tag.img" | Out-Null
	Say "booted, bench starts"
	& 'C:\Program Files\Git\bin\bash.exe' "$S\bench.sh" $tag 0 | Select-String 'RESULT'
}
Say "series done: phone to fastboot"
python "$P\ios-lavender\lavender\tools\usblog.py" --cmd fastboot | Out-Null
