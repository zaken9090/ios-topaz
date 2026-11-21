#!/bin/bash
# ladder.sh IMG...: for each image: fastboot boot, wait for iOS (LVSTAT) - beep and stop if it hangs - 40 s, bench (incl. Spotlight shot)
P=/c/Users/Vasyl/Desktop/Claude/Projects/Android-iOS; S=/c/Users/Vasyl/AppData/Local/Temp/claude/880cf9603a2f840c/3663574c-5523-46a3-8852-592f80c20921/scratchpad
F=/c/Users/Vasyl/Desktop/miflash_unlock_en_7.6.727.43/fastboot.exe; L=$P/dump/usblog-m51.txt
beep() { powershell.exe -c "[console]::Beep(880,300); [console]::Beep(1175,300)"; }
cd $P
for img in "$@"; do
	n=$(basename $img .img)
	$F devices | grep -q fastboot || python ios-lavender/lavender/tools/usblog.py --cmd fastboot >/dev/null
	for i in $(seq 1 60); do $F devices | grep -q fastboot && break; sleep 2; done
	$F devices | grep -q fastboot || { echo "NO FASTBOOT before $n"; beep; exit 1; }
	s=$(wc -c < $L); $F boot $img >/dev/null 2>&1
	ok=0; for i in $(seq 1 75); do tail -c +$s $L | grep -aq LVSTAT && { ok=1; break; }; sleep 2; done
	[ $ok = 1 ] || { echo "$n HUNG (no LVSTAT)"; echo "$(date +%T) [$n] HUNG: no LVSTAT in 150 s" >> $P/dump/bench-live.txt; beep; exit 1; }
	sleep 40
	LOG=$L bash $S/bench.sh $n 0 now 2>&1 | grep -E "RESULT|spotlight"
done
python ios-lavender/lavender/tools/usblog.py --cmd fastboot >/dev/null
