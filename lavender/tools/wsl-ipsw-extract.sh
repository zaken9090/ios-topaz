#!/bin/bash
# Extract the root (OS) and cryptex images of the iPhone X 16.7.16 IPSW into WSL's own disk.
#   wsl-ipsw-extract.sh IPSW   ->  /root/ipsw/{os,systemos,appos}.dmg
set -e
ipsw=$1
out=/root/ipsw
mkdir -p $out
cd $out
unzip -o -q "$ipsw" 087-86698-044.dmg 087-86593-047.dmg 087-86715-047.dmg
mv -f 087-86698-044.dmg os.dmg
mv -f 087-86593-047.dmg systemos.dmg
mv -f 087-86715-047.dmg appos.dmg
ls -la $out
for d in os systemos appos; do
	mkdir -p /mnt/$d
	apfs-fuse -o allow_other $out/$d.dmg /mnt/$d && echo "$d mounted" || echo "$d: apfs-fuse FAILED"
done
ls /mnt/os | head
du -sh /mnt/systemos/System/Library/Caches/com.apple.dyld 2>/dev/null || find /mnt/systemos -maxdepth 4 -name 'dyld_shared_cache*' | head
umount /mnt/os /mnt/systemos /mnt/appos || true
