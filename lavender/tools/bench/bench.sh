#!/bin/bash
# bench.sh TAG [PROF]: after a fresh boot - unlock, 10 home-page swipes, Settings + 8 scrolls;
# prints mean LVSTAT fps (frames that moved) for each part. PROF=1 also runs lv:prof during the home swipes.
P=/c/Users/Vasyl/Desktop/Claude/Projects/Android-iOS
T="$P/ios-lavender/lavender/tools/usblog.py"
F="$P/dump/usblog-m40.txt"
TAG=$1
cmd() { python "$T" --cmd "$1" >/dev/null; }
s0=$(wc -c < "$F")
if [ "$3" != now ]; then
for i in $(seq 1 60); do sleep 5; tail -c +$s0 "$F" | grep -a -q 'LVSTAT' && break; done
sleep 25
fi
cmd "swipe 540 2300 540 900 250"; sleep 4
python "$T" --shot "$TAG-home" --build "$TAG" >/dev/null
s1=$(wc -c < "$F")
[ "$2" = 1 ] && cmd "prof backboardd 12"
for i in 1 2 3; do cmd "swipe 950 1200 150 1200 2000"; sleep 2.6; cmd "swipe 150 1200 950 1200 2000"; sleep 2.6; done
sleep 1
s2=$(wc -c < "$F")
for try in 1 2 3; do
	cmd "key home"; sleep 2; cmd "tap 675 594"; sleep 4
	SH=$(python "$T" --shot "$TAG-settings" --build "$TAG")
	INSET=0; python "$(dirname "$0")/isapp.py" "$SH" >/dev/null && { INSET=1; break; }
done
s3=$(wc -c < "$F")
for i in 1 2 3; do cmd "swipe 540 2000 540 500 2000"; sleep 2.6; cmd "swipe 540 500 540 2000 2000"; sleep 2.6; done
sleep 1
s4=$(wc -c < "$F")
avg() { awk '{ if ($3 > 2) { n++; f += $3; g += $5; w += $9; if ($3 > m) m = $3 } } END { if (n) printf "fps %.1f max %d (n=%d) gpu %.0f%% wait %.0f ms/s", f/n, m, n, g/n, w/n; else print "no frames" }'; }
H=$(tail -c +$s1 "$F" | head -c $((s2-s1)) | grep -a -o 'LVSTAT F [0-9]* G [0-9]* C [0-9]* W [0-9]* T [0-9]*' | avg)
S=$(tail -c +$s3 "$F" | head -c $((s4-s3)) | grep -a -o 'LVSTAT F [0-9]* G [0-9]* C [0-9]* W [0-9]* T [0-9]*' | avg)
[ "$INSET" = 1 ] || S="NOT IN SETTINGS ($S)"
echo "BENCH $TAG home: $H | settings: $S"
echo "BENCH $TAG home: $H | settings: $S" >> "$P/dump/bench-results.txt"
[ "$2" = 1 ] && { sleep 3; tail -c +$s1 "$F" | grep -a 'PROF' | head -100; }
cmd "key home"; sleep 1
