param([string[]]$Images)
$fb = 'C:\Users\Vasyl\Desktop\Claude\Projects\Android-iOS\platform-tools\fastboot.exe'
$n = 0
foreach ($img in $Images) {
	$n++
	if (-not (Test-Path $img)) { Write-Host "[$n] no image $img - skipped"; continue }
	for ($try = 1; $try -le 10; $try++) {
		Write-Host "[$n/$($Images.Count)] try $try waiting for fastboot for $img"
		while (-not ((& $fb devices) -match 'fastboot')) { Start-Sleep -Seconds 2 }
		Start-Sleep -Seconds 3
		[console]::Beep(880,300); [console]::Beep(1175,300)
		& $fb boot $img
		if ($LASTEXITCODE -eq 0) { break }
		Write-Host "[$n] boot failed: replug needed"
		[console]::Beep(400,800)
		# wait for the stuck device to go away (Power+VolDown, replug)
		$t = 0
		while (((& $fb devices) -match 'fastboot') -and $t -lt 900) { Start-Sleep -Seconds 2; $t += 2 }
	}
	Write-Host "[$n] booted $img at $(Get-Date -Format HH:mm:ss)"
	while ((& $fb devices) -match 'fastboot') { Start-Sleep -Seconds 2 }
}
Write-Host 'series done'
