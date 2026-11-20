#!/bin/bash
# watch.sh: phone state. Exits (so Claude is notified) when the phone shows up in fastboot.
# Beeps when the phone looks hung: log silent > 60 s and no fastboot (at most every 3 min).
P=/c/Users/Vasyl/Desktop/Claude/Projects/Android-iOS
F="$P/dump/usblog-m40.txt"
FB="$P/platform-tools/fastboot.exe"
last=$(wc -c < "$F"); quiet=0; beeped=0
while true; do
	sleep 10
	if "$FB" devices 2>/dev/null | grep -q fastboot; then quiet=0; last=$(wc -c < "$F"); continue; fi
	now=$(wc -c < "$F")
	if [ "$now" != "$last" ]; then last=$now; quiet=0; continue; fi
	quiet=$((quiet + 10))
	if [ $quiet -ge 60 ] && [ $(( $(date +%s) - beeped )) -ge 180 ]; then
		echo "HUNG? log silent ${quiet}s $(date +%T)"
		powershell -c '[console]::Beep(880,300); [console]::Beep(1175,300)'
		beeped=$(date +%s)
	fi
done
