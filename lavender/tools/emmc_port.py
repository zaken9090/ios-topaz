#!/usr/bin/env python3
"""Turn topaz's bsd/dev/sweet_ufs.c into the lavender eMMC (SDHCI) root device.

  emmc_port.py sweet_ufs.c        (rewrites the file in place)

Only the hardware layer changes: ufs_scsi / ufs_read_blocks / ufs_write_blocks /
ufs_sync_cache / ufs_take_over_host become polled SDHCI PIO commands to the
eMMC user area. GPT lookup, the ufsroot0 BSD disk, rd=ufsroot0, the write
fences and the console log stay as topaz wrote them. Used to (re)generate
xnu/patches/0991-lavender-emmc.patch against the patched tree from CI.
"""
import sys

path = sys.argv[1]
src = open(path, newline='\n').read()


def cut(text, start, end, new, keep_end=True):
    i = text.index(start)
    j = text.index(end, i)
    assert text.count(start) == 1, start
    return text[:i] + new + (text[j:] if keep_end else text[j + len(end):])


def rep(text, old, new):
    assert text.count(old) == 1, old[:60]
    return text.replace(old, new)


# --- header -----------------------------------------------------------------
src = rep(src, "/*\n * SWEET internal UFS root device.\n", """/*
 * lavender (Redmi Note 7, SDM660): the same root device on the eMMC.
 * sdhc_1 (Qualcomm sdhci-msm v5, standard SDHCI registers at 0xc0c4000) is
 * left powered and clocked by ABL with the card selected in the user area.
 * Commands are polled and data moves by PIO through the buffer data port
 * into the bounce buffer (no DMA, so no cache maintenance). Device blocks are
 * the eMMC's 512-byte sectors, which is also the GPT's LBA unit. Everything
 * below the hardware layer (GPT lookup, ufsroot0, write fences, sweetlog) is
 * topaz's code unchanged.
 *
 * Original topaz description:
 * SWEET internal UFS root device.
""")

# --- registers, sizes, DMA layout --------------------------------------------
src = cut(src, "#define REG_CAP ", "static struct {\n", """#define SD_BLKSIZE      0x04            /* [15:0] block size, [31:16] block count */
#define SD_ARG          0x08
#define SD_CMD          0x0c            /* [15:0] transfer mode, [31:16] command */
#define SD_RESP0        0x10
#define SD_BUFFER       0x20
#define SD_PRESENT      0x24
#define SD_CLOCK        0x2c            /* [15:0] clock, [23:16] timeout, [31:24] reset */
#define SD_INT_STATUS   0x30            /* [15:0] normal, [31:16] error */
#define SD_INT_ENABLE   0x34
#define SD_INT_SIGNAL   0x38
#define SD_CAP          0x40
#define CQ_CFG          0x08            /* cmdq_mem: CQE enable is bit 0 */

#define PRES_CMD_INHIBIT        (1u << 0)
#define PRES_DAT_INHIBIT        (1u << 1)
#define PRES_BUF_WR             (1u << 10)
#define PRES_BUF_RD             (1u << 11)
#define CLK_SD_EN               (1u << 2)
#define RST_CMD                 (1u << 25)
#define RST_DAT                 (1u << 26)
#define INT_CMD_DONE            (1u << 0)
#define INT_XFER_DONE           (1u << 1)
#define INT_BUF_WR              (1u << 4)
#define INT_BUF_RD              (1u << 5)
#define INT_ERROR               (1u << 15)
#define INT_ERRORS              0xffff0000u
#define INT_ENABLE_ALL          0x03ff003fu     /* normal 0-5, errors 16-25 */

/* transfer mode */
#define TM_BLKCNT       (1u << 1)
#define TM_ACMD12       (1u << 2)
#define TM_READ         (1u << 4)
#define TM_MULTI        (1u << 5)
/* command register flags: response 48 / 48 busy, CRC and index checks, data */
#define CF_R1           0x1a
#define CF_R1B          0x1b
#define CF_DATA         0x20
/* R1 card status bits that mean the command failed (Linux CMD_ERRORS) */
#define R1_ERRORS       0xe4300000u

#define UFS_BLOCK       512u            /* lavender: eMMC sector = GPT LBA */
#define BOUNCE_SIZE     (1u << 20)
#define CMD_TIMEOUT_NS  (5ull * NSEC_PER_SEC)

/* Layout of the contiguous allocation: only the bounce buffer (PIO, no DMA). */
#define DMA_BOUNCE      0x0000
#define DMA_SIZE        BOUNCE_SIZE

""")

src = rep(src, "\tuint8_t lun;\n\tuint8_t tag;\n", "\tvm_offset_t cq;                         /* lavender: cmdq_mem, 0 if not in the ADT */\n\tbool emmc_cache;                        /* EXT_CSD CACHE_CTRL: flush on sync */\n\tuint8_t lun;\n\tuint8_t tag;\n")

# --- low-level access -----------------------------------------------------------
src = cut(src, "static inline uint32_t\nufs_rd(", "/* READ(10) of count device blocks", """static inline uint32_t
ufs_rd(uint32_t off)
{
	return *(volatile uint32_t *)(su.regs + off);
}

static inline void
ufs_wr(uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(su.regs + off) = v;
}

static uint64_t
sd_deadline(void)
{
	uint64_t d;
	nanoseconds_to_absolutetime(CMD_TIMEOUT_NS, &d);
	return d + mach_absolute_time();
}

/*
 * Wait for any of the interrupt status bits in want, or for the present state
 * bits in pmask to read as pval. lavender (2026-11-19, first phone test): the
 * interrupt status stayed 0 while the present state showed the data in the
 * buffer, so the present state is what the driver relies on; error status bits
 * still fail the command. 0, EIO on an error status, or ETIMEDOUT.
 */
static int
sd_wait(uint32_t want, uint32_t pmask, uint32_t pval, uint64_t deadline, uint32_t *status)
{
	for (;;) {
		uint32_t s = ufs_rd(SD_INT_STATUS);
		if (s & (INT_ERROR | INT_ERRORS)) {
			*status = s;
			return EIO;
		}
		if ((s & want) || (pmask && (ufs_rd(SD_PRESENT) & pmask) == pval)) {
			*status = s;
			return 0;
		}
		if (mach_absolute_time() > deadline) {
			*status = s;
			return ETIMEDOUT;
		}
		delay(1);
	}
}

static void
sd_reset_lines(void)
{
	ufs_wr(SD_CLOCK, ufs_rd(SD_CLOCK) | RST_CMD | RST_DAT);
	uint64_t deadline = sd_deadline();
	while ((ufs_rd(SD_CLOCK) & (RST_CMD | RST_DAT)) && mach_absolute_time() < deadline) {
		delay(1);
	}
	ufs_wr(SD_INT_STATUS, 0xffffffffu);
}

/*
 * One command. With blocks != 0, data moves between the bounce buffer and the
 * card by PIO (to the card when write). R1 status is checked. 0 or an errno.
 */
static int
sd_cmd(uint32_t idx, uint32_t arg, uint32_t flags, uint32_t blocks, bool write)
{
	uint64_t deadline = sd_deadline();
	uint32_t s = 0;
	int error = 0;

	while (ufs_rd(SD_PRESENT) & (PRES_CMD_INHIBIT | (blocks || flags == CF_R1B ? PRES_DAT_INHIBIT : 0))) {
		if (mach_absolute_time() > deadline) {
			printf("SWEET EMMC: CMD%u: lines busy (present 0x%x)\\n", idx, ufs_rd(SD_PRESENT));
			sd_reset_lines();
			return ETIMEDOUT;
		}
		delay(1);
	}
	ufs_wr(SD_INT_STATUS, 0xffffffffu);
	uint32_t mode = 0;
	if (blocks) {
		flags |= CF_DATA;
		mode = TM_BLKCNT | (write ? 0 : TM_READ) | (blocks > 1 ? TM_MULTI | TM_ACMD12 : 0);
		ufs_wr(SD_BLKSIZE, blocks << 16 | UFS_BLOCK);
	}
	ufs_wr(SD_ARG, arg);
	/* transfer mode and command in one 32-bit write: the command write starts it */
	ufs_wr(SD_CMD, (idx << 8 | flags) << 16 | mode);

	error = sd_wait(INT_CMD_DONE, PRES_CMD_INHIBIT, 0, deadline, &s);
	if (!error) {
		ufs_wr(SD_INT_STATUS, INT_CMD_DONE);
		uint32_t r1 = ufs_rd(SD_RESP0);
		if (r1 & R1_ERRORS) {
			printf("SWEET EMMC: CMD%u arg 0x%x: card status 0x%x\\n", idx, arg, r1);
			error = EIO;
		}
	}
	uint32_t *buf = (uint32_t *)(su.dma + DMA_BOUNCE);
	for (uint32_t b = 0; !error && b < blocks; b++) {
		error = sd_wait(write ? INT_BUF_WR : INT_BUF_RD, write ? PRES_BUF_WR : PRES_BUF_RD,
		    write ? PRES_BUF_WR : PRES_BUF_RD, deadline, &s);
		if (error) {
			break;
		}
		ufs_wr(SD_INT_STATUS, write ? INT_BUF_WR : INT_BUF_RD);
		uint32_t *p = buf + b * (UFS_BLOCK / 4);
		if (write) {
			for (unsigned i = 0; i < UFS_BLOCK / 4; i++) {
				ufs_wr(SD_BUFFER, p[i]);
			}
		} else {
			for (unsigned i = 0; i < UFS_BLOCK / 4; i++) {
				p[i] = ufs_rd(SD_BUFFER);
			}
		}
	}
	if (!error && (blocks || flags == CF_R1B)) {
		error = sd_wait(INT_XFER_DONE, PRES_DAT_INHIBIT, 0, deadline, &s);
	}
	ufs_wr(SD_INT_STATUS, 0xffffffffu);
	if (error) {
		printf("SWEET EMMC: CMD%u arg 0x%x blocks %u failed: %d, int status 0x%x present 0x%x\\n",
		    idx, arg, blocks, error, s, ufs_rd(SD_PRESENT));
		sd_reset_lines();
	}
	return error;
}

""")

# --- block I/O ------------------------------------------------------------------
src = cut(src, "/* READ(10) of count device blocks", "/*\n * topaz: on 2026-10-19", """/* lavender: CMD17/CMD18 (auto CMD12) of count sectors at lba into the bounce buffer. */
static int
ufs_read_blocks(uint64_t lba, uint32_t count)
{
	if (lba > UINT32_MAX || count == 0 || count > BOUNCE_SIZE / UFS_BLOCK) {
		return EINVAL;
	}
	return sd_cmd(count > 1 ? 18 : 17, (uint32_t)lba, CF_R1, count, false);
}

""")
src = rep(src, """	uint8_t cdb[10] = {
		0x2a, 0,
		(uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba,
		0, (uint8_t)(count >> 8), (uint8_t)count, 0
	};
	return ufs_scsi(cdb, sizeof(cdb), count * UFS_BLOCK, true);
}""", """	/* lavender: CMD24/CMD25 (auto CMD12) */
	return sd_cmd(count > 1 ? 25 : 24, (uint32_t)lba, CF_R1, count, true);
}""")
src = rep(src, """	static const uint8_t cdb[10] = { 0x35 };       /* SYNCHRONIZE CACHE(10), whole LUN */
	return ufs_scsi(cdb, sizeof(cdb), 0, false);""", """	/* lavender: CMD6 SWITCH FLUSH_CACHE (EXT_CSD[32] = 1), only with the cache on */
	if (!su.emmc_cache) {
		return 0;
	}
	return sd_cmd(6, 3u << 24 | 32u << 16 | 1u << 8, CF_R1B, 0, false);""")

# --- take over the host -------------------------------------------------------------
src = cut(src, "static int\nufs_take_over_host(void)\n{", "static int\nsweet_ufs_open(", """static int
ufs_take_over_host(void)
{
	uint32_t clk = ufs_rd(SD_CLOCK), pres = ufs_rd(SD_PRESENT);
	printf("SWEET EMMC: cap 0x%x present 0x%x clock 0x%x int enable 0x%x\\n",
	    ufs_rd(SD_CAP), pres, clk, ufs_rd(SD_INT_ENABLE));
	if (!(clk & CLK_SD_EN)) {
		printf("SWEET EMMC: card clock is off: ABL left no usable host\\n");
		return ENXIO;
	}
	if (su.cq && (*(volatile uint32_t *)(su.cq + CQ_CFG) & 1)) {
		printf("SWEET EMMC: command queue engine enabled (CQCFG 0x%x): not supported\\n",
		    *(volatile uint32_t *)(su.cq + CQ_CFG));
		return ENXIO;
	}
	ufs_wr(SD_INT_SIGNAL, 0);               /* polled operation */
	ufs_wr(SD_INT_ENABLE, INT_ENABLE_ALL);
	if (pres & (PRES_CMD_INHIBIT | PRES_DAT_INHIBIT)) {
		sd_reset_lines();
	}
	ufs_wr(SD_INT_STATUS, 0xffffffffu);
	printf("SWEET EMMC: int enable now 0x%x, int status 0x%x\\n",
	    ufs_rd(SD_INT_ENABLE), ufs_rd(SD_INT_STATUS));

	/* CMD8 SEND_EXT_CSD: proves the card answers, and which area is selected. */
	int error = sd_cmd(8, 0, CF_R1, 1, false);
	if (error) {
		return error;
	}
	const uint8_t *x = (const uint8_t *)(su.dma + DMA_BOUNCE);
	uint32_t sectors = (uint32_t)x[212] | (uint32_t)x[213] << 8 | (uint32_t)x[214] << 16 | (uint32_t)x[215] << 24;
	printf("SWEET EMMC: EXT_CSD rev %u, %u MiB, partition config 0x%x, bus width %u, hs timing %u, cache %u, cmdq %u\\n",
	    x[192], sectors >> 11, x[179], x[183], x[185], x[33], x[15]);
	if (x[15] & 1) {
		printf("SWEET EMMC: card is in command queue mode: not supported\\n");
		return ENXIO;
	}
	if (x[179] & 7) {
		/* Never switched here: PARTITION_CONFIG also holds the boot settings. */
		printf("SWEET EMMC: card is not in the user area (partition config 0x%x)\\n", x[179]);
		return ENXIO;
	}
	su.emmc_cache = (x[33] & 1) != 0;
	return 0;
}

""")

# --- init: ADT, no UFS test unit ready, no topaz input drivers ------------------------
src = rep(src, """	su.regs = ml_io_map(reg[0], reg[1]);""", """	su.regs = ml_io_map(reg[0], reg[1]);
	/* lavender: optional second range = cmdq_mem, to check the CQE is off */
	su.cq = size >= 4 * sizeof(uintptr_t) ? ml_io_map(reg[2], reg[3]) : 0;""")
src = rep(src, """	if (!error) {
		static const uint8_t tur[6] = { 0x00 };
		/* The first command after the bootloader may report a unit attention. */
		for (int i = 0; i < 3 && (error = ufs_scsi(tur, sizeof(tur), 0, false)); i++) {
		}
	}
""", "")
src = rep(src, """	sweet_touch_init();
	sweet_keys_init();
	if (hw == 1) {
		sweet_usb_probe();""", """	/* lavender: the topaz touch/keys/USB drivers poke SM6225 MMIO */
	if (hw == 1) {
		sweet_touch_init();
		sweet_keys_init();
		sweet_usb_probe();""")
src = rep(src, '\tprintf("SWEET UFS: \\"%s\\" on LUN %u: first block', '\tprintf("SWEET EMMC: \\"%s\\" on LUN %u: first block')

open(path, 'w', newline='\n').write(src)
print('ok')
