#!/usr/bin/env python3
"""Minimal Apple Device Tree for XNU on the Redmi Note 7 (lavender, SDM660).

Adapted from ios-topaz tools/mkadt.py: same iPhone X (D22AP) identity, but
every hardware value is lavender's, measured with our probe shim
(dump/shim-probe-v1.md, v2.md) or taken from the lavender kernel DT:

  RAM          0x80000000 +2G, 0x100000000 +0x7e1c0000 (DTB from ABL)
  carve-outs   all below 0xa0400000 -> the kernel gets the upper bank
  GICv3        GICD 0x17a00000, GICR 0x17b00000 (8 x 0x20000), GIC-500
  timer        19.2 MHz, virtual timer INTID 19 (Qualcomm PPI 3)
  CPUs         Gold A73-class MPIDR 0x100-0x103 (16K granule), Silver parked
  framebuffer  ABL splash 0x9d400000, 1080x2340, stride 4320, a8r8g8b8

Storage is eMMC (sdhci@c0c4000), not UFS: no storage node yet, the first
milestone boots to the root-device wait / a ramdisk.

  python mkadt_lavender.py OUT.adt [--dump]
"""
import hashlib, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adt  # noqa: E402

# Kernel-managed RAM: the whole upper DDR bank (rounded down to 32 MB)
PHYS_BASE = 0x100000000
MEM_SIZE = 0x7e000000
# /arm-io children are relative to this; it must not be 0 (serial_init bails)
SOC_BASE = 0x01000000

GICD, GICD_SIZE = 0x17a00000, 0x10000
GICR, GICR_SIZE = 0x17b00000, 0x100000
FB_W, FB_H, FB_STRIDE = 1080, 2340, 1080 * 4
EMMC, EMMC_SIZE = 0x0c0c4000, 0x1000         # sdhc_1 hc_mem (sdm660.dtsi)
EMMC_CQ, EMMC_CQ_SIZE = 0x0c0c5000, 0x1000   # sdhc_1 cmdq_mem
FB, FB_SIZE = 0x9d400000, 0x9a4000          # 1080*2340*4 rounded up to 16K
TIMEBASE = 19200000
VTIMER_INTID = 19
GOLD_MPIDR = (0x100, 0x101, 0x102, 0x103)
SWEET_ECID = 0x4c4156454e000001             # 'LAVEN' + 1, synthetic

BOOT_MANIFEST_HASH = hashlib.sha384(b'lavender boot manifest').digest()

# /product of the real iPhone X tree (Apple artifact, not in the repo); see
# ios-topaz README step 2. Optional for the first kernel boots.
D22AP_ADT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../research/ipsw-x/d22ap.adt')

u32 = lambda *v: struct.pack('<%dI' % len(v), *v)
u64 = lambda *v: struct.pack('<%dQ' % len(v), *v)
cstr = lambda s: s.encode() + b'\0'


def d22ap_product(path=D22AP_ADT):
    if not os.path.exists(path):
        print(f'warning: {path} missing, no /product node', file=sys.stderr)
        return None
    root, _ = adt.parse(open(path, 'rb').read())
    for c in root.children:
        if dict((k, v) for k, f, v in c.props).get('name') == b'product\0':
            return [(k, v) for k, f, v in c.props if not f and k not in ('name', 'AAPL,phandle')]
    raise SystemExit(f'{path}: no /product node')


def node(name, props=(), children=()):
    n = adt.Node()
    n.props = [('name', 0, cstr(name))] + [(k, 0, v) for k, v in props]
    n.children = list(children)
    return n


def build(seed=None):
    seed = seed if seed is not None else os.urandom(64)
    phandle = iter(range(1, 1000))
    ph = lambda: ('AAPL,phandle', u32(next(phandle)))

    chosen = node('chosen', [
        ph(),
        ('dram-base', u64(PHYS_BASE)),
        ('dram-size', u64(MEM_SIZE)),
        ('random-seed', seed),
        ('debug-enabled', u32(1)),
        ('firmware-version', cstr('lavender-shim')),
        ('system-firmware-version', cstr('lavender-shim')),
        ('boot-manifest-hash', BOOT_MANIFEST_HASH),
        ('crypto-hash-method', cstr('sha2-384')),
        # 1080x2340 @3x = 360x780 points
        ('display-scale', u32(3)),
        ('unique-chip-id', struct.pack('<Q', SWEET_ECID)),
        ('chip-id', u32(0x8015)),
        ('certificate-production-status', u32(0)),
        ('effective-production-status-ap', u32(0)),
        ('certificate-security-mode', u32(1)),
        ('effective-security-mode-sep', u32(1)),
    ], [node('memory-map', [ph()]),
        node('asmb', [ph(), ('lp-sip0', u64(0x6f)), ('lp-sip1', u32(1))])])

    cpu_nodes = []
    for core, mpidr in enumerate(GOLD_MPIDR):
        cpu_nodes.append(node(f'cpu{core}', [
            ph(),
            ('device_type', cstr('cpu')),
            # qcom,kryo470 stays: sweet's CPU driver IONameMatch uses it
            ('compatible', cstr('qcom,kryo260') + cstr('qcom,kryo470') + cstr('ARM,v8')),
            ('reg', u32(mpidr)),
            ('cpu-id', u32(core)),
            ('cluster-id', u32(0)),
            ('cluster-type', u32(0)),
            ('die-cluster-id', u32(0)),
            ('cluster-core-id', u32(core)),
            ('l2-cache-id', u32(0)),
            ('l3-cache-id', u32(0)),
            ('state', cstr('running' if core == 0 else 'stopped')),
            ('timebase-frequency', u32(TIMEBASE)),
            ('clock-frequency', u32(2208000000)),
            ('bus-frequency', u32(100000000)),
            ('memory-frequency', u32(1866000000)),
            ('peripheral-frequency', u32(TIMEBASE)),
            ('fixed-frequency', u32(TIMEBASE)),
        ]))
    cpus = node('cpus', [ph(), ('#address-cells', u32(1)), ('#size-cells', u32(0))], cpu_nodes)

    gic = node('gic', [
        ph(),
        ('compatible', cstr('arm,gic-v3')),
        ('interrupt-controller', b''),
        ('virtual-timer-intid', u32(VTIMER_INTID)),
        ('reg', u64(GICD - SOC_BASE, GICD_SIZE, GICR - SOC_BASE, GICR_SIZE)),
    ])
    arm_io = node('arm-io', [
        ph(),
        ('device_type', cstr('sweet-io')),
        ('compatible', cstr('sweet-io') + cstr('arm-io')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
        ('ranges', u64(0, SOC_BASE, 0x100000000 - SOC_BASE)),
    ], [gic,
        node('wdt', [ph(), ('no-pmu', u32(1))]),
        node('disp0', [
            ph(),
            ('device_type', cstr('display-subsystem')),
            ('compatible', cstr('sweet,display')),
            ('canvas-width', u32(1125)),
            ('canvas-height', u32(2436)),
        ])])

    fbcon = node('sweet-fbcon', [
        ph(),
        ('reg', u64(FB, FB_SIZE)),
        ('width', u32(FB_W)),
        ('height', u32(FB_H)),
        ('stride', u32(FB_STRIDE)),
    ])
    # lavender: eMMC host sdhc_1 for the ufsroot0 root device (xnu patch 0991):
    # hc_mem (standard SDHCI registers), then cmdq_mem (only read, to check CQE is off)
    emmc = node('sweet-ufs', [
        ph(),
        ('compatible', cstr('qcom,sdhci-msm-v5')),
        ('reg', u64(EMMC, EMMC_SIZE, EMMC_CQ, EMMC_CQ_SIZE)),
    ])
    defaults = node('defaults', [ph(), ('no-effaceable-storage', u32(1))])
    buttons = node('buttons', [
        ph(),
        ('button-names', cstr('volup') + cstr('voldown') + cstr('hold')),
        ('home-button-type', u32(2)),
        ('opposed-power-vol-buttons', u32(1)),
    ])
    product_props = d22ap_product()
    if product_props:
        product_props = [(k, cstr('sRGB') if k == 'artwork-display-gamut' else v) for k, v in product_props]
    product = [node('product', [ph()] + product_props)] if product_props else []

    return node('device-tree', [
        ph(),
        ('compatible', cstr('D22AP') + cstr('iPhone10,3') + cstr('xiaomi,lavender') + cstr('xiaomi,sweet') +
         cstr('qcom,sdm660') + cstr('AppleARM')),
        ('model', cstr('iPhone10,3')),
        ('target-type', cstr('D22')),
        ('target-sub-type', cstr('D22AP')),
        ('manufacturer', cstr('Apple Inc.')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
    ], [chosen, defaults, cpus, arm_io, fbcon, emmc, buttons] + product)


if __name__ == '__main__':
    blob = adt.serialize(build())
    open(sys.argv[1], 'wb').write(blob)
    if '--dump' in sys.argv:
        adt.dump(adt.parse(blob)[0])
    print(f'{sys.argv[1]}: {len(blob)} bytes', file=sys.stderr)
