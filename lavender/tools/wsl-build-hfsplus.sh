#!/bin/bash
# Build and load hfsplus.ko for the running WSL kernel (Microsoft's kernel has
# CONFIG_HFSPLUS_FS off; tools/mkiosroot.sh needs to loop-mount HFS+).
# Same source tag + the running /proc/config.gz, so module CRCs match.
#   wsl -d Ubuntu -u root -- bash wsl-build-hfsplus.sh
set -e
TAG=linux-msft-wsl-$(uname -r | sed 's/-microsoft-standard-WSL2//')
W=/root/wslk
export DEBIAN_FRONTEND=noninteractive
# Microsoft builds the WSL kernel with GCC 13; GCC 15 gives a different module_layout CRC
apt-get install -y -qq flex bison libelf-dev libssl-dev bc dwarves cpio gcc-13 >/tmp/apt3.log 2>&1
MK="make CC=gcc-13 HOSTCC=gcc-13"
mkdir -p $W
cd $W
if [ ! -d src ]; then
	curl -sSfL -o src.tar.gz "https://github.com/microsoft/WSL2-Linux-Kernel/archive/refs/tags/$TAG.tar.gz"
	ls -la src.tar.gz
	mkdir src
	tar -xzf src.tar.gz -C src --strip-components=1
	rm -f src.tar.gz
fi
cd src
zcat /proc/config.gz > .config
scripts/config --module HFSPLUS_FS
make mrproper >/dev/null
zcat /proc/config.gz > .config
scripts/config --module HFSPLUS_FS
$MK olddefconfig >/dev/null
grep -E '^CONFIG_HFSPLUS_FS|^CONFIG_LOCALVERSION=' .config
echo "kernelrelease: $($MK -s kernelrelease)   running: $(uname -r)"
time $MK -j"$(nproc)" >/tmp/kbuild.log 2>&1 || { tail -30 /tmp/kbuild.log; exit 1; }
ls -la fs/hfsplus/hfsplus.ko
modinfo fs/hfsplus/hfsplus.ko | grep -E 'vermagic|depends'
mkdir -p /lib/modules/$(uname -r)/extra
cp fs/hfsplus/hfsplus.ko /lib/modules/$(uname -r)/extra/
depmod -a 2>/dev/null || true
# same source and config: CRCs should match; MODULE_FORCE_LOAD=y allows the fallback
insmod fs/hfsplus/hfsplus.ko || { dmesg | tail -2; echo "trying --force-modversion"; modprobe --force-modversion hfsplus; }
grep -w hfsplus /proc/filesystems
echo HFSPLUS_LOADED
