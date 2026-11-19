#!/bin/bash
# Can this WSL kernel loop-mount HFS+ read-write? (needed by tools/mkiosroot.sh)
export DEBIAN_FRONTEND=noninteractive
apt-get install -y -qq android-sdk-libsparse-utils >/tmp/apt2.log 2>&1; echo "libsparse apt=$?"
command -v img2simg || echo "img2simg MISSING"
modprobe hfsplus 2>&1 | head -2
grep -w hfsplus /proc/filesystems || echo "hfsplus NOT in /proc/filesystems"
set -e
t=$(mktemp -d)
truncate -s 64M $t/test.img
mkfs.hfsplus -s -J -v Test $t/test.img >/dev/null
mkdir $t/m
if mount -t hfsplus -o loop,force $t/test.img $t/m; then
	echo hello > $t/m/file && sync && cat $t/m/file && echo "HFS+ loop mount read-write: OK"
	umount $t/m
else
	echo "HFS+ loop mount FAILED"
fi
rm -rf $t
