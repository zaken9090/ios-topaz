param([string]$Kernel, [string[]]$Specs)
# each spec "tag|extra boot-args": build image, boot it over USB/fastboot, bench, next.
$P = 'C:\Users\Vasyl\Desktop\Claude\Projects\Android-iOS'
$S = 'C:\Users\Vasyl\AppData\Local\Temp\claude\880cf9603a2f840c\b1095f33-e9d6-4b97-bcfc-5f4a08d2a19a\scratchpad'
$base = 'lv_gold=96 lv_usb=1 sweetusb=2 sweetfps_osd=3 sweetfb_noconsole=1 sweetfb_offy=24 gpu=1 gpu_mhz=600 lv_ddr_khz=1804800 topaz_inject=/topaz/lib/libtopaz-inject.dylib,backboardd'
New-Item -ItemType Directory -Force "$P\test\flags" | Out-Null
foreach ($spec in $Specs) {
	$tag, $extra = $spec -split '\|', 2
	& "$S\mkimg.ps1" -Kernel $Kernel -Adt "$P\test\m1\lavender-gold4k.adt" -Out "$P\test\flags\$tag.img" -Extra "$base $extra" | Out-Null
	& "$S\cycle.ps1" -Img "$P\test\flags\$tag.img" | Out-Null
	& 'C:\Program Files\Git\bin\bash.exe' "$S\bench.sh" $tag 0 | Select-String 'BENCH'
}
python "$P\ios-lavender\lavender\tools\usblog.py" --cmd fastboot | Out-Null
