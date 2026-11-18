#!/usr/bin/env python3
"""Test boot image for lavender: proves `fastboot boot` reaches our code.

The payload paints the ABL continuous-splash framebuffer (0x9d400000,
1080x2340, a8r8g8b8) with three bands: red / green / blue (top to bottom),
waits ~30 s and resets the phone via PSCI SYSTEM_RESET. Nothing is flashed:
the image only lives in RAM.

Packing: arm64 Image (gzip) + appended DTB = Image.gz-dtb, Android boot
image header v1, like the stock lavender kernel.

  python mktest.py OUT.img
"""
import gzip, hashlib, struct, sys

FB, W, H = 0x9d400000, 1080, 2340
COLORS = [0xFFFF0000, 0xFF00FF00, 0xFF0000FF]  # a8r8g8b8 little endian
WAIT_TICKS = 30 * 19_200_000                     # arch timer is 19.2 MHz
PSCI_SYSTEM_RESET = 0x84000009


# ---------------- tiny AArch64 assembler (only what we need) ----------------
class Asm:
    def __init__(self):
        self.code, self.lits, self.fix = [], [], []

    def emit(self, w):
        self.code.append(w & 0xFFFFFFFF)

    def here(self):
        return len(self.code)

    def ldr_lit(self, rt, value, wide=True):        # LDR Xt/Wt, =value
        self.fix.append((self.here(), len(self.lits), wide))
        self.lits.append((value, wide))
        self.emit((0x58000000 if wide else 0x18000000) | rt)

    def b_cond(self, cond, target):                  # B.cond target
        off = (target - self.here()) & 0x7FFFF
        self.emit(0x54000000 | off << 5 | cond)

    def b(self, target):
        self.emit(0x14000000 | ((target - self.here()) & 0x3FFFFFF))

    def finish(self):
        # literal pool after code, 8-byte aligned
        if len(self.code) % 2:
            self.emit(0xD503201F)                    # NOP
        pool, offs = bytearray(), []
        for value, wide in self.lits:
            offs.append(len(self.code) * 4 + len(pool))
            pool += struct.pack('<Q', value)         # 8 bytes even for W loads
        for idx, lit, wide in self.fix:
            imm19 = (offs[lit] - idx * 4) // 4
            self.code[idx] |= (imm19 & 0x7FFFF) << 5
        return b''.join(struct.pack('<I', w) for w in self.code) + pool


def payload():
    a = Asm()
    a.emit(0xD5034FDF)                               # msr daifset, #0xf
    a.ldr_lit(1, FB)                                 # x1 = framebuffer
    band = W * (H // 3)
    for i, c in enumerate(COLORS):
        a.ldr_lit(2, band if i < 2 else W * H - 2 * band)
        a.ldr_lit(3, c, wide=False)
        loop = a.here()
        a.emit(0xB8004423)                           # str w3, [x1], #4
        a.emit(0xF1000442)                           # subs x2, x2, #1
        a.b_cond(1, loop)                            # b.ne loop
    a.emit(0xD5033F9F)                               # dsb sy
    a.emit(0xD53BE024)                               # mrs x4, cntpct_el0
    a.ldr_lit(5, WAIT_TICKS)
    a.emit(0x8B050085)                               # add x5, x4, x5
    wait = a.here()
    a.emit(0xD53BE024)                               # mrs x4, cntpct_el0
    a.emit(0xEB05009F)                               # cmp x4, x5
    a.b_cond(3, wait)                                # b.lo wait
    a.ldr_lit(0, PSCI_SYSTEM_RESET)
    a.emit(0xD4000003)                               # smc #0
    hang = a.here()
    a.emit(0xD503205F)                               # wfe
    a.b(hang)
    return a.finish()


def arm64_image():
    body = payload()
    size = 0x10000
    hdr = struct.pack('<II QQQ QQQ 4sI',
                      0x14000010, 0,                 # b +0x40 (skip header)
                      0x80000, size, 0xA,            # text_offset, image_size, flags(LE, 4K)
                      0, 0, 0, b'ARM\x64', 0)
    img = hdr + body
    return img + b'\0' * (size - len(img))


# ---------------- minimal flattened device tree writer ----------------
def fdt(nodes_fn):
    struct_blk, strings, str_off = bytearray(), bytearray(), {}

    def tok(t):
        struct_blk.extend(struct.pack('>I', t))

    def pad():
        struct_blk.extend(b'\0' * (-len(struct_blk) & 3))

    def begin(name):
        tok(1); struct_blk.extend(name.encode() + b'\0'); pad()

    def end():
        tok(2)

    def prop(name, val):
        if isinstance(val, str):
            val = val.encode() + b'\0'
        elif isinstance(val, list) and val and isinstance(val[0], str):
            val = b''.join(s.encode() + b'\0' for s in val)
        elif isinstance(val, list):
            val = b''.join(struct.pack('>I', v) for v in val)
        if name not in str_off:
            str_off[name] = len(strings); strings.extend(name.encode() + b'\0')
        tok(3); struct_blk.extend(struct.pack('>II', len(val), str_off[name])); struct_blk.extend(val); pad()

    nodes_fn(begin, end, prop)
    tok(9)
    rsv = b'\0' * 16
    off_rsv = 40
    off_struct = off_rsv + len(rsv)
    off_str = off_struct + len(struct_blk)
    total = off_str + len(strings)
    total += -total & 7
    hdr = struct.pack('>10I', 0xD00DFEED, total, off_struct, off_str, off_rsv, 17, 16, 0,
                      len(strings), len(struct_blk))
    blob = hdr + rsv + bytes(struct_blk) + bytes(strings)
    return blob + b'\0' * (total - len(blob))


def dtb():
    def nodes(begin, end, prop):
        begin('')
        prop('#address-cells', [2]); prop('#size-cells', [2])
        prop('model', 'lavender fastboot-boot test')
        prop('compatible', ['xiaomi,lavender', 'qcom,sdm660'])
        prop('qcom,msm-id', [317, 0x10000])
        # every platform type with default subtype 0: ABL picks the one the board reports
        ids = []
        for t in range(0, 48):
            ids += [t, 0]
        prop('qcom,board-id', ids)
        begin('chosen'); prop('bootargs', ''); end()
        begin('memory'); prop('device_type', 'memory'); prop('reg', [0, 0x80000000, 0, 0]); end()
        end()
    return fdt(nodes)


# ---------------- empty initramfs + boot.img v1 ----------------
def empty_cpio():
    name = b'TRAILER!!!\0'
    h = b'070701' + b''.join(b'%08X' % v for v in (0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, len(name), 0))
    d = h + name
    d += b'\0' * (-len(d) & 3)
    d += b'\0' * (-len(d) & 511)
    return gzip.compress(d, 9, mtime=0)


def bootimg(kernel, ramdisk, page=4096):
    def padp(b):
        return b + b'\0' * (-len(b) % page)
    sha = hashlib.sha1()
    for blob in (kernel, ramdisk, b''):
        sha.update(blob); sha.update(struct.pack('<I', len(blob)))
    sha.update(b''); sha.update(struct.pack('<I', 0))   # recovery dtbo (v1)
    os_version = (10 << 25) | ((2021 - 2000) << 4) | 8   # Android 10, 2021-08
    hdr = struct.pack('<8s10I16s512s32s1024sIQI',
                      b'ANDROID!', len(kernel), 0x00008000, len(ramdisk), 0x01000000,
                      0, 0x00F00000, 0x00000100, page, 1, os_version,
                      b'', b'console=null', sha.digest(), b'',
                      0, 0, 1648)
    return padp(hdr) + padp(kernel) + padp(ramdisk)


if __name__ == '__main__':
    out = sys.argv[1] if len(sys.argv) > 1 else 'lavender-test.img'
    # optional 2nd arg: a real lavender DTB (e.g. split out of a working boot.img)
    dt = open(sys.argv[2], 'rb').read() if len(sys.argv) > 2 else dtb()
    kernel = gzip.compress(arm64_image(), 9, mtime=0) + dt
    img = bootimg(kernel, empty_cpio())
    open(out, 'wb').write(img)
    print(f'{out}: {len(img)} bytes (kernel.gz-dtb {len(kernel)} bytes)')
