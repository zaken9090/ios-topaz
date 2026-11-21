#!/bin/bash
# shade.sh TAG: phone just booted -> wait for iOS, unlock, then pull the notification shade and
# Control Center down, with screenshots (also one while the shade is half way).
P=/c/Users/Vasyl/Desktop/Claude/Projects/Android-iOS
S=/c/Users/Vasyl/AppData/Local/Temp/claude/880cf9603a2f840c/3663574c-5523-46a3-8852-592f80c20921/scratchpad
T=$P/ios-lavender/lavender/tools/usblog.py
F=${LOG:-$P/dump/usblog-m51.txt}
TAG=$1
LIVE=$P/dump/bench-live.txt
say() { echo "$(date +%T) [$TAG] $*" | tee -a "$LIVE"; }
cmd() { python "$T" --cmd "$1" >/dev/null; }
shot() { python "$T" --shot "$TAG-$1" --build "$TAG" | grep -o '[^ \\/]*\.png' | tail -1; }
start=$(wc -c < "$F")
say "waiting for iOS (LVSTAT)"
for i in $(seq 1 150); do tail -c +$start "$F" | grep -aq LVSTAT && break; sleep 2; done
say "iOS draws; 40 s"
sleep 40
cmd "key power"; sleep 2
for try in 1 2 3 4 5; do
	cmd "swipe 540 2300 540 900 250"; sleep 4
	f=$(shot home)
	python -I "$S/ishome.py" "$P/screenshots/$f" >/dev/null 2>&1 && { say "home ($f)"; break; }
	say "try $try: not home ($f)"
	cmd "key home"; sleep 2
done
if [ "$2" = loop ]; then
	say "shade pulls for 70 s"
	for k in $(seq 1 16); do cmd "swipe 300 10 300 1500 1500"; sleep 2; cmd "swipe 540 2400 540 600 300"; sleep 2; done
fi
say "notification shade: slow pull"
cmd "swipe 300 10 300 1300 3000" & sleep 1.5; f=$(shot shade-half); wait; say "shade half way ($f)"
sleep 2; f=$(shot shade-open); say "shade open ($f)"
cmd "swipe 540 2400 540 600 300"; sleep 3
say "Control Center"
cmd "swipe 1000 10 1000 1400 600"; sleep 3; f=$(shot cc); say "control center ($f)"
cmd "swipe 540 2300 540 600 300"; sleep 2
say "DONE"
