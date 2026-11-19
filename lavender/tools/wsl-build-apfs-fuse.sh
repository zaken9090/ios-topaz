#!/bin/bash
# Build apfs-fuse (github.com/sgan81/apfs-fuse) inside WSL Ubuntu, install to /usr/local/bin.
# Run as root:  wsl -d Ubuntu -u root -- bash /mnt/c/.../tools/wsl-build-apfs-fuse.sh
set -e
for c in mkfs.hfsplus fsck.hfsplus rsync unzip cmake g++ git; do
	printf '%s: %s\n' "$c" "$(command -v "$c" || echo MISSING)"
done
mkdir -p /opt/src
cd /opt/src
rm -rf apfs-fuse
git clone -q --recursive https://github.com/sgan81/apfs-fuse.git
cd apfs-fuse
git log --oneline -1
# GCC 15: <cstdint> is no longer pulled in indirectly
sed -i '1i #include <cstdint>' ApfsLib/PList.h
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DUSE_FUSE3=ON >/tmp/cm.log 2>&1 || { tail -20 /tmp/cm.log; exit 1; }
make -j8 >/tmp/mk.log 2>&1 || { tail -30 /tmp/mk.log; exit 1; }
install -m 755 apfs-fuse apfsutil /usr/local/bin/
ls -la /usr/local/bin/apfs-fuse /usr/local/bin/apfsutil
