param([string]$Run, [string]$Dir, [string]$Name, [string]$Extra = '', [string]$Img = '')
# CI kernel -> image -> phone to fastboot over USB (lv:fastboot) -> fastboot boot. Beeps only when stuck.
$P = 'C:\Users\Vasyl\Desktop\Claude\Projects\Android-iOS'
$S = $PSScriptRoot
$fb = "$P\platform-tools\fastboot.exe"
$base = 'lv_gold=96 lv_usb=1 sweetusb=2 sweetfps_osd=3 sweetfb_noconsole=1 sweetfb_offy=24 gpu=1 gpu_mhz=600 lv_ddr_khz=1804800 topaz_inject=/topaz/lib/libtopaz-inject.dylib,backboardd'
if ($Img -eq '') {
	& "$S\after_ci.ps1" -Run $Run -Dir $Dir -Adt "$P\test\m1\lavender-gold4k.adt" -Specs "$Name|$base $Extra"
	$Img = "$P\test\$Dir\$Name.img"
}
if (-not (Test-Path $Img)) { Write-Host "NO IMAGE $Img"; exit 1 }
if (-not ((& $fb devices) -match 'fastboot')) {
	python "$P\ios-lavender\lavender\tools\usblog.py" --cmd fastboot | Out-Null
	$t = 0
	while (-not ((& $fb devices) -match 'fastboot') -and $t -lt 60) { Start-Sleep 2; $t += 2 }
	if (-not ((& $fb devices) -match 'fastboot')) {
		Write-Host 'phone did not reach fastboot: user needed'
		[console]::Beep(880,300); [console]::Beep(1175,300)
	}
}
& "$S\series.ps1" -Images $Img
