#!/bin/bash
# Put the GPU user space (userland CI artifact: libOSMesa.8.dylib, libtopaz-gl.dylib,
# libtopaz-inject.dylib) into /topaz/lib of the iOS root image, and the lavender GPU
# firmware into /usr/share/firmware/lavender, then make the sparse image to flash.
#   wsl -d Ubuntu -u root -- bash wsl-add-gpu-userland.sh LIBDIR FWDIR OUT_DIR
set -euo pipefail
libdir=$1
fwdir=$2
out=$3
work=/root/work
img=$work/ios16-root.img
grep -qw hfsplus /proc/filesystems || modprobe hfsplus || modprobe --force-modversion hfsplus
mkdir -p /mnt/iosroot "$out"
mount -t hfsplus -o loop,force "$img" /mnt/iosroot
mkdir -p /mnt/iosroot/topaz/lib /mnt/iosroot/usr/share/firmware/lavender
for f in libOSMesa.8.dylib libtopaz-gl.dylib libtopaz-inject.dylib; do
	install -m 755 -o 0 -g 0 "$libdir/$f" /mnt/iosroot/topaz/lib/$f
done
for f in a512_zap.mdt a512_zap.b02 a530_pm4.fw a530_pfp.fw; do
	install -m 644 -o 0 -g 0 "$fwdir/$f" /mnt/iosroot/usr/share/firmware/lavender/$f
done
ls -la /mnt/iosroot/topaz/lib /mnt/iosroot/usr/share/firmware/lavender
umount /mnt/iosroot
fsck.hfsplus -fy "$img" | tail -2 || true
rm -f $work/ios16-root.simg
img2simg "$img" $work/ios16-root.simg
cp $work/ios16-root.simg "$out/ios16-root-gpu.simg"
ls -la "$out/ios16-root-gpu.simg"
echo GPU_ROOT_READY
