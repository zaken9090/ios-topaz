#!/bin/bash
# Build the iOS 16.7.16 root image for lavender in WSL (topaz README step 2).
#   wsl -d Ubuntu -u root -- bash wsl-mkroot.sh REPO_DIR OUT_DIR
# REPO_DIR: ios-lavender checkout (Windows path under /mnt/c), OUT_DIR: where the
# sparse image for `fastboot flash userdata` goes. Needs hfsplus.ko loaded
# (wsl-build-hfsplus.sh), apfs-fuse, hfsprogs, img2simg, /root/ipsw/*.dmg.
set -euo pipefail
repo=$1
out=$2
work=/root/work
mkdir -p $work /mnt/iosroot "$out"
grep -qw hfsplus /proc/filesystems || modprobe hfsplus || modprobe --force-modversion hfsplus
cd $work
rm -f ios16-root.img ios16-root.simg
time bash "$repo/tools/mkiosroot.sh" /root/ipsw/os.dmg /root/ipsw/systemos.dmg /root/ipsw/appos.dmg ios16-root.img 16G
fsck.hfsplus -fy ios16-root.img || true
mount -t hfsplus -o loop,force ios16-root.img /mnt/iosroot
python3 -I "$repo/tools/sweet-root-tweaks.py" /mnt/iosroot
ls /mnt/iosroot | head -20
du -sh /mnt/iosroot 2>/dev/null || true
umount /mnt/iosroot
fsck.hfsplus -fy ios16-root.img || true
img2simg ios16-root.img ios16-root.simg
sha256sum ios16-root.img > ios16-root.img.sha256
ls -la ios16-root.img ios16-root.simg
cp ios16-root.simg ios16-root.img.sha256 "$out/"
ls -la "$out"
echo ROOT_IMAGE_READY
