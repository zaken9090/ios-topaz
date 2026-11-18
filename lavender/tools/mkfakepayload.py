#!/usr/bin/env python3
"""Loader self-test payload: same SWEETXNU layout as mkpayload.py, but the
"kernel" is mktest's tiny band painter (red/green/blue, then PSCI reset after
~30 s). If the bands appear after the shim's loader screen, the shim copied
the segment to phys_base, placed ADT/boot_args and jumped correctly.

  python mkfakepayload.py ADT OUT.bin
"""
import os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mktest import payload as band_painter  # noqa: E402
from mkadt_lavender import PHYS_BASE, MEM_SIZE  # noqa: E402

BOOT_LINE_LENGTH = 608
SEG_TABLE = 0x48 + BOOT_LINE_LENGTH
VIRT_BASE = 0xfffffff006000000        # 32 MB aligned, like a real kernel's
SEG_VA = VIRT_BASE + 0x4000

if __name__ == '__main__':
    adt_blob = open(sys.argv[1], 'rb').read()
    code = band_painter()
    code += b'\0' * (-len(code) & 0x3fff)
    hdr = bytearray(SEG_TABLE + 32)
    data_off = len(hdr)
    out = hdr + code
    adt_off = len(out)
    out += adt_blob + b'\0' * (-len(adt_blob) & 0xf)
    kernel_end = SEG_VA + len(code)
    struct.pack_into('<8sII', out, 0, b'SWEETXNU', 1, 1)
    struct.pack_into('<7Q', out, 0x10, VIRT_BASE, SEG_VA, adt_off, len(adt_blob), kernel_end, PHYS_BASE, MEM_SIZE)
    out[0x48:0x48 + 9] = b'fake=test'
    struct.pack_into('<4Q', out, SEG_TABLE, SEG_VA, len(code), data_off, len(code))
    open(sys.argv[2], 'wb').write(out)
    print(f'{sys.argv[2]}: {len(out)} bytes, segment -> PA {hex(PHYS_BASE + SEG_VA - VIRT_BASE)}')
