#!/usr/bin/env python3
"""Split the appended DTBs out of an Android boot image (Image.gz-dtb) and
print the properties Qualcomm ABL uses to pick one.

  python dtbinfo.py BOOT.img OUTDIR
"""
import os, struct, sys, zlib


def fdt_props(blob, wanted):
    """Root-node properties (name -> bytes) of a flattened device tree."""
    _, total, off_struct, off_str = struct.unpack_from('>4I', blob, 0)
    p, depth, out = off_struct, 0, {}
    while True:
        tok, = struct.unpack_from('>I', blob, p); p += 4
        if tok == 1:
            end = blob.index(b'\0', p); p = (end + 4) & ~3; depth += 1
        elif tok == 2:
            depth -= 1
            if depth == 0:
                break
        elif tok == 3:
            ln, no = struct.unpack_from('>II', blob, p); p += 8
            name = blob[off_str + no:blob.index(b'\0', off_str + no)].decode()
            if depth == 1 and name in wanted:
                out[name] = blob[p:p + ln]
            p = (p + ln + 3) & ~3
        elif tok == 4:
            pass
        elif tok == 9:
            break
    return out


def cells(b):
    return [struct.unpack_from('>I', b, i)[0] for i in range(0, len(b), 4)]


def main(img_path, outdir):
    d = open(img_path, 'rb').read()
    ks, = struct.unpack_from('<I', d, 8)
    page, = struct.unpack_from('<I', d, 36)
    kernel = d[page:page + ks]
    dec = zlib.decompressobj(16 + zlib.MAX_WBITS)
    dec.decompress(kernel)
    pos = len(kernel) - len(dec.unused_data)
    os.makedirs(outdir, exist_ok=True)
    n = 0
    while pos < len(kernel):
        pos = kernel.find(b'\xd0\x0d\xfe\xed', pos)
        if pos < 0:
            break
        total, = struct.unpack_from('>I', kernel, pos + 4)
        blob = kernel[pos:pos + total]
        pr = fdt_props(blob, {'model', 'compatible', 'qcom,msm-id', 'qcom,board-id', 'qcom,pmic-id'})
        open(os.path.join(outdir, f'dtb{n}.dtb'), 'wb').write(blob)
        print(f'dtb{n}: {total} bytes  model={pr.get("model", b"").rstrip(b"\0").decode()}')
        for k in ('qcom,msm-id', 'qcom,board-id', 'qcom,pmic-id'):
            if k in pr:
                print(f'    {k} = {[hex(c) for c in cells(pr[k])]}')
        n += 1
        pos += total
    print(f'{n} DTBs')


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
