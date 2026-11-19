#!/bin/bash
# List the IPSW and name its root / cryptex images from BuildManifest.plist.
#   wsl-ipsw-list.sh IPSW
set -e
ipsw=$1
unzip -l "$ipsw" | awk '$1 > 50000000 {print}'
work=/root/ipsw-meta
mkdir -p $work
unzip -o -q "$ipsw" BuildManifest.plist -d $work
python3 -I - "$work/BuildManifest.plist" <<'EOF'
import plistlib, sys
m = plistlib.load(open(sys.argv[1], 'rb'))
print('ProductVersion', m.get('ProductVersion'), m.get('ProductBuildVersion'))
seen = set()
for bi in m['BuildIdentities']:
    info = bi.get('Info', {})
    for k, v in bi['Manifest'].items():
        p = v.get('Info', {}).get('Path', '')
        if p.endswith('.dmg') and (k, p) not in seen:
            seen.add((k, p))
            print(f"{info.get('DeviceClass')} {info.get('Variant','')[:30]:30} {k:28} {p}")
EOF
