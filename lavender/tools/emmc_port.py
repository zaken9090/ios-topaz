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
#define SD_HOST_CTRL    0x28            /* 8-bit: [4:3] DMA select, 11 = 64-bit ADMA2 */
#define SD_ADMA_LO      0x58
#define SD_ADMA_HI      0x5c
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
#define INT_ADMA_ERR            (1u << 25)
#define INT_ERROR               (1u << 15)
#define INT_ERRORS              0xffff0000u
#define INT_ENABLE_ALL          0x03ff003fu     /* normal 0-5, errors 16-25 */

/* transfer mode */
#define TM_DMA          (1u << 0)
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
/*
 * lavender: one ADMA2 descriptor per command, so its 12- or 16-byte size in
 * 64-bit mode does not matter; 32 KiB fits its 16-bit length field.
 */
#define BOUNCE_SIZE     (32u << 10)
#define CMD_TIMEOUT_NS  (5ull * NSEC_PER_SEC)
#define ADMA_TRAN_END   0x23            /* valid | end | act = transfer */

/* Layout of the contiguous DMA allocation (may be above 4 GiB: 64-bit ADMA2). */
#define DMA_DESC        0x0000
#define DMA_BOUNCE      0x1000
#define DMA_SIZE        (DMA_BOUNCE + BOUNCE_SIZE)

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

/* 16-bit register writes, as Linux sdhci does for block size/count, transfer mode, command */
static inline void
ufs_wr16(uint32_t off, uint16_t v)
{
	*(volatile uint16_t *)(su.regs + off) = v;
}

static void
dma_flush(uint32_t off, uint32_t len)
{
	flush_dcache64((addr64_t)(su.dma_pa + off), len, TRUE);
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

/* lavender bring-up: register reads for the photo; never the buffer port (it pops data) */
static uint32_t
sd_peek(uint32_t off)
{
	return off == SD_BUFFER ? 0xbbbbbbbbu : ufs_rd(off);
}

/* the first two commands: every standard SDHCI register, the msm vendor block once */
static void
sd_dump_once(const char *why)
{
	static unsigned dumped;
	if (dumped >= 2) {
		return;
	}
	dumped++;
	printf("SWEET EMMC: regs after %s:\\n", why);
	for (uint32_t off = 0; off < 0x80; off += 0x20) {
		printf("SWEET EMMC: %02x: %08x %08x %08x %08x %08x %08x %08x %08x\\n", off,
		    sd_peek(off), sd_peek(off + 4), sd_peek(off + 8), sd_peek(off + 12),
		    sd_peek(off + 16), sd_peek(off + 20), sd_peek(off + 24), sd_peek(off + 28));
	}
	if (dumped == 1) {
		for (uint32_t off = 0x100; off < 0x140; off += 0x20) {
			printf("SWEET EMMC: %03x: %08x %08x %08x %08x %08x %08x %08x %08x\\n", off,
			    ufs_rd(off), ufs_rd(off + 4), ufs_rd(off + 8), ufs_rd(off + 12),
			    ufs_rd(off + 16), ufs_rd(off + 20), ufs_rd(off + 24), ufs_rd(off + 28));
		}
	}
	printf("SWEET EMMC: int status as 16-bit: normal %04x error %04x\\n",
	    *(volatile uint16_t *)(su.regs + SD_INT_STATUS), *(volatile uint16_t *)(su.regs + SD_INT_STATUS + 2));
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
		/*
		 * lavender phone test 5: PIO reads through the buffer port never drained
		 * the FIFO on this msm host (Linux, LK and ABL only ever use ADMA2), so
		 * data moves by one 64-bit ADMA2 descriptor into the bounce buffer.
		 */
		uint32_t len = blocks * UFS_BLOCK;
		uint64_t buf_pa = su.dma_pa + DMA_BOUNCE, desc_pa = su.dma_pa + DMA_DESC;
		volatile uint32_t *d = (volatile uint32_t *)(su.dma + DMA_DESC);
		d[0] = (len << 16) | ADMA_TRAN_END;
		d[1] = (uint32_t)buf_pa;
		d[2] = (uint32_t)(buf_pa >> 32);
		d[3] = 0;
		dma_flush(DMA_DESC, 64);
		dma_flush(DMA_BOUNCE, len);
		__builtin_arm_dsb(0xf);
		uint8_t hc = *(volatile uint8_t *)(su.regs + SD_HOST_CTRL);
		*(volatile uint8_t *)(su.regs + SD_HOST_CTRL) = (uint8_t)((hc & ~0x18u) | 0x18u);
		ufs_wr(SD_ADMA_LO, (uint32_t)desc_pa);
		ufs_wr(SD_ADMA_HI, (uint32_t)(desc_pa >> 32));
		flags |= CF_DATA;
		mode = TM_DMA | TM_BLKCNT | (write ? 0 : TM_READ) | (blocks > 1 ? TM_MULTI | TM_ACMD12 : 0);
		ufs_wr16(SD_BLKSIZE, UFS_BLOCK);
		ufs_wr16(SD_BLKSIZE + 2, (uint16_t)blocks);
	}
	ufs_wr(SD_ARG, arg);
	/*
	 * Transfer mode, then the command (the command write starts it). lavender
	 * phone test 3: one 32-bit write here read back as command index 0, so
	 * these are two 16-bit writes like Linux sdhci.
	 */
	ufs_wr16(SD_CMD, (uint16_t)mode);
	ufs_wr16(SD_CMD + 2, (uint16_t)(idx << 8 | flags));

	/*
	 * lavender bring-up: for the first two commands record every change of
	 * (present state, interrupt status) during 20 ms, then print the timeline.
	 */
	static unsigned traces;
	if (traces < 2) {
		traces++;
		uint32_t tp[16], ts[16], tus[16], n = 0, lp = ~0u, ls = ~0u;
		uint64_t t0 = mach_absolute_time(), lim, ns;
		nanoseconds_to_absolutetime(20 * NSEC_PER_MSEC, &lim);
		while (mach_absolute_time() - t0 < lim && n < 16) {
			uint32_t pp = ufs_rd(SD_PRESENT), ss = ufs_rd(SD_INT_STATUS);
			if (pp != lp || ss != ls) {
				absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
				tp[n] = pp;
				ts[n] = ss;
				tus[n] = (uint32_t)(ns / 1000);
				n++;
				lp = pp;
				ls = ss;
			}
		}
		printf("SWEET EMMC: CMD%u trace (us present status):\\n", idx);
		for (uint32_t i = 0; i < n; i += 2) {
			if (i + 1 < n) {
				printf("SWEET EMMC:  %u %08x %08x | %u %08x %08x\\n",
				    tus[i], tp[i], ts[i], tus[i + 1], tp[i + 1], ts[i + 1]);
			} else {
				printf("SWEET EMMC:  %u %08x %08x\\n", tus[i], tp[i], ts[i]);
			}
		}
		printf("SWEET EMMC: CMD%u resp %08x\\n", idx, ufs_rd(SD_RESP0));
	}

	/*
	 * lavender (phone test 2): CMD inhibit stayed set and no status bit came
	 * although the data was already in the buffer. Treat buffer read enable as
	 * the response having arrived too.
	 */
	for (;;) {
		uint32_t pres = ufs_rd(SD_PRESENT);
		s = ufs_rd(SD_INT_STATUS);
		if (s & (INT_ERROR | INT_ERRORS)) {
			error = EIO;
			break;
		}
		if ((s & INT_CMD_DONE) || !(pres & PRES_CMD_INHIBIT) ||
		    (blocks && !write && (pres & PRES_BUF_RD))) {
			break;
		}
		if (mach_absolute_time() > deadline) {
			error = ETIMEDOUT;
			break;
		}
		delay(1);
	}
	if (!error) {
		ufs_wr(SD_INT_STATUS, INT_CMD_DONE);
		uint32_t r1 = ufs_rd(SD_RESP0);
		if (r1 & R1_ERRORS) {
			printf("SWEET EMMC: CMD%u arg 0x%x: card status 0x%x\\n", idx, arg, r1);
			error = EIO;
		}
	}
	if (!error && (blocks || (flags & 0x1f) == CF_R1B)) {
		error = sd_wait(INT_XFER_DONE, PRES_DAT_INHIBIT, 0, deadline, &s);
	}
	if (blocks) {
		dma_flush(DMA_BOUNCE, blocks * UFS_BLOCK);      /* drop stale lines before the CPU reads */
	}
	sd_dump_once(error ? "a failed command" : "the first command");
	if (!error && (ufs_rd(SD_PRESENT) & PRES_CMD_INHIBIT)) {
		/* the controller never closed the command: reset the CMD line only */
		static unsigned stuck;
		if (stuck++ < 3) {
			printf("SWEET EMMC: CMD%u done but CMD inhibit stuck (present 0x%x): CMD line reset\\n",
			    idx, ufs_rd(SD_PRESENT));
		}
		ufs_wr(SD_CLOCK, ufs_rd(SD_CLOCK) | RST_CMD);
		uint64_t d2 = sd_deadline();
		while ((ufs_rd(SD_CLOCK) & RST_CMD) && mach_absolute_time() < d2) {
			delay(1);
		}
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
