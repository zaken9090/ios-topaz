#!/bin/bash
# profset.sh TAG PROC: phone just booted -> unlock, open Settings, profile PROC for 12 s while scrolling
P=/c/Users/Vasyl/Desktop/Claude/Projects/Android-iOS
S=/c/Users/Vasyl/AppData/Local/Temp/claude/880cf9603a2f840c/3663574c-5523-46a3-8852-592f80c20921/scratchpad
T=$P/ios-lavender/lavender/tools/usblog.py
F=${LOG:-$P/dump/usblog-m51.txt}
TAG=$1; PROC=${2:-Preferences}
LIVE=$P/dump/bench-live.txt
say() { echo "$(date +%T) [$TAG] $*" | tee -a "$LIVE"; }
cmd() { python "$T" --cmd "$1" >/dev/null; }
shot() { python "$T" --shot "$TAG-$1" --build "$TAG" | grep -o '[^ \/]*\.png' | tail -1; }
say "=========START TEST========="
start=$(wc -c < "$F")
for i in $(seq 1 150); do tail -c +$start "$F" | grep -aq LVSTAT && break; sleep 2; done
sleep 40; cmd "key power"; sleep 2
for try in 1 2 3 4 5; do
	cmd "swipe 540 2300 540 900 250"; sleep 4
	f=$(shot home); python -I "$S/ishome.py" "$P/screenshots/$f" >/dev/null 2>&1 && { say "home ($f)"; break; }
	cmd "key home"; sleep 2
done
cmd "tap 675 594"; sleep 5; say "settings ($(shot settings))"
s=$(wc -c < "$F")
cmd "prof $PROC 12"; sleep 1
for i in 1 2 3; do cmd "swipe 540 2000 540 500 2000"; sleep 2.6; cmd "swipe 540 500 540 2000 2000"; sleep 2.6; done
sleep 4
tail -c +$s "$F" | grep -aE "^PROF ($PROC|cpu|image|other)" | head -30
say "=========END TEST========="
