#!/bin/bash
# WSL (os.dmg at /mnt/os): copy the 375w-812h@3x wallpaper variants as 414w-896h@2x (XR mode) into $1.
OUT=${1:-/root/work/wp}; rm -rf "$OUT"; mkdir -p "$OUT"; cd /mnt/os/root/Library/Wallpaper/Collections
for d in */Wallpapers/*-375w-812h@3x~iphone.wallpaper; do
  nd=$(echo "$d" | sed 's/375w-812h@3x/414w-896h@2x/')
  (cd "$d" && find . -type f) | while read f; do nf=$(echo "$f" | sed 's/375w-812h@3x/414w-896h@2x/g'); mkdir -p "$OUT/$nd/$(dirname "$nf")"; cp "$d/$f" "$OUT/$nd/$nf"; done
done
cd "$OUT"; python3 -c "
import plistlib,glob
def fix(o):
    if isinstance(o,dict): return {k:fix(v) for k,v in o.items()}
    if isinstance(o,list): return [fix(v) for v in o]
    return o.replace('375w-812h@3x','414w-896h@2x') if isinstance(o,str) else o
for p in glob.glob('*/Wallpapers/*-414w-896h@2x~iphone.wallpaper/Wallpaper.plist'):
    plistlib.dump(fix(plistlib.load(open(p,'rb'))),open(p,'wb'),fmt=plistlib.FMT_BINARY)"
