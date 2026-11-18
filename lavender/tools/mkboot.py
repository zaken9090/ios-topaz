#!/usr/bin/env python3
"""Wrap a raw arm64 Image + lavender DTB into a boot.img ABL accepts:
Image.gz with the DTB appended, Android boot image header v1.

  python mkboot.py Image DTB OUT.img
"""
import gzip, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mktest import bootimg, empty_cpio  # noqa: E402

if __name__ == '__main__':
    image, dtb, out = sys.argv[1:4]
    kernel = gzip.compress(open(image, 'rb').read(), 9, mtime=0) + open(dtb, 'rb').read()
    img = bootimg(kernel, empty_cpio())
    open(out, 'wb').write(img)
    print(f'{out}: {len(img)} bytes')
