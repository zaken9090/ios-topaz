#!/bin/bash
# spot.sh TAG: phone just booted -> unlock, open Spotlight (swipe down mid-screen) 3 times with screenshots
P=/c/Users/Vasyl/Desktop/Claude/Projects/Android-iOS
S=/c/Users/Vasyl/AppData/Local/Temp/claude/880cf9603a2f840c/3663574c-5523-46a3-8852-592f80c20921/scratchpad
T=$P/ios-lavender/lavender/tools/usblog.py
F=${LOG:-$P/dump/usblog-m51.txt}
TAG=$1
LIVE=$P/dump/bench-live.txt
say() { echo "$(date +%T) [$TAG] $*" | tee -a "$LIVE"; }
cmd() { python "$T" --cmd "$1" >/dev/null; }
shot() { python "$T" --shot "$TAG-$1" --build "$TAG" | grep -o '[^ \/]*\.png' | tail -1; }
start=$(wc -c < "$F")
say "waiting for iOS (LVSTAT)"
for i in $(seq 1 150); do tail -c +$start "$F" | grep -aq LVSTAT && break; sleep 2; done
sleep 40
cmd "key power"; sleep 2
for try in 1 2 3 4 5; do
	cmd "swipe 540 2300 540 900 250"; sleep 4
	f=$(shot home)
	python -I "$S/ishome.py" "$P/screenshots/$f" >/dev/null 2>&1 && { say "home ($f)"; break; }
	say "try $try: not home ($f)"; cmd "key home"; sleep 2
done
for k in 1 2 3; do
	cmd "swipe 540 900 540 1600 400"; sleep 3
	f=$(shot spot$k); say "spotlight $k ($f)"
	cmd "key home"; sleep 3
done
say "DONE"
