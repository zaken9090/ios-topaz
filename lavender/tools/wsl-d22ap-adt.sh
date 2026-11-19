#!/bin/bash
# Extract the iPhone X device tree (DeviceTree.d22ap.im4p, LZFSE) from the IPSW
# into REPO/lavender/research/ipsw-x/d22ap.adt for mkadt_lavender.py (/product).
#   wsl-d22ap-adt.sh IPSW REPO
set -euo pipefail
ipsw=$1
repo=$2
lz=/opt/src/apfs-fuse/3rdparty/lzfse
if [ ! -x /usr/local/bin/lzfse ]; then
	make -C $lz -s >/dev/null
	install -m 755 $lz/build/bin/lzfse /usr/local/bin/lzfse
fi
t=$(mktemp -d)
unzip -o -q "$ipsw" 'Firmware/all_flash/DeviceTree.d22ap.im4p' -d $t
python3 -I - "$t/Firmware/all_flash/DeviceTree.d22ap.im4p" "$t/dt.lzfse" <<'EOF'
import sys
b = open(sys.argv[1], 'rb').read()
i = b.find(b'bvx2')
assert i > 0, 'no LZFSE payload'
open(sys.argv[2], 'wb').write(b[i:])
print('payload at', i, 'size', len(b) - i)
EOF
mkdir -p "$repo/lavender/research/ipsw-x"
lzfse -decode -i $t/dt.lzfse -o "$repo/lavender/research/ipsw-x/d22ap.adt"
ls -la "$repo/lavender/research/ipsw-x/d22ap.adt"
rm -rf $t
