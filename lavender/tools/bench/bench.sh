#!/bin/bash
# bench.sh TAG [PROF] [now]: after a fresh boot (or "now": the phone is up already) - wake, unlock,
# 3+3 slow swipes on the home screen, open Settings, 3+3 slow scrolls; mean LVSTAT fps of each part.
# Every step is checked with a screenshot and written with the time to dump\bench-live.txt.
# PROF=1 also runs lv:prof backboardd during the home swipes.
P=/c/Users/Vasyl/Desktop/Claude/Projects/Android-iOS
T="$P/ios-lavender/lavender/tools/usblog.py"
F="$P/dump/usblog-m45.txt"
LIVE="$P/dump/bench-live.txt"
D="$(dirname "$0")"
TAG=$1
say() { echo "$(date +%T) [$TAG] $*" | tee -a "$LIVE"; }
cmd() { python "$T" --cmd "$1" >/dev/null; }
shot() { python "$T" --shot "$TAG-$1" --build "$TAG"; }
# iOS turns the display off (the panel keeps the last picture: sweetfb_stayon) and touches do not wake it
awake() { [ "$(grep -a -o 'SWEET FB: display o[nf]*' "$F" | tail -1)" != "SWEET FB: display off" ]; }
wake() {
	if ! awake; then
		say "display is off (iOS auto-lock): power key"
		cmd "key power"; sleep 2
		awake || { cmd "key power"; sleep 2; }
		awake && say "display on" || say "display still off"
	fi
}
fps() { awk '{ if ($3 > 2) { n++; f += $3; g += $5; w += $9; if ($3 > m) m = $3 } } END { if (n) printf "fps %.1f max %d (n=%d) gpu %.0f%% wait %.0f ms/s", f/n, m, n, g/n, w/n; else print "no frames" }'; }
part() { tail -c +$1 "$F" | head -c $(($2 - $1)) | grep -a -o 'LVSTAT F [0-9]* G [0-9]* C [0-9]* W [0-9]* T [0-9]*' | fps; }
result() { say "RESULT $*"; echo "BENCH $TAG $*" >> "$P/dump/bench-results.txt"; }

s0=$(wc -c < "$F")
if [ "$3" != now ]; then
	say "waiting for the boot (first LVSTAT line)"
	for i in $(seq 1 90); do sleep 5; tail -c +$s0 "$F" | grep -a -q 'LVSTAT' && break; done
	say "iOS draws; 40 s for SpringBoard to settle"
	sleep 40
fi

say "unlocking"
HOME=0
for try in $(seq 1 10); do
	wake
	SH=$(shot home)
	python "$D/ishome.py" "$SH" >/dev/null && { HOME=1; break; }
	say "try $try: not the home screen ($(basename "$SH")): swipe up"
	cmd "swipe 540 2300 540 900 250"; sleep 4
	cmd "key home"; sleep 3
	SH=$(shot home)
	python "$D/ishome.py" "$SH" >/dev/null && { HOME=1; break; }
	sleep 10
done
[ "$HOME" = 1 ] || { result "NOT UNLOCKED"; exit 1; }
say "home screen ($(basename "$SH")): 6 slow page swipes"
s1=$(wc -c < "$F")
[ "$2" = 1 ] && cmd "prof backboardd 12"
for i in 1 2 3; do cmd "swipe 950 1200 150 1200 2000"; sleep 2.6; cmd "swipe 150 1200 950 1200 2000"; sleep 2.6; done
sleep 1
s2=$(wc -c < "$F")
H=$(part $s1 $s2)
say "home: $H"

INSET=0
for try in 1 2 3; do
	wake
	cmd "key home"; sleep 2
	say "opening Settings (try $try)"
	cmd "tap 675 594"; sleep 4
	SH=$(shot settings)
	python "$D/isapp.py" "$SH" >/dev/null && { INSET=1; break; }
	say "not in Settings ($(basename "$SH"))"
done
if [ "$INSET" = 1 ]; then
	say "in Settings ($(basename "$SH")): 6 slow scrolls"
	s3=$(wc -c < "$F")
	for i in 1 2 3; do cmd "swipe 540 2000 540 500 2000"; sleep 2.6; cmd "swipe 540 500 540 2000 2000"; sleep 2.6; done
	sleep 1
	s4=$(wc -c < "$F")
	S=$(part $s3 $s4)
else
	S="NOT IN SETTINGS"
fi
say "settings: $S"
result "home: $H | settings: $S"
[ "$2" = 1 ] && { sleep 3; tail -c +$s1 "$F" | grep -a 'PROF' | head -100; }
cmd "key home"; sleep 1
