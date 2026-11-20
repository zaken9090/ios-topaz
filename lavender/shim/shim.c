/*
 * lavender probe shim: prints on the ABL framebuffer what Android hides
 * without root - CPU ID registers (16K granule?), RAM map and reserved
 * memory from the DTB ABL hands over, PSCI, GIC. Then resets after 120 s.
 *
 * MMU is off: every data access is Device memory, so the build uses
 * -mstrict-align and nothing here relies on caches or atomics.
 */
#include <stdint.h>
#include "font.h"
#include "mmu.h"

#define FB_BASE		0x9d400000UL
#define FB_W		1080
#define FB_H		2340
#define FB_STRIDE	(FB_W * 4)
#define SCALE		2
#define CH_W		(8 * SCALE)
#define CH_H		(16 * SCALE)
#define COLS		(FB_W / CH_W)
#define ROWS		(FB_H / CH_H)

#define GICD_BASE	0x17a00000UL
#define GICR_BASE	0x17b00000UL

#define PSCI_VERSION		0x84000000UL
#define PSCI_CPU_ON		0xc4000003UL
#define PSCI_FEATURES		0x8400000aUL
#define PSCI_SYSTEM_RESET	0x84000009UL

#define WHITE	0xffffffffu
#define GREEN	0xff40ff40u
#define YELLOW	0xffffff40u
#define RED	0xffff4040u
#define CYAN	0xff40ffffu
#define BG	0xff000020u

#define RD(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " #r : "=r"(_v)); _v; })
#define WR(r, v) __asm__ volatile("msr " #r ", %0" : : "r"((uint64_t)(v)))

#define GOLD_MPIDR	0x100	/* first A73-class core: has the 16K granule */

extern char secondary_entry[];

/* ---------------- freestanding helpers ---------------- */
void *memset(void *d, int c, unsigned long n)
{
	volatile unsigned char *p = d;
	while (n--)
		*p++ = (unsigned char)c;
	return d;
}

void *memcpy(void *d, const void *s, unsigned long n)
{
	volatile unsigned char *p = d;
	const unsigned char *q = s;
	while (n--)
		*p++ = *q++;
	return d;
}

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }

static uint64_t ticks(void) { return RD(cntpct_el0); }

static void delay_ms(uint64_t ms)
{
	uint64_t f = RD(cntfrq_el0), t = ticks() + f / 1000 * ms;
	while (ticks() < t)
		;
}

static uint64_t smc(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = a0;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : :
			 "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12",
			 "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

/* ---------------- console ---------------- */
static int cx, cy;
static uint32_t fg = WHITE;

static void con_clear(void)
{
	volatile uint32_t *p = (volatile uint32_t *)FB_BASE;
	for (int i = 0; i < FB_W * FB_H; i++)
		p[i] = BG;
	cx = cy = 0;
}

static void con_glyph(int col, int row, char c)
{
	if (c < 0x20 || c > 0x7e)
		c = '?';
	const unsigned char *g = font8x16[c - 0x20];
	volatile uint32_t *fb = (volatile uint32_t *)FB_BASE;
	for (int gy = 0; gy < 16; gy++)
		for (int sy = 0; sy < SCALE; sy++) {
			volatile uint32_t *line = fb + (row * CH_H + gy * SCALE + sy) * (FB_STRIDE / 4) + col * CH_W;
			for (int gx = 0; gx < 8; gx++) {
				uint32_t px = (g[gy] & (0x80 >> gx)) ? fg : BG;
				for (int sx = 0; sx < SCALE; sx++)
					*line++ = px;
			}
		}
}

static void putc_(char c)
{
	if (c == '\n' || cx >= COLS) {
		cx = 0;
		if (++cy >= ROWS)
			cy = 0;
		if (c == '\n')
			return;
	}
	con_glyph(cx++, cy, c);
}

static void puts_(const char *s)
{
	while (*s)
		putc_(*s++);
}

static void puthex(uint64_t v, int digits)
{
	for (int i = digits - 1; i >= 0; i--)
		putc_("0123456789abcdef"[(v >> (i * 4)) & 0xf]);
}

static void putdec(int64_t v)
{
	char b[24];
	int n = 0;
	if (v < 0) {
		putc_('-');
		v = -v;
	}
	do {
		b[n++] = '0' + v % 10;
		v /= 10;
	} while (v);
	while (n)
		putc_(b[--n]);
}

static void kv(const char *k, uint64_t v, int digits)
{
	puts_(k);
	puts_("0x");
	puthex(v, digits);
	putc_('\n');
}

/* ---------------- FDT ---------------- */
static uint32_t be32(const void *p)
{
	const unsigned char *b = p;
	return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

static uint64_t cells(const unsigned char *p, int n)
{
	uint64_t v = 0;
	for (int i = 0; i < n; i++)
		v = v << 32 | be32(p + 4 * i);
	return v;
}

static int streq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

static int prefix(const char *s, const char *p)
{
	while (*p)
		if (*s++ != *p++)
			return 0;
	return 1;
}

#define MAXCPU 8
static uint64_t cpu_mpidr[MAXCPU];
static int ncpu;
static int print_reserved = 1;

/* Walk the tree and print /memory, /reserved-memory/<*>, collect /cpus/cpu@<*>. */
static void fdt_walk(const unsigned char *fdt)
{
	uint32_t off_struct = be32(fdt + 8), off_str = be32(fdt + 12);
	const unsigned char *p = fdt + off_struct;
	const char *path[8] = { 0 };
	int depth = 0, root_ac = 2, root_sc = 2, cpus_ac = 1;
	int nres = 0;

	for (;;) {
		uint32_t tok = be32(p);
		p += 4;
		if (tok == 1) {			/* BEGIN_NODE */
			const char *name = (const char *)p;
			while (*p)
				p++;
			p++;
			p = (const unsigned char *)(((uintptr_t)p + 3) & ~3UL);
			if (depth < 8)
				path[depth] = name;
			depth++;
		} else if (tok == 2) {		/* END_NODE */
			depth--;
		} else if (tok == 3) {		/* PROP */
			uint32_t len = be32(p), no = be32(p + 4);
			const unsigned char *val = p + 8;
			const char *pn = (const char *)fdt + off_str + no;
			p = (const unsigned char *)(((uintptr_t)(val + len) + 3) & ~3UL);

			if (depth == 1 && streq(pn, "#address-cells"))
				root_ac = be32(val);
			else if (depth == 1 && streq(pn, "#size-cells"))
				root_sc = be32(val);
			else if (depth == 2 && streq(path[1], "cpus") && streq(pn, "#address-cells"))
				cpus_ac = be32(val);
			else if (depth == 2 && prefix(path[1], "memory") && streq(pn, "reg")) {
				int step = 4 * (root_ac + root_sc);
				fg = GREEN;
				for (uint32_t i = 0; i + step <= len; i += step) {
					uint64_t b = cells(val + i, root_ac), s = cells(val + i + 4 * root_ac, root_sc);
					puts_("RAM  ");
					puthex(b, 9);
					puts_(" +");
					puthex(s, 9);
					puts_(" (");
					putdec((int64_t)(s >> 20));
					puts_(" MB)\n");
				}
				fg = WHITE;
			} else if (depth == 3 && print_reserved && streq(path[1], "reserved-memory") &&
				   streq(pn, "reg")) {
				const char *n = path[2];
				char nm[22];
				int k = 0;
				while (n[k] && n[k] != '@' && k < 21) {
					nm[k] = n[k];
					k++;
				}
				nm[k] = 0;
				uint64_t b = cells(val, root_ac), s = cells(val + 4 * root_ac, root_sc);
				puthex(b, 9);
				putc_('+');
				puthex(s, 8);
				putc_(' ');
				puts_(nm);
				putc_('\n');
				nres++;
			} else if (depth == 3 && streq(path[1], "cpus") && prefix(path[2], "cpu@") &&
				   streq(pn, "reg") && ncpu < MAXCPU) {
				cpu_mpidr[ncpu++] = cells(val + 4 * (cpus_ac - 1), 1);
			}
		} else if (tok == 4) {		/* NOP */
		} else {			/* END or garbage */
			break;
		}
		if (depth < 0)
			break;
	}
	if (print_reserved) {
		puts_("reserved-memory with reg: ");
		putdec(nres);
		putc_('\n');
	}
}

/* ---------------- secondary CPUs ---------------- */
struct cpuinfo {
	uint64_t done, mpidr, midr, mmfr0, el;
};
static struct cpuinfo sec[MAXCPU];

static void probe_secondary(uint64_t slot)
{
	struct cpuinfo *c = &sec[slot & 7];
	c->mpidr = RD(mpidr_el1);
	c->midr = RD(midr_el1);
	c->mmfr0 = RD(id_aa64mmfr0_el1);
	c->el = RD(currentel) >> 2;
	__asm__ volatile("dsb sy" ::: "memory");
	c->done = 1;
	__asm__ volatile("dsb sy" ::: "memory");
}

/* printed directly: a switch returning strings becomes a table of absolute pointers */
static void put_tgran16(uint64_t mmfr0)
{
	uint32_t t = (mmfr0 >> 20) & 0xf;
	if (t == 0)
		puts_("16K:NO ");
	else if (t == 1)
		puts_("16K:YES");
	else if (t == 2)
		puts_("16K:YES52");
	else
		puts_("16K:?  ");
}

static void print_cpu(const char *tag, uint64_t mpidr, uint64_t midr, uint64_t mmfr0)
{
	uint32_t part = (midr >> 4) & 0xfff;
	puts_(tag);
	puts_(" mpidr ");
	puthex(mpidr & 0xffffff, 6);
	puts_(" midr ");
	puthex(midr, 8);
	puts_(part == 0x800 ? " GOLD " : part == 0x801 ? " SILV " : " ???? ");
	fg = ((mmfr0 >> 20) & 0xf) ? GREEN : RED;
	put_tgran16(mmfr0);
	fg = WHITE;
	puts_(((mmfr0 >> 28) & 0xf) == 0 ? " 4K" : " -");
	puts_(((mmfr0 >> 24) & 0xf) == 0 ? " 64K" : " -");
	putc_('\n');
}

/* ---------------- gold hand-off + 16K MMU ---------------- */
static volatile uint64_t gold_go, silver_ticks;

/*
 * Core speed probe (from ios-topaz): 19.2 MHz ticks for a fixed loop with the
 * I-cache on. Lower = faster. Gold after PSCI CPU_ON may run at a low clock.
 */
static uint64_t speed_ticks(void)
{
	uint64_t sctlr = RD(sctlr_el1);
	WR(sctlr_el1, sctlr | (1UL << 12));
	__asm__ volatile("isb");
	uint64_t t = ticks(), x = 0;
	for (uint64_t i = 0; i < 20000000; i++)
		__asm__ volatile("add %0, %0, #1" : "+r"(x));
	t = ticks() - t;
	WR(sctlr_el1, sctlr);
	__asm__ volatile("isb");
	return t;
}

static void par(const char *what, uintptr_t va)
{
	uint64_t p = mmu_probe(va);
	puts_(what);
	puthex(va, 9);
	puts_(" -> ");
	if (p & 1) {
		fg = RED;
		puts_("FAULT ");
		puthex(p, 4);
	} else {
		fg = GREEN;
		puthex(p & 0x0000fffffffff000UL, 9);
		puts_(" attr ");
		puthex(p >> 56, 2);
	}
	fg = WHITE;
	putc_('\n');
}

static uint64_t test_buf[512];

/*
 * READ-ONLY look at the SDM660 CPU clock hardware (addresses from the
 * lavender msm-4.4 sdm660.dtsi node qcom,clk-cpu-660@179c0000 and
 * drivers/clk/qcom/clk-cpu-osm.c). Nothing is written.
 *   OSM: pwrcl (silver) 0x179c0000, perfcl (gold) +0x2000
 *   PLL: pwrcl 0x17916000, perfcl 0x17816000 (L_VAL * 19.2 MHz)
 */
#define OSM_PWRCL	0x179c0000UL
#define OSM_PERFCL	(OSM_PWRCL + 0x2000)
#define PLL_PWRCL	0x17916000UL
#define PLL_PERFCL	0x17816000UL

static void reg(const char *name, uintptr_t a)
{
	puts_(name);
	putc_(' ');
	puthex(a, 8);
	puts_(" = ");
	puthex(rd32(a), 8);
	putc_('\n');
}

static void pll_line(const char *name, uintptr_t b)
{
	uint32_t mode = rd32(b + 0x0), l = rd32(b + 0x4) & 0xff, st = rd32(b + 0x2c);
	puts_(name);
	puts_(" mode ");
	puthex(mode, 8);
	puts_(" L ");
	putdec(l);
	puts_(" = ");
	putdec((int64_t)l * 192 / 10);
	puts_(" MHz st ");
	puthex(st, 8);
	putc_('\n');
}

/*
 * Gold glitch-free mux (APCS perfcl RCGR, same values clk-cpu-osm.c uses):
 * cfg SRC bits[10:8]: 4 = GPLL0 aux (~300 MHz, ABL default), 3 = PLL main.
 * Writing cmd = RCG_UPDATE (0x3) latches cfg; bit0 clears when done.
 * Only the source changes: no PLL, voltage or OSM writes.
 */
#define RCGR_GOLD_CMD	0x17811050UL
#define RCGR_GOLD_CFG	0x17811054UL

static inline void wr32(uintptr_t a, uint32_t v)
{
	*(volatile uint32_t *)a = v;
	__asm__ volatile("dsb sy" ::: "memory");
}

static void gold_mux_to_pll(void)
{
	fg = CYAN;
	puts_("\n-- gold clock: mux GPLL0 -> own PLL --\n");
	fg = WHITE;
	uint32_t st = rd32(PLL_PERFCL + 0x2c), l = rd32(PLL_PERFCL + 0x4) & 0xff;
	if (!(st & (1u << 16)) || (rd32(PLL_PERFCL) & 7) != 7) {
		fg = RED;
		puts_("gold PLL not locked/enabled - skip\n");
		fg = WHITE;
		return;
	}
	uint32_t cfg = rd32(RCGR_GOLD_CFG);
	kv("cfg before ", cfg, 8);
	uint64_t before = speed_ticks();
	wr32(RCGR_GOLD_CFG, (cfg & ~0x700u) | 0x300u);
	wr32(RCGR_GOLD_CMD, 0x3);
	int n = 0;
	while ((rd32(RCGR_GOLD_CMD) & 1) && n < 100000)
		n++;
	__asm__ volatile("isb");
	kv("cfg after  ", rd32(RCGR_GOLD_CFG), 8);
	kv("cmd after  ", rd32(RCGR_GOLD_CMD), 8);
	puts_("update polls ");
	putdec(n);
	putc_('\n');
	uint64_t after = speed_ticks();
	puts_("speed ticks before ");
	putdec((int64_t)before);
	puts_(" after ");
	putdec((int64_t)after);
	putc_('\n');
	/* silver ran the same loop at 1382 MHz (PLL L 72) in silver_ticks */
	puts_("gold PLL ");
	putdec((int64_t)l * 192 / 10);
	puts_(" MHz, speedup x");
	putdec((int64_t)(before / (after ? after : 1)));
	putc_('.');
	putdec((int64_t)((before * 10 / (after ? after : 1)) % 10));
	putc_('\n');
}

/*
 * Real Gold clock: PMU cycle counter against the 19.2 MHz timer over 20 ms.
 * Counts core cycles, so clock dividers and pulse swallowing show up too.
 */
static uint64_t gold_mhz(void)
{
	WR(pmcr_el0, RD(pmcr_el0) | 1);          /* E */
	WR(pmcntenset_el0, 1UL << 31);           /* cycle counter */
	__asm__ volatile("isb");
	uint64_t f = RD(cntfrq_el0), t0 = ticks(), c0 = RD(pmccntr_el0);
	while (ticks() - t0 < f / 50)
		;
	uint64_t c1 = RD(pmccntr_el0), t1 = ticks();
	return (c1 - c0) * (f / 1000000) / (t1 - t0 ? t1 - t0 : 1);
}

#define APCS_ITM_PRESENT	0x179d143cUL

static void gold_line(const char *what)
{
	puts_(what);
	puts_(": ");
	putdec((int64_t)gold_mhz());
	puts_(" MHz, loop ");
	putdec((int64_t)speed_ticks());
	puts_(" | cfg ");
	puthex(rd32(RCGR_GOLD_CFG), 4);
	puts_(" itm ");
	puthex(rd32(APCS_ITM_PRESENT), 8);
	putc_('\n');
}

/*
 * Before XNU: measure Gold, then try (1) the APCS ITM_PRESENT perfcl bit off
 * (clk-cpu-osm.c clears it for a cluster at OSM hand-off), (2) the Gold mux
 * on its own PLL (614 MHz, set by XBL; v4). No PLL L or voltage changes.
 * Keeps whatever ends up faster. Results stay on screen for 12 s.
 */
static void clk_regs(const char *name, uintptr_t pll, uintptr_t rcg)
{
	puts_(name);
	puts_(" pll ");
	puthex(rd32(pll), 1);
	puts_(" L ");
	putdec(rd32(pll + 4) & 0xff);
	puts_(" uctl ");
	puthex(rd32(pll + 0xc), 8);
	puts_(" cfg ");
	puthex(rd32(pll + 0x10), 8);
	puts_(" dd ");
	puthex(rd32(pll + 0x208), 8);
	puts_(" rcg ");
	puthex(rd32(rcg), 2);
	putc_('/');
	puthex(rd32(rcg + 4), 4);
	putc_('\n');
}

static void mux_set(uint32_t cfg)
{
	wr32(RCGR_GOLD_CFG, cfg);
	wr32(RCGR_GOLD_CMD, 0x3);
	for (int n = 0; (rd32(RCGR_GOLD_CMD) & 1) && n < 100000; n++)
		;
	__asm__ volatile("isb");
}

/*
 * Probe v7 (no XNU): compare the Silver cluster (runs at its PLL rate) with
 * Gold, dump APCS common, then try Silver's PLL user_ctl (post divider) on
 * the Gold PLL while Gold runs from GPLL0, and the mux to PLL main / early.
 */
static void silver_mux_set(uint32_t cfg)
{
	wr32(0x17911054UL, cfg);
	wr32(0x17911050UL, 0x3);
	for (int n = 0; (rd32(0x17911050UL) & 1) && n < 100000; n++)
		;
	__asm__ volatile("isb");
}

/* v8: does the Silver RCG choose Silver's clock? (Silver runs at its PLL rate with cfg 0x401) */
static void silver_mux_test(void)
{
	uint32_t cfg = rd32(0x17911054UL);
	puts_("\nsilver mux: ");
	puthex(cfg, 4);
	puts_(" ");
	putdec((int64_t)gold_mhz());
	silver_mux_set((cfg & ~0x700u) | 0x300u);
	puts_(" | 301 ");
	putdec((int64_t)gold_mhz());
	silver_mux_set((cfg & ~0x700u) | 0x500u);
	puts_(" | 501 ");
	putdec((int64_t)gold_mhz());
	silver_mux_set(cfg);
	puts_(" | back ");
	putdec((int64_t)gold_mhz());
	puts_(" MHz\n");
}

static void osm_pair(const char *name, uint32_t off)
{
	puts_(name);
	putc_(' ');
	puthex(rd32(OSM_PWRCL + off), 8);
	putc_(' ');
	puthex(rd32(OSM_PERFCL + off), 8);
	putc_(' ');
}

static void gold_pll_relock(uint32_t l)
{
	wr32(PLL_PERFCL + 0x0, 0x0);
	wr32(PLL_PERFCL + 0x4, l);
	wr32(PLL_PERFCL + 0x0, 0x2);
	for (volatile int i = 0; i < 20000; i++)
		;
	wr32(PLL_PERFCL + 0x0, 0x6);
	for (volatile int i = 0; i < 100000; i++)
		;
	wr32(PLL_PERFCL + 0x0, 0x7);
	for (int n = 0; !(rd32(PLL_PERFCL + 0x2c) & (1u << 16)) && n < 100000; n++)
		;
}

static void hexrow(const char *name, uintptr_t a, int n)
{
	puts_(name);
	for (int i = 0; i < n; i++) {
		putc_(' ');
		puthex(rd32(a + 4 * (uintptr_t)i), 8);
	}
	putc_('\n');
}

/* print offsets where the Gold (0x178xxxxx) and Silver (0x179xxxxx) pages differ */
static int diff_pages(uintptr_t gold, uintptr_t silver, uint32_t size, int budget)
{
	for (uint32_t o = 0; o < size && budget > 0; o += 4) {
		uint32_t g = rd32(gold + o), sv = rd32(silver + o);
		if (g != sv) {
			puthex(gold + o, 8);
			puts_(" g ");
			puthex(g, 8);
			puts_(" s ");
			puthex(sv, 8);
			putc_('\n');
			budget--;
		}
	}
	return budget;
}

static void try_reg(const char *what, uintptr_t a, uint32_t v)
{
	puts_(what);
	puts_(" <- ");
	puthex(v, 8);
	puts_(" ...\n");
	delay_ms(3000);
	wr32(a, v);
	__asm__ volatile("isb");
	gold_line("   ");
	delay_ms(3000);
}

/*
 * Probe v12 (no XNU): v11 diff of the Gold vs Silver APCS clock pages.
 * Copy Silver's values into Gold one at a time (0x...058 also 1 and 0),
 * measuring after each. A hang shows the step that did it.
 */
static void gold_clock_tune(void)
{
	fg = CYAN;
	puts_("\n-- gold clock probe v12c: silver values into gold --\n");
	fg = WHITE;
	gold_line("start");
	try_reg("00c=6", 0x1781100cUL, 6);
	try_reg("010=6", 0x17811010UL, 6);
	try_reg("018  ", 0x17811018UL, rd32(0x17811018UL) & ~(1u << 16));
	try_reg("06c  ", 0x1781106cUL, rd32(0x1781106cUL) & ~(1u << 24));
	try_reg("028  ", 0x17811028UL, rd32(0x17811028UL) | (1u << 29));
	try_reg("290  ", 0x17811290UL, 0xc005fe00u);
	fg = GREEN;
	puts_("PROBE ONLY: take a photo, then hold POWER + VOL DOWN\n");
	fg = WHITE;
	for (;;)
		__asm__ volatile("wfe");
}

/*
 * MDP probe (no XNU): which SDM660 MDP5 source pipe scans the ABL splash
 * (0x9d400000), which CTL drives it, and does pointing that pipe at another
 * buffer + CTL flush switch the panel (needed for double-buffered frames)?
 * Layout from the lavender DTB: mdp_phys 0xc900000, VIG 0x5000/0x7000,
 * DMA 0x25000/0x27000/0x29000, CTL 0x2000 + n*0x200, FLUSH at +0x18.
 */
/* 1: run gpu_probe() on gold with the MMU on (the hypervisor emulates SMMU
 * stream-mapping writes only for a guest with its MMU on?), no XNU */
#define GPU_PROBE_ON_GOLD	0

#define MDP_BASE	0x0c900000UL
#define MDP_TEST_BUF	0xc0000000UL

static void mdp_probe(void)
{
	static const uint32_t pipes[] = {0x5000, 0x7000, 0x25000, 0x27000, 0x29000};
	static const char names[] = "vig0vig1dma0dma1dma2";  /* no pointer table: no ABS64 relocs */
	static const uint32_t flushbit[] = {1u << 0, 1u << 1, 1u << 11, 1u << 12, 1u << 13};
	fg = CYAN;
	puts_("\n-- MDP probe --\n");
	fg = WHITE;
	reg("mdp hw ver", MDP_BASE + 0x0);
	reg("mdp 1000  ", MDP_BASE + 0x1000);
	int found = -1;
	for (int i = 0; i < 5; i++) {
		uintptr_t b = MDP_BASE + pipes[i];
		for (int k = 0; k < 4; k++)
			putc_(names[i * 4 + k]);
		puts_(" size ");
		puthex(rd32(b + 0x0), 8);
		puts_(" addr ");
		puthex(rd32(b + 0x14), 8);
		puts_(" stride ");
		puthex(rd32(b + 0x24), 8);
		puts_(" fmt ");
		puthex(rd32(b + 0x30), 8);
		putc_('\n');
		if (rd32(b + 0x14) == FB_BASE)
			found = i;
	}
	int ctl = -1;
	for (int c = 0; c < 5; c++) {
		uintptr_t b = MDP_BASE + 0x2000 + (uintptr_t)c * 0x200;
		puts_("ctl");
		putdec(c);
		puts_(" lm0 ");
		puthex(rd32(b + 0x0), 8);
		puts_(" lm1 ");
		puthex(rd32(b + 0x4), 8);
		puts_(" top ");
		puthex(rd32(b + 0x14), 8);
		puts_(" flush ");
		puthex(rd32(b + 0x18), 8);
		putc_('\n');
		if (ctl < 0 && rd32(b + 0x14) != 0)
			ctl = c;
	}
	puts_("pipe ");
	putdec(found);
	puts_(" ctl ");
	putdec(ctl);
	putc_('\n');
	if (found < 0 || ctl < 0) {
		puts_("no splash pipe/ctl - stop\n");
		goto park;
	}
	{
		uintptr_t pb = MDP_BASE + pipes[found];
		uintptr_t cb = MDP_BASE + 0x2000 + (uintptr_t)ctl * 0x200;
		uint32_t orig = rd32(pb + 0x14);
		/* test frame: green/blue bands in a buffer of our own */
		volatile uint32_t *t = (volatile uint32_t *)MDP_TEST_BUF;
		for (uint32_t y = 0; y < FB_H; y++)
			for (uint32_t x = 0; x < FB_W; x++)
				t[y * FB_W + x] = ((y / 100) & 1) ? 0xff00c000u : 0xff0040ffu;
		__asm__ volatile("dsb sy" ::: "memory");
		puts_("switching pipe to test buffer for 4 s (expect green/blue bands)\n");
		delay_ms(1500);
		wr32(pb + 0x14, (uint32_t)MDP_TEST_BUF);
		wr32(cb + 0x18, flushbit[found] | (1u << 17));
		delay_ms(4000);
		uint32_t fl = rd32(cb + 0x18);
		wr32(pb + 0x14, orig);
		wr32(cb + 0x18, flushbit[found] | (1u << 17));
		delay_ms(100);
		puts_("back on splash; flush after test ");
		puthex(fl, 8);
		puts_(" now ");
		puthex(rd32(cb + 0x18), 8);
		puts_(" addr ");
		puthex(rd32(pb + 0x14), 8);
		putc_('\n');
	}
park:
	fg = GREEN;
	puts_("MDP PROBE ONLY: did the bands show? photo, then POWER + VOL DOWN\n");
	fg = WHITE;
	for (;;)
		__asm__ volatile("wfe");
}

/*
 * GPU power probe (no XNU): SDM660 Adreno 512. GCC GPU branches, GPUCC GDSCs
 * (CX 0x5066004, GX 0x5066094 with clamp 0x5065130 and BCR 0x5066090),
 * gfx3d RCG 0x5066070 from GPLL0 (src 5), branches gfx3d 0x5066098 and
 * rbbmtimer 0x50660d0, then read RBBM_STATUS (reg 0x4f5) at 0x5000000.
 * Values from mainline gpucc-sdm660.c / gcc-sdm660.c; a hang shows the step.
 */
#define GCCB		0x00100000UL
#define GPUCC		0x05065000UL
#define GPUREG(r)	(0x05000000UL + 4UL * (r))

static void step(const char *what)
{
	puts_(what);
	puts_(" ...\n");
	delay_ms(300);
}

static int poll_set(uintptr_t a, uint32_t mask, int want_set)
{
	for (int i = 0; i < 100000; i++) {
		uint32_t v = rd32(a);
		if (want_set ? (v & mask) : !(v & mask))
			return 1;
	}
	return 0;
}

#if __has_include("out/gpufw.h")
#include "out/gpufw.h"
#define HAVE_GPUFW 1
/*
 * GPU CP probe, after gpu_probe() powered the Adreno 512 (MMU off, physical
 * addresses; topaz TopazAdreno.cpp flow with the a5xx registers of Linux
 * a5xx_gpu.c): zap shader through TZ PIL (PAS 13), GPU SMMU context bank 0
 * pass-through, minimal a5xx init, PM4/PFP microcode, ringbuffer, ME_INIT,
 * SET_SECURE_MODE 0 and a CP_MEM_WRITE we read back.
 */
#define ZAP_PA		0xbfe00000UL
#define META_PA		0xbfd00000UL
#define PM4_PA		0xbfc00000UL
#define PFP_PA		0xbfc10000UL
#define RING_PA		0xbfb00000UL
#define SCRATCH_PA	0xbfb10000UL
#define RB_DW		(0x8000 / 4)
#define GSMMU		0x05040000UL
#define gw(r, v)	wr32(GPUREG(r), (v))
#define gr(r)		rd32(GPUREG(r))

static uint64_t scm6(uint64_t fn, uint64_t info, uint64_t a, uint64_t b, uint64_t c, uint64_t *res1)
{
	register uint64_t x0 __asm__("x0") = fn, x1 __asm__("x1") = info, x2 __asm__("x2") = a,
		x3 __asm__("x3") = b, x4 __asm__("x4") = c, x5 __asm__("x5") = 0, x6 __asm__("x6") = 0;
	__asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6) : :
			 "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	for (int n = 0; x0 == 1 && n < 100000; n++) {	/* interrupted: resume with the session in x6 */
		uint64_t session = x6;
		x0 = 1; x1 = info; x2 = a; x3 = b; x4 = c; x5 = 0; x6 = session;
		__asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6) : :
				 "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	}
	if (res1)
		*res1 = x1;
	return x0;
}

#define SCM_PIL(cmd)	(0x42000000u | (0x02u << 8) | (cmd))

static void st_line(const char *what, uint64_t st, uint64_t res)
{
	puts_(what);
	puts_(" st ");
	puthex(st, 8);
	puts_(" res ");
	puthex(res, 8);
	putc_('\n');
}

static uint32_t par4(uint32_t v)
{
	return (0x9669 >> (0xF & (v ^ (v >> 4) ^ (v >> 8) ^ (v >> 12) ^ (v >> 16) ^ (v >> 20) ^ (v >> 24) ^ (v >> 28)))) & 1;
}

static uint32_t pkt7(uint32_t op, uint32_t cnt)
{
	return 0x70000000u | cnt | (par4(cnt) << 15) | ((op & 0x7F) << 16) | (par4(op) << 23);
}

static uint32_t rb_w;

static void emit(uint32_t v)
{
	((volatile uint32_t *)RING_PA)[rb_w % RB_DW] = v;
	rb_w++;
}

static int kick_wait(const char *what)
{
	__asm__ volatile("dsb sy" ::: "memory");
	gw(0x807, rb_w % RB_DW);
	int ok = 0;
	for (int i = 0; i < 2000000; i++) {
		if (gr(0x806) == rb_w % RB_DW) {
			ok = 1;
			break;
		}
	}
	puts_(what);
	puts_(ok ? ": done" : ": STUCK");
	puts_(" rptr ");
	puthex(gr(0x806), 4);
	puts_(" wptr ");
	puthex(rb_w % RB_DW, 4);
	puts_(" status ");
	puthex(gr(0x4f5), 8);
	puts_(" int ");
	puthex(gr(0x4e1), 8);
	puts_(" fault ");
	puthex(gr(0xb1a), 8);
	putc_('\n');
	return ok;
}

static void copy_bytes(uintptr_t dst, const unsigned char *src, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++)
		*(volatile unsigned char *)(dst + i) = src[i];
}

static void gpu_cp_probe(void)
{
	delay_ms(4000);			/* power probe output stays readable for a moment */
	con_clear();
	fg = CYAN;
	puts_("-- GPU CP probe (GPU powered, RBBM_STATUS ok) --\n");
	fg = WHITE;
	uint64_t st, res = 0;

	step("GPU SMMU before zap: attach-impl-defs, CB0 pass-through, SMR0/S2CR0 for SID 0");
	reg("smmu sCR0    ", GSMMU + 0x0);
	reg("smmu S2CR0   ", GSMMU + 0xc00);
	{
		/* lavender DT attach-impl-defs (arm-smmu writes them at attach) */
		static const uint32_t impl[][2] = {
			{0x6000, 0x2378}, {0x6060, 0x1055}, {0x678c, 0x8}, {0x6794, 0x28}, {0x6800, 0x6},
			{0x6900, 0x3ff}, {0x6924, 0x204}, {0x6928, 0x11000}, {0x6930, 0x800}, {0x6960, 0xffffffff},
			{0x6b64, 0x1a5551}, {0x6b68, 0x9a82a382},
		};
		step("w impl-defs");
		for (unsigned i = 0; i < sizeof(impl) / sizeof(impl[0]); i++)
			wr32(GSMMU + impl[i][0], impl[i][1]);
	}
	step("w CB0 (SCTLR 0, FSR, CBA2R0, CBAR0, SCTLR 0xe0)");
	wr32(GSMMU + 0x8000, 0);
	wr32(GSMMU + 0x8058, 0xffffffff);
	wr32(GSMMU + 0x1800, 1);
	wr32(GSMMU + 0x1000, 0x0001f000);
	wr32(GSMMU + 0x8000, 0xe0);
	/* the SMMU's own clocks (lavender DT): gcc_gpu_cfg_ahb, gcc_bimc_gfx, gcc_gpu_bimc_gfx */
	reg("gcc gpu ahb  ", GCCB + 0x71004);
	reg("gcc bimc gfx ", GCCB + 0x7106c);
	reg("gcc gpu bimc ", GCCB + 0x71010);
	reg("smmu sGFSR   ", GSMMU + 0x48);
	reg("smmu sACR    ", GSMMU + 0x10);
	reg("smmu sCR2    ", GSMMU + 0x8);
	/* mainline arm-smmu-qcom.c qcom_smmu_cfg_probe() for qcom,sdm630-smmu-v2:
	 * the hypervisor traps and emulates S2CR; Linux first writes the LAST
	 * group's S2CR as BYPASS and reads it back (bypass quirk detection). */
	{
		uint32_t last = (rd32(GSMMU + 0x20) & 0xff) - 1;
		puts_("last S2CR index ");
		putdec(last);
		putc_('\n');
		step("w S2CR[last] = BYPASS (mainline probe)");
		wr32(GSMMU + 0xc00 + 4 * last, (1u << 16) | 0xffu);
		reg("S2CR[last]   ", GSMMU + 0xc00 + 4 * last);
		step("w SMR[last] = valid SID 0 mask 0");
		wr32(GSMMU + 0x800 + 4 * last, (1u << 31));
		reg("SMR[last]    ", GSMMU + 0x800 + 4 * last);
		/* bypass quirk (BYPASS reads back as FAULT): like mainline, translate
		 * through a context bank that does not translate (CB0, SCTLR.M=0) */
		/* downstream arm_smmu_write_s2cr(): TRANS | CBNDX | SHCFG_NSH (3 << 8); 0 alone hung */
		step("w S2CR[last] = TRANS, CB0, SHCFG NSH (0x300)");
		wr32(GSMMU + 0xc00 + 4 * last, 0x300u);
		reg("S2CR[last]   ", GSMMU + 0xc00 + 4 * last);
	}
	reg("smmu S2CR0   ", GSMMU + 0xc00);
	reg("smmu SMR0    ", GSMMU + 0x800);
	reg("cb0 SCTLR    ", GSMMU + 0x8000);

	step("zap: PIL init_image / mem_setup / auth_reset (PAS 13)");
	for (uint32_t i = 0; i < 0x2000; i += 4)
		*(volatile uint32_t *)(ZAP_PA + i) = 0;
	copy_bytes(META_PA, fw_zap_mdt, sizeof(fw_zap_mdt));
	__asm__ volatile("dsb sy" ::: "memory");
	st = scm6(SCM_PIL(0x06), 1, 13, 0, 0, &res);
	st_line("shutdown  ", st, res);
	st = scm6(SCM_PIL(0x01), 2 | (2 << 6), 13, META_PA, 0, &res);
	st_line("init_image", st, res);
	st = scm6(SCM_PIL(0x02), 3, 13, ZAP_PA, 0x2000, &res);
	st_line("mem_setup ", st, res);
	copy_bytes(ZAP_PA, fw_zap_b02, sizeof(fw_zap_b02));	/* segment paddr 0x5000 = region start */
	__asm__ volatile("dsb sy" ::: "memory");
	st = scm6(SCM_PIL(0x05), 1, 13, 0, 0, &res);
	st_line("auth_reset", st, res);

	step("a5xx init (Linux a5xx_hw_init, no hwcg/protect)");
	gw(0x3049, 3);
	gw(0x302a, 9);
	gw(0x46f, 0xffffffff);
	gw(0x93, 1);
	gw(0x464, 1);
	gw(0xe96, 2);
	gw(0xe89, 0xfffff000); gw(0xe8a, 0x0001ffff);
	gw(0xe87, 0xfffff000); gw(0xe88, 0x0001ffff);
	gw(0xe8b, 0x00100000); gw(0xe8c, 0);
	gw(0xe8d, 0x00100000 + 0x44000 - 1); gw(0xe8e, 0);
	gw(0x825, 0x40);
	gw(0x826, 0x400);
	gw(0x820, 0x80000060);
	gw(0x81f, 0x40201b16);
	gw(0xd00, (0x200u << 11) | (0x200u << 22));
	gw(0xcc4, gr(0xcc4) | (1u << 9));
	gw(0xe81, 1u << 29);
	gw(0x833, 0x02000000);
	gw(0x94, 0xa6ffffff);
	gw(0x95, 0x3f);
	gw(0xf02, 1u << 7);
	gw(0xcc6, 1u << 1);
	gw(0xe8f, 1);
	gw(0xe60, gr(0xe60) | (1u << 10));
	gw(0x8a0, 0);
	gw(0xf803, 0);
	gw(0xf800, 0); gw(0xf801, 0);
	gw(0xf802, 0);
	gw(0x860, 1); gw(0xbc1, 1); gw(0xc81, 1); gw(0xcc5, 1); gw(0xd01, 1); gw(0xe05, 1);
	gw(0xe41, 1); gw(0xe61, 1); gw(0xe80, 1); gw(0xec1, 1); gw(0xf01, 1); gw(0xf810, 1);
	gw(0x38, 0);

	step("microcode PM4/PFP, ringbuffer, start CP");
	copy_bytes(PM4_PA, fw_pm4 + 4, sizeof(fw_pm4) - 4);
	copy_bytes(PFP_PA, fw_pfp + 4, sizeof(fw_pfp) - 4);
	for (uint32_t i = 0; i < 0x8000; i += 4)
		*(volatile uint32_t *)(RING_PA + i) = 0;
	*(volatile uint32_t *)SCRATCH_PA = 0x11111111;
	__asm__ volatile("dsb sy" ::: "memory");
	gw(0x838, (uint32_t)PM4_PA); gw(0x839, 0);
	gw(0x835, (uint32_t)PFP_PA); gw(0x836, 0);
	gw(0x800, (uint32_t)RING_PA); gw(0x801, 0);
	gw(0x802, 12u | (2u << 8) | (1u << 27));
	rb_w = 0;
	gw(0x807, 0);
	gw(0x832, 0);			/* CP_PFP_ME_CNTL: clear ME_HALT */

	emit(pkt7(0x48, 8));		/* CP_ME_INIT */
	emit(0x2f);
	emit(3);
	emit(0x20000000);
	emit(0);
	emit(0);
	emit(0);
	emit(0);
	emit(0);
	if (!kick_wait("ME_INIT"))
		goto out;
	emit(pkt7(0x66, 1));		/* CP_SET_SECURE_MODE 0 */
	emit(0);
	kick_wait("SECURE 0");
	reg("SECVID_TRUST ", GPUREG(0xf400));
	emit(pkt7(0x3d, 3));		/* CP_MEM_WRITE */
	emit((uint32_t)SCRATCH_PA);
	emit(0);
	emit(0xc0ffee42);
	kick_wait("MEM_WRITE");
	for (int i = 0; i < 1000000 && *(volatile uint32_t *)SCRATCH_PA != 0xc0ffee42; i++)
		;
	reg("scratch      ", SCRATCH_PA);
	if (*(volatile uint32_t *)SCRATCH_PA == 0xc0ffee42) {
		fg = GREEN;
		puts_("GPU EXECUTED COMMANDS\n");
		fg = WHITE;
	}
out:
	return;
}
#endif

#if defined(HAVE_GPUFW)
/* GPU firmware block for the kernel (TopazAdreno.cpp fw_get): "LVGPUFW1", count, { name[24], offset, size } */
#define LV_FW_PA	0xbf000000UL
static void lv_put_entry(int i, const char *name, uint32_t off, const unsigned char *src, uint32_t len)
{
	volatile unsigned char *e = (volatile unsigned char *)(LV_FW_PA + 16 + 32 * (uintptr_t)i);
	for (int k = 0; k < 24; k++)
		e[k] = 0;
	for (int k = 0; k < 23 && name[k]; k++)
		e[k] = (unsigned char)name[k];
	*(volatile uint32_t *)(e + 24) = off;
	*(volatile uint32_t *)(e + 28) = len;
	copy_bytes(LV_FW_PA + off, src, len);
}

static void lv_fw_block(void)
{
	volatile unsigned char *b = (volatile unsigned char *)LV_FW_PA;
	uint32_t off = 0x1000;
	lv_put_entry(0, "a512_zap.mdt", off, fw_zap_mdt, sizeof(fw_zap_mdt));
	off += (sizeof(fw_zap_mdt) + 0xfff) & ~0xfffu;
	lv_put_entry(1, "a512_zap.b02", off, fw_zap_b02, sizeof(fw_zap_b02));
	off += (sizeof(fw_zap_b02) + 0xfff) & ~0xfffu;
	lv_put_entry(2, "a530_pm4.fw", off, fw_pm4, sizeof(fw_pm4));
	off += (sizeof(fw_pm4) + 0xfff) & ~0xfffu;
	lv_put_entry(3, "a530_pfp.fw", off, fw_pfp, sizeof(fw_pfp));
	*(volatile uint32_t *)(b + 8) = 4;
	*(volatile uint32_t *)(b + 12) = 0;
	const char *magic = "LVGPUFW1";
	for (int k = 0; k < 8; k++)
		b[k] = (unsigned char)magic[k];
	__asm__ volatile("dsb sy" ::: "memory");
	puts_("GPU firmware block at 0xbf000000 (4 files)\n");
}
#endif

static void gpu_probe(void)
{
	fg = CYAN;
	puts_("\n-- GPU power probe (Adreno 512) --\n");
	fg = WHITE;
	reg("gcc gpu ahb  ", GCCB + 0x71004);
	reg("gcc gpu bimc ", GCCB + 0x71010);
	reg("gcc bimc gfx ", GCCB + 0x7106c);
	reg("gcc gpll0 gpu", GCCB + 0x5200c);
	reg("gpucc cxo    ", GPUCC + 0x1020);
	reg("gdsc cx      ", GPUCC + 0x1004);
	reg("gdsc gx      ", GPUCC + 0x1094);
	reg("gx clamp     ", GPUCC + 0x130);
	reg("gfx3d rcg    ", GPUCC + 0x1070);
	reg("gfx3d cfg    ", GPUCC + 0x1074);
	reg("gfx3d cbcr   ", GPUCC + 0x1098);
	reg("pll0 mode    ", GPUCC + 0x0);

	step("gcc: gpu ahb, bimc, gpll0 branches on");
	wr32(GCCB + 0x71004, rd32(GCCB + 0x71004) | 1);
	wr32(GCCB + 0x71010, rd32(GCCB + 0x71010) | 1);
	wr32(GCCB + 0x7106c, rd32(GCCB + 0x7106c) | 1);
	wr32(GCCB + 0x5200c, rd32(GCCB + 0x5200c) | (1u << 4) | (1u << 3));
	delay_ms(5);
	reg("gcc gpu ahb  ", GCCB + 0x71004);
	reg("gcc gpu bimc ", GCCB + 0x71010);

	step("gdsc cx on");
	wr32(GPUCC + 0x1004, rd32(GPUCC + 0x1004) & ~1u);
	puts_(poll_set(GPUCC + 0x1004, 1u << 31, 1) ? "cx PWR_ON\n" : "cx TIMEOUT\n");
	reg("gdsc cx      ", GPUCC + 0x1004);

	/* msm_gfx_ldo.c enable_bhs_mode(): GX powered from CX through the BHS switch */
#define GFXLDO 0x0506e000UL
	reg("ldo pwrsw ctl", GFXLDO + 0x1c);
	reg("ldo pwrsw st ", GFXLDO + 0x28);
	step("gfx3d root: XO 19.2 MHz, ROOT_EN (force-enable-root-clk), branches on");
	wr32(GPUCC + 0x1074, 0u);
	wr32(GPUCC + 0x1070, rd32(GPUCC + 0x1070) | 2u | 1u);
	puts_(poll_set(GPUCC + 0x1070, 1, 0) ? "rcg updated\n" : "rcg TIMEOUT\n");
	wr32(GPUCC + 0x1098, rd32(GPUCC + 0x1098) | 1);
	wr32(GPUCC + 0x10d0, rd32(GPUCC + 0x10d0) | 1);
	reg("gfx3d rcg    ", GPUCC + 0x1070);
	reg("gfx3d cbcr   ", GPUCC + 0x1098);

	step("gfx ldo init: clamp off, CPR auto-bypass, sdm660 config");
	wr32(GFXLDO + 0x1c, (rd32(GFXLDO + 0x1c) & ~(1u << 31)) | (1u << 29));
	wr32(GFXLDO + 0x00, 0x00000080);
	wr32(GFXLDO + 0x04, 0x0100a600);
	wr32(GFXLDO + 0x08, 0x000000a0);
	wr32(GFXLDO + 0x0c, 0x0000c3fe);
	wr32(GFXLDO + 0x10, 0x00000000);
	wr32(GFXLDO + 0x14, 0x00401100);
	wr32(GFXLDO + 0x2c, 0x00000230);
	wr32(GFXLDO + 0x30, 0x00000048);
	wr32(GFXLDO + 0x34, 0x00000000);
	reg("ldo pwrsw ctl", GFXLDO + 0x1c);

	step("gfx ldo: BHS mode under GDSC control");
	{
		uint32_t c = rd32(GFXLDO + 0x1c);
		c |= 1u << 4;			/* LDO_UNDER_SW_CTRL */
		wr32(GFXLDO + 0x1c, c);
		c &= ~(1u << 9);		/* PWR_SRC_SEL = BHS */
		wr32(GFXLDO + 0x1c, c);
		c &= ~(1u << 30);		/* CPR_BYPASS_IN_LDO_MODE off */
		wr32(GFXLDO + 0x1c, c);
		c &= ~(1u << 1);		/* BHS_EN_FEW to gdsc */
		wr32(GFXLDO + 0x1c, c);
		c &= ~(1u << 2);		/* BHS_EN_REST to gdsc */
		wr32(GFXLDO + 0x1c, c);
		c &= ~1u;			/* BHS under GDSC control */
		wr32(GFXLDO + 0x1c, c);
	}
	reg("ldo pwrsw ctl", GFXLDO + 0x1c);
	reg("ldo pwrsw st ", GFXLDO + 0x28);

	/* downstream gdsc.c gdsc_enable(): domain-addr 0x5065130 with reset-aon-logic:
	 * pulse GMEM_RESET (bit 4), then release GMEM clamp (bit 0), THEN power on */
	step("gx domain: aon reset pulse, clamp off");
	wr32(GPUCC + 0x130, rd32(GPUCC + 0x130) | (1u << 4));
	delay_ms(1);
	wr32(GPUCC + 0x130, rd32(GPUCC + 0x130) & ~(1u << 4));
	wr32(GPUCC + 0x130, rd32(GPUCC + 0x130) & ~1u);
	reg("gx domain    ", GPUCC + 0x130);

	step("gdsc gx on");
	wr32(GPUCC + 0x1094, rd32(GPUCC + 0x1094) & ~1u);
	int gx = poll_set(GPUCC + 0x1094, 1u << 31, 1);
	puts_(gx ? "gx PWR_ON\n" : "gx TIMEOUT\n");
	reg("gdsc gx      ", GPUCC + 0x1094);
	if (!gx)
		goto park;
	step("gx clamp off, gx BCR pulse");
	wr32(GPUCC + 0x130, rd32(GPUCC + 0x130) & ~1u);
	wr32(GPUCC + 0x1090, 1);
	delay_ms(1);
	wr32(GPUCC + 0x1090, 0);
	delay_ms(1);

	puts_(poll_set(GPUCC + 0x1098, 1u << 31, 0) ? "gfx3d branch ON\n" : "gfx3d branch HALTED\n");
	reg("gfx3d cbcr   ", GPUCC + 0x1098);
	reg("rbbmtimer    ", GPUCC + 0x10d0);

	step("read GPU registers");
	reg("RBBM_STATUS  ", GPUREG(0x4f5));
	reg("RBBM_CLK_CNTL", GPUREG(0xaf));
	reg("CP_PFP_ME    ", GPUREG(0x804));
	reg("SECVID_TRUST ", GPUREG(0xf400));
	fg = GREEN;
	puts_("GPU answered\n");
	fg = WHITE;
#if defined(HAVE_GPUFW)
	gpu_cp_probe();
#endif
park:
	fg = GREEN;
	puts_("GPU PROBE ONLY: photo, then POWER + VOL DOWN\n");
	fg = WHITE;
	for (;;)
		__asm__ volatile("wfe");
}

static void osm_probe(void)
{
	fg = CYAN;
	puts_("\n-- CPU clock hw (read only) --\n");
	fg = WHITE;
	pll_line("PLL silver", PLL_PWRCL);
	pll_line("PLL gold  ", PLL_PERFCL);
	reg("RCGR gold cmd", 0x17811050UL);
	reg("RCGR gold cfg", 0x17811054UL);
	reg("OSM ver      ", OSM_PWRCL + 0x0);
	reg("OSM sil EN   ", OSM_PWRCL + 0x1004);
	reg("OSM gold EN  ", OSM_PERFCL + 0x1004);
	reg("OSM gold DCVS", OSM_PERFCL + 0x1f10);
	puts_("gold LUT: idx freq volt override\n");
	for (int i = 0; i < 8; i++) {
		uintptr_t e = OSM_PERFCL + (uintptr_t)i * 32;
		puts_(" ");
		putdec(i);
		putc_(' ');
		puthex(rd32(e + 0x1154), 8);
		putc_(' ');
		puthex(rd32(e + 0x1158), 8);
		putc_(' ');
		puthex(rd32(e + 0x115c), 8);
		putc_('\n');
	}
}

/* ---------------- GIC for XNU (from ios-topaz test_gic/gold_gicr_prepare) ----------------
 * Topaz found XNU hangs after initCPU on Gold without this: GICD EnableGrp1/ARE_NS,
 * every Gold redistributor awake, SGIs/PPIs in Group 1 with a default priority.
 * Same final state as topaz, minus its interrupt test windows.
 */
#define GICR_SIZE	0x100000UL
#define GICR_PE_SIZE	0x20000UL
#define GICR_TYPER	0x08
#define GICR_WAKER	0x14
#define GICR_SGI	0x10000
#define GICR_IGROUPR0	(GICR_SGI + 0x080)
#define GICR_ISENABLER0	(GICR_SGI + 0x100)
#define GICR_ICENABLER0	(GICR_SGI + 0x180)
#define GICR_IPRIORITYR	(GICR_SGI + 0x400)
#define VTIMER_INTID	19	/* Qualcomm PPI 3 */
#define PTIMER_INTID	18
#define SGI_INTID	1

static uintptr_t gicr_for(uint32_t aff)
{
	for (uintptr_t off = 0; off < GICR_SIZE; off += GICR_PE_SIZE) {
		uintptr_t r = GICR_BASE + off;
		uint64_t typer = (uint64_t)rd32(r + GICR_TYPER + 4) << 32 | rd32(r + GICR_TYPER);
		if ((uint32_t)(typer >> 32) == aff)
			return r;
		if (typer & (1 << 4))	/* Last */
			break;
	}
	return 0;
}

static void gicr_setup(uintptr_t r, int enable_timers)
{
	uint32_t waker = rd32(r + GICR_WAKER);
	if (waker & 2) {
		wr32(r + GICR_WAKER, waker & ~2U);
		for (int i = 0; i < 100000 && (rd32(r + GICR_WAKER) & 4); i++)
			;
	}
	wr32(r + GICR_IGROUPR0, 0xffffffff);
	for (int id = 0; id < 32; id += 4)
		wr32(r + GICR_IPRIORITYR + id, 0xa0a0a0a0);
	if (enable_timers)
		wr32(r + GICR_ISENABLER0, (1U << VTIMER_INTID) | (1U << PTIMER_INTID) | (1U << SGI_INTID));
	else
		wr32(r + GICR_ICENABLER0, 0xffffffff);
}

static void gic_prepare_for_xnu(void)
{
	uint64_t sre;
	__asm__ volatile("mrs %0, S3_0_C12_C12_5" : "=r"(sre));	/* ICC_SRE_EL1 */
	if (!(sre & 1)) {
		__asm__ volatile("msr S3_0_C12_C12_5, %0; isb" : : "r"(sre | 1));
	}
	uint32_t d = rd32(GICD_BASE);
	wr32(GICD_BASE, d | 0x13);		/* EnableGrp1, EnableGrp1A, ARE_NS */
	for (int i = 0; i < 1000000 && (rd32(GICD_BASE) & (1U << 31)); i++)
		;
	kv("GICD_CTLR     ", rd32(GICD_BASE), 8);
	uint32_t me = (uint32_t)(RD(mpidr_el1) & 0xffffff);
	/* the boot core's cluster: gold 0x100-0x103 (16K kernel) or silver 0x0-0x3 (4K) */
	uint32_t first = me & ~0xffU;
	for (uint32_t aff = first; aff <= first + 3; aff++) {
		uintptr_t r = gicr_for(aff);
		puts_("GICR ");
		puthex(aff, 3);
		if (!r) {
			fg = RED;
			puts_(" not found\n");
			fg = WHITE;
			continue;
		}
		/* topaz: boot CPU ends with its PPIs disabled, the others with timers+SGI1 enabled */
		gicr_setup(r, aff != me);
		puts_(" @");
		puthex(r, 8);
		puts_(" waker ");
		puthex(rd32(r + GICR_WAKER), 2);
		puts_(" en ");
		puthex(rd32(r + GICR_ISENABLER0), 8);
		putc_('\n');
	}
	/* CPU interface: EOImode 0, PMR open, group 1 on */
	uint64_t ctlr;
	__asm__ volatile("mrs %0, S3_0_C12_C12_4" : "=r"(ctlr));	/* ICC_CTLR_EL1 */
	__asm__ volatile("msr S3_0_C12_C12_4, %0" : : "r"(ctlr & ~2ULL));
	__asm__ volatile("msr S3_0_C4_C6_0, %0" : : "r"(0xf0UL));	/* ICC_PMR_EL1 */
	__asm__ volatile("msr S3_0_C12_C12_3, %0" : : "r"(0UL));	/* ICC_BPR1_EL1 */
	__asm__ volatile("msr S3_0_C12_C12_7, %0; isb" : : "r"(1UL));	/* ICC_IGRPEN1_EL1 */
}

/* ---------------- XNU loader (from ios-topaz loader.c) ----------------
 * tools/mkpayload.py resolved the kernel's chained fixups and packed the
 * segments + ADT + boot-args; build.ps1 appends that payload to the Image at
 * the first 4K boundary after _end. Copy the segments to phys_base, place the
 * ADT and boot_args behind the kernel, jump with MMU and caches off.
 */
#define BOOT_LINE_LENGTH	608
#define PAGE_16K		0x4000UL

struct payload_seg {
	uint64_t vmaddr, vmsize, file_off, file_size;
};

struct payload {
	char magic[8];
	uint32_t version, nsegs;
	uint64_t virt_base, entry_va, adt_off, adt_size, kernel_end;
	uint64_t phys_base, mem_size;
	char cmdline[BOOT_LINE_LENGTH];
	struct payload_seg segs[];
};

struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

struct boot_args {
	uint16_t Revision, Version;
	uint64_t virtBase, physBase, memSize, topOfKernelData;
	struct boot_video Video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char CommandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags, memSizeActual;
};

_Static_assert(__builtin_offsetof(struct payload, segs) == 0x2a8, "payload header layout");
_Static_assert(__builtin_offsetof(struct boot_args, Video) == 40, "boot_args layout");
_Static_assert(__builtin_offsetof(struct boot_args, CommandLine) == 108, "boot_args layout");
_Static_assert(sizeof(struct boot_args) == 736, "boot_args layout");

extern char _end[];

static void copy64(uintptr_t dst, uintptr_t src, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	volatile const uint64_t *s = (volatile const uint64_t *)src;
	for (uint64_t i = 0; i < (n + 7) / 8; i++)
		d[i] = s[i];
}

static void zero64(uintptr_t dst, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	for (uint64_t i = 0; i < n / 8; i++)
		d[i] = 0;
}

static const struct payload *find_payload(void)
{
	const char *m = (const char *)((((uintptr_t)_end) + 0xfff) & ~0xfffUL);
	const char want[8] = { 'S', 'W', 'E', 'E', 'T', 'X', 'N', 'U' };
	for (int i = 0; i < 8; i++)
		if (m[i] != want[i])
			return 0;
	return (const struct payload *)m;
}

typedef void (*xnu_entry_t)(uint64_t boot_args_pa);

static void __attribute__((noreturn)) boot_xnu(const struct payload *p)
{
	uintptr_t src = (uintptr_t)p;
	fg = CYAN;
	puts_("\n-- XNU loader --\n");
	fg = WHITE;
	if (p->version != 1 || (p->adt_off & 7)) {
		fg = RED;
		puts_("bad payload header\n");
		goto stop;
	}
	uint64_t pa_of_va_base = p->phys_base - p->virt_base;
	kv("payload       ", src, 9);
	kv("phys_base     ", p->phys_base, 9);
	kv("mem_size      ", p->mem_size, 9);
	kv("virt_base     ", p->virt_base, 16);
	kv("entry         ", p->entry_va, 16);
	puts_("segments      ");
	putdec(p->nsegs);
	putc_('\n');
	if (src + 0x8000000 > p->phys_base && src < p->phys_base + p->mem_size) {
		fg = RED;
		puts_("payload overlaps kernel memory\n");
		goto stop;
	}
	for (uint32_t i = 0; i < p->nsegs; i++) {
		const struct payload_seg *s = &p->segs[i];
		uintptr_t dst = s->vmaddr + pa_of_va_base;
		copy64(dst, src + s->file_off, s->file_size);
		if (s->vmsize > s->file_size)
			zero64(dst + ((s->file_size + 7) & ~7UL), s->vmsize - ((s->file_size + 7) & ~7UL));
	}
	uintptr_t adt_pa = p->kernel_end + pa_of_va_base;
	copy64(adt_pa, src + p->adt_off, p->adt_size);
	uintptr_t ba_pa = (adt_pa + p->adt_size + PAGE_16K - 1) & ~(PAGE_16K - 1);
	uintptr_t top = ba_pa + PAGE_16K;
	zero64(ba_pa, PAGE_16K);
	volatile struct boot_args *ba = (volatile struct boot_args *)ba_pa;
	ba->Revision = 2;
	ba->Version = 2;
	ba->virtBase = p->virt_base;
	ba->physBase = p->phys_base;
	ba->memSize = p->mem_size;
	ba->topOfKernelData = top;
	ba->Video.v_baseAddr = FB_BASE;
	ba->Video.v_display = 1;
	ba->Video.v_rowBytes = FB_STRIDE;
	ba->Video.v_width = FB_W;
	ba->Video.v_height = FB_H;
	ba->Video.v_depth = 32;
	ba->deviceTreeP = adt_pa - pa_of_va_base;	/* XNU reads it with the MMU on: a VA */
	ba->deviceTreeLength = (uint32_t)p->adt_size;
	for (int i = 0; i < BOOT_LINE_LENGTH; i++)
		ba->CommandLine[i] = p->cmdline[i];
	ba->memSizeActual = p->mem_size;
	kv("boot_args     ", ba_pa, 9);
	kv("top           ", top, 9);

#if defined(HAVE_GPUFW)
	lv_fw_block();
#endif
	gic_prepare_for_xnu();
	uintptr_t entry_pa = p->entry_va + pa_of_va_base;
	kv("JUMP          ", entry_pa, 9);
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
	((xnu_entry_t)entry_pa)(ba_pa);
	fg = RED;
	puts_("xnu returned\n");
stop:
	__asm__ volatile("dsb sy" ::: "memory");
	for (;;)
		__asm__ volatile("wfe");
}

static void __attribute__((noreturn)) gold_main(void)
{
	uint64_t mmfr0 = RD(id_aa64mmfr0_el1);
	const struct payload *pl = find_payload();
	if (pl) {
		/* real boot: stay with MMU off (XNU sets up its own), go straight to the loader */
		fg = CYAN;
		puts_("\n== GOLD: payload found, booting XNU ==\n");
		fg = WHITE;
		print_cpu("gold", RD(mpidr_el1), RD(midr_el1), mmfr0);
		/* clock probes v6-v12 (dump/shim-probe-v6-12.md); park forever, so off */
		if (0)
			gold_clock_tune();
		if (GPU_PROBE_ON_GOLD) {
			struct mmu_info gm;
			int mr = mmu_enable(&gm);
			puts_(mr ? "gold MMU enable FAILED\n" : "gold: MMU on (16K, Device identity map)\n");
			gpu_probe();
		}
		boot_xnu(pl);
	}

	fg = CYAN;
	puts_("\n== now running on GOLD ==\n");
	fg = WHITE;
	print_cpu("gold", RD(mpidr_el1), RD(midr_el1), mmfr0);
	kv("MMFR0 gold    ", mmfr0, 16);
	uint64_t gt = speed_ticks();
	puts_("speed ticks: silver ");
	putdec((int64_t)silver_ticks);
	puts_(" gold ");
	putdec((int64_t)gt);
	putc_('\n');

	if (((mmfr0 >> 20) & 0xf) == 0) {
		fg = RED;
		puts_("no 16K granule on this core - stop\n");
		goto park;
	}

	struct mmu_info mi;
	int r = mmu_enable(&mi);
	/* from here on: MMU on, 16K pages, image cacheable, fb write-combining */
	fg = r ? RED : GREEN;
	puts_(r ? "MMU enable FAILED\n" : "MMU ON (16K granule)\n");
	fg = WHITE;
	kv("TTBR0         ", mi.ttbr, 9);
	puts_("image         ");
	puthex(mi.img_lo, 9);
	puts_("..");
	puthex(mi.img_hi, 9);
	puts_(" L3 tables ");
	putdec(mi.n_l3);
	putc_('\n');
	kv("SCTLR before  ", mi.sctlr_before, 8);
	kv("SCTLR after   ", mi.sctlr_after, 8);
	kv("TCR_EL1       ", RD(tcr_el1), 9);
	par("PAR image  ", mi.img_lo);
	par("PAR fb     ", MMU_FB_PHYS);
	par("PAR GICD   ", 0x17a00000UL);
	par("PAR RAM hi ", 0x100000000UL);
	par("PAR 20G    ", 0x500000000UL);

	/* cached memory works: write, clean, read back */
	uint64_t bad = 0;
	for (int i = 0; i < 512; i++)
		test_buf[i] = 0x5a5a000000000000UL ^ ((uint64_t)i * 0x9e3779b97f4a7c15UL);
	for (int i = 0; i < 512; i++)
		if (test_buf[i] != (0x5a5a000000000000UL ^ ((uint64_t)i * 0x9e3779b97f4a7c15UL)))
			bad++;
	puts_("cached RAM test: ");
	fg = bad ? RED : GREEN;
	puts_(bad ? "BAD\n" : "ok\n");
	fg = WHITE;
	puts_("speed ticks with MMU+caches: ");
	putdec((int64_t)speed_ticks());
	putc_('\n');

	uint64_t t0 = ticks();
	volatile uint32_t *fb = (volatile uint32_t *)MMU_FB_PHYS;
	for (int y = FB_H - 6 * CH_H; y < FB_H - 4 * CH_H; y++)
		for (int x = 0; x < FB_W; x++)
			fb[y * FB_W + x] = 0xff2040a0;
	puts_("fb band (2 rows) drawn in ticks: ");
	putdec((int64_t)(ticks() - t0));
	putc_('\n');
	osm_probe();
	/* v4 experiment: mux switch latched (cfg 0x301) but gave no speedup; off by default */
	if (0)
		gold_mux_to_pll();
park:
	fg = CYAN;
	puts_("\nDONE v5. Take a photo, then hold POWER ~10 s\n");
	__asm__ volatile("dsb sy" ::: "memory");
	for (;;)
		__asm__ volatile("wfe");
}

static void silver_pll_relock_from_gold(void);

void secondary_main(uint64_t slot)
{
	if (gold_go == 2)
		silver_pll_relock_from_gold();
	if (gold_go)
		gold_main();
	probe_secondary(slot);
}

/* ---------------- exceptions ---------------- */
void exc_handler(uint64_t slot, uint64_t esr, uint64_t elr, uint64_t far)
{
	fg = RED;
	puts_("\n*** EXCEPTION slot ");
	putdec((int64_t)slot);
	kv("\n ESR ", esr, 8);
	kv(" ELR ", elr, 10);
	kv(" FAR ", far, 10);
	puts_("hold POWER 10s to reboot\n");
}

/* ---------------- lavender CPU clock: silver PLL relock from gold (2026-11-20) ---------------- */
/*
 * Silver (cpu0-3, where the 4K XNU runs) is clocked straight from its PLL (pwrcl 0x17916000,
 * L * 19.2 MHz; XBL leaves L 72 = 1382 MHz) and bypasses its RCG, so its PLL cannot be relocked
 * from silver itself. Gold 0x100 is woken with PSCI CPU_ON, relocks the silver PLL at the new L
 * (the sequence gold_pll_relock() used on the gold PLL), says so and switches itself off; silver
 * stalls while its clock is away and goes on afterwards. No voltage is changed: XBL's APC0 level
 * (CPRh closed loop for its corner) stays. OSM speed-bin table (DT qcom,pwrcl-speedbin0-v0):
 * L 15 288, 33 633.6, 47 902.4, 58 1113.6, 73 1401.6, 80 1536, 91 1747.2, 96 1843.2 MHz.
 *
 * Payload boot-args read by the shim (they also reach XNU, which ignores them):
 *   lv_cpudiag=1     read-only APC/CPRh/SAW/APM dump, then a silver step test (L 76..96, a
 *                    checksum loop at each step), no XNU; stops at the first wrong checksum
 *   lv_silver_l=NN   relock silver at L NN (40..100) before XNU
 */
#define PSCI_CPU_OFF		0x84000002UL
#define PSCI_AFFINITY_INFO	0xc4000004UL
static volatile uint32_t relock_l, relock_done;

static void silver_pll_relock_from_gold(void)
{
	uint32_t l = relock_l;

	wr32(PLL_PWRCL + 0x0, 0x0);
	wr32(PLL_PWRCL + 0x4, l);
	wr32(PLL_PWRCL + 0x0, 0x2);
	for (volatile int i = 0; i < 20000; i++)
		;
	wr32(PLL_PWRCL + 0x0, 0x6);
	for (volatile int i = 0; i < 100000; i++)
		;
	wr32(PLL_PWRCL + 0x0, 0x7);
	for (int n = 0; !(rd32(PLL_PWRCL + 0x2c) & (1u << 16)) && n < 1000000; n++)
		;
	relock_done = 1;
	__asm__ volatile("dsb sy" ::: "memory");
	(void)smc(PSCI_CPU_OFF, 0, 0, 0);
	for (;;)
		__asm__ volatile("wfe");
}

static int silver_relock(uint32_t l)
{
	relock_l = l;
	relock_done = 0;
	gold_go = 2;
	__asm__ volatile("dsb sy" ::: "memory");
	/* (gold may still be shutting down from the previous step) */
	for (int n = 0; n < 1000 && smc(PSCI_AFFINITY_INFO, GOLD_MPIDR, 0, 0) != 1; n++)
		for (volatile int i = 0; i < 10000; i++)
			;
	int64_t r = (int64_t)smc(PSCI_CPU_ON, GOLD_MPIDR, (uintptr_t)secondary_entry, 4);
	if (r != 0)
		return (int)r;
	uint64_t t = ticks() + RD(cntfrq_el0);
	while (!relock_done && ticks() < t)
		;
	return relock_done ? 0 : -100;
}

static const char *bootarg(const char *cmdline, const char *key)
{
	for (const char *p = cmdline; p && *p; p++) {
		if (p != cmdline && p[-1] != ' ')
			continue;
		const char *k = key, *q = p;
		while (*k && *q == *k) {
			k++;
			q++;
		}
		if (*k == 0)
			return q;
	}
	return 0;
}

static uint32_t bootarg_u32(const char *cmdline, const char *key, uint32_t dflt)
{
	const char *v = bootarg(cmdline, key);
	uint32_t n = 0;
	if (!v || *v < '0' || *v > '9')
		return dflt;
	while (*v >= '0' && *v <= '9')
		n = n * 10 + (uint32_t)(*v++ - '0');
	return n;
}

/* ALU + memory checksum, the same every time on a healthy core */
static uint64_t stress_sum(void)
{
	uint64_t x = 0x9e3779b97f4a7c15UL, s = 0;
	for (int r = 0; r < 200; r++) {
		for (int i = 0; i < 512; i++) {
			x ^= x << 13;
			x ^= x >> 7;
			x ^= x << 17;
			test_buf[i] = x + (uint64_t)i * 0x100000001UL;
		}
		for (int i = 0; i < 512; i++)
			s = (s ^ test_buf[i]) * 0x100000001b3UL + (s >> 29);
	}
	return s;
}

static void cpu_diag_dump(void)
{
	fg = CYAN;
	puts_("\n-- lavender CPU diag (read only) --\n");
	fg = WHITE;
	pll_line("PLL silver", PLL_PWRCL);
	pll_line("PLL gold  ", PLL_PERFCL);
	reg("silver USER_CTL ", PLL_PWRCL + 0xc);
	reg("gold   USER_CTL ", PLL_PERFCL + 0xc);
	reg("silver RCG cmd  ", 0x17911050UL);
	reg("silver RCG cfg  ", 0x17911054UL);
	reg("gold   RCG cmd  ", 0x17811050UL);
	reg("gold   RCG cfg  ", 0x17811054UL);
	reg("SAW4 cl0 PMICSTS", 0x17912c18UL);
	reg("SAW4 cl1 PMICSTS", 0x17812c18UL);
	reg("CPRH apc0 STATUS", 0x179cbaa4UL);
	reg("CPRH apc1 STATUS", 0x179c7aa4UL);
	reg("APM cl0 mode    ", 0x179d0004UL);
	reg("APM cl0 status  ", 0x179d000cUL);
	reg("APM cl1 mode    ", 0x179d0010UL);
	reg("APM cl1 status  ", 0x179d0018UL);
	reg("ITM present     ", 0x179d143cUL);
	reg("OSM pwrcl 0x000 ", OSM_PWRCL + 0x000);
	reg("OSM pwrcl 0x900 ", OSM_PWRCL + 0x900);
	reg("efuse 0x784130  ", 0x784130UL);
	reg("efuse 0x784134  ", 0x784134UL);
	/* (CPRh apc0 at 0x179c8000 is not readable from EL1: synchronous external abort) */
	puts_("silver ");
	putdec((int64_t)gold_mhz());
	puts_(" MHz now\n");
}

static void cpu_step_test(void)
{
	static const uint32_t steps[] = { 72, 76, 80, 84, 88, 92, 96 };
	uint64_t ref = stress_sum();
	fg = CYAN;
	puts_("\n-- silver step test (no voltage change) --\n");
	fg = WHITE;
	for (unsigned i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
		puts_("L ");
		putdec(steps[i]);
		puts_(" (");
		putdec((int64_t)steps[i] * 192 / 10);
		puts_(" MHz): relock ");
		__asm__ volatile("dsb sy" ::: "memory");
		int r = silver_relock(steps[i]);
		if (r != 0) {
			fg = RED;
			puts_("FAILED ");
			putdec(r);
			putc_('\n');
			fg = WHITE;
			return;
		}
		puts_("ok, ");
		putdec((int64_t)gold_mhz());
		puts_(" MHz measured, checksums ");
		int bad = 0;
		for (int k = 0; k < 20; k++)
			bad += stress_sum() != ref;
		if (bad) {
			fg = RED;
			putdec(bad);
			puts_("/20 WRONG\n");
			fg = WHITE;
			puts_("back to L 72\n");
			silver_relock(72);
			return;
		}
		fg = GREEN;
		puts_("20/20 ok\n");
		fg = WHITE;
	}
	fg = GREEN;
	puts_("all steps passed (L 96 = 1843 MHz)\n");
	fg = WHITE;
}

/* ---------------- v2 main: silver -> gold hand-off ---------------- */
void shim_main(const unsigned char *dtb, uintptr_t load)
{
	con_clear();
	fg = CYAN;
	puts_("lavender shim v5 (gold + XNU loader)\n\n");
	fg = WHITE;
	kv("load addr     ", load, 9);
	kv("CurrentEL     ", RD(currentel) >> 2, 1);
	print_cpu("boot", RD(mpidr_el1), RD(midr_el1), RD(id_aa64mmfr0_el1));
	if (dtb && be32(dtb) == 0xd00dfeed) {
		print_reserved = 0;
		fdt_walk(dtb);
	}
	uint64_t pv = smc(PSCI_VERSION, 0, 0, 0);
	puts_("PSCI ");
	putdec((int64_t)(pv >> 16));
	putc_('.');
	putdec((int64_t)(pv & 0xffff));
	putc_('\n');

	silver_ticks = speed_ticks();
	puts_("silver speed ticks ");
	putdec((int64_t)silver_ticks);
	puts_(" = ");
	putdec((int64_t)gold_mhz());
	puts_(" MHz");
	{
		/*
		 * A 4 KiB-page kernel (TOPAZ_4K, compact KVA at 0xfffffff0...) runs on
		 * the silver cluster: silver has no 16K granule, but gold only runs at
		 * 1/4 clock. Boot it right here on silver cpu0.
		 */
		const struct payload *pl = find_payload();
		if (0)
			mdp_probe();
		if (0)
			gpu_probe();
		if (pl && bootarg_u32(pl->cmdline, "lv_cpudiag=", 0)) {
			cpu_diag_dump();
			cpu_step_test();
			cpu_diag_dump();
			fg = CYAN;
			puts_("\nCPU diag done. Photo, then POWER+VOL- for fastboot\n");
			for (;;)
				__asm__ volatile("wfe");
		}
		if (pl) {
			uint32_t l = bootarg_u32(pl->cmdline, "lv_silver_l=", 0);
			if (l >= 40 && l <= 100) {
				puts_("\nsilver PLL -> L ");
				putdec(l);
				int r = silver_relock(l);
				puts_(r == 0 ? " ok, " : " FAILED, ");
				putdec((int64_t)gold_mhz());
				puts_(" MHz\n");
			}
		}
		if (pl && pl->virt_base >= 0xfffffff000000000UL && !GPU_PROBE_ON_GOLD) {
			fg = CYAN;
			puts_("\n== SILVER: 4K payload found, booting XNU ==\n");
			fg = WHITE;
			boot_xnu(pl);
		}
	}
	puts_("\nwaking gold 0x100, silver parks\n");
	gold_go = 1;
	__asm__ volatile("dsb sy" ::: "memory");
	int64_t r = (int64_t)smc(PSCI_CPU_ON, GOLD_MPIDR, (uintptr_t)secondary_entry, 4);
	if (r == 0)
		for (;;)	/* parked, DAIF masked; gold owns the screen now */
			__asm__ volatile("wfe");
	gold_go = 0;
	fg = RED;
	puts_("CPU_ON gold failed: ");
	putdec(r);
	putc_('\n');
	for (;;)
		__asm__ volatile("wfe");
}

/* ---------------- v1 probe (kept for reference) ---------------- */
void __attribute__((unused)) probe_v1(const unsigned char *dtb, uintptr_t load)
{
	con_clear();
	fg = CYAN;
	puts_("lavender probe shim v1\n\n");
	fg = WHITE;

	kv("load addr     ", load, 9);
	kv("CurrentEL     ", RD(currentel) >> 2, 1);
	kv("SCTLR_EL1     ", RD(sctlr_el1), 8);
	kv("CNTFRQ        ", RD(cntfrq_el0), 8);
	kv("ID_AA64PFR0   ", RD(id_aa64pfr0_el1), 16);
	kv("ID_AA64MMFR0  ", RD(id_aa64mmfr0_el1), 16);
	kv("ID_AA64MMFR1  ", RD(id_aa64mmfr1_el1), 16);
	kv("ID_AA64ISAR0  ", RD(id_aa64isar0_el1), 16);
	print_cpu("boot", RD(mpidr_el1), RD(midr_el1), RD(id_aa64mmfr0_el1));
	putc_('\n');

	/* DTB from ABL: real RAM map + reserved regions */
	kv("DTB           ", (uintptr_t)dtb, 9);
	if (dtb && be32(dtb) == 0xd00dfeed) {
		kv("DTB size      ", be32(dtb + 4), 8);
		fdt_walk(dtb);
	} else {
		fg = RED;
		puts_("no FDT magic at x0\n");
		fg = WHITE;
	}
	putc_('\n');

	/* PSCI + bring up every other CPU just long enough to read its ID regs */
	uint64_t pv = smc(PSCI_VERSION, 0, 0, 0);
	puts_("PSCI ");
	putdec((int64_t)(pv >> 16));
	putc_('.');
	putdec((int64_t)(pv & 0xffff));
	puts_("  cpus in DT: ");
	putdec(ncpu);
	putc_('\n');
	uint64_t me = RD(mpidr_el1) & 0xffffff;
	for (int i = 0; i < ncpu; i++) {
		if ((cpu_mpidr[i] & 0xffffff) == me)
			continue;
		int64_t r = (int64_t)smc(PSCI_CPU_ON, cpu_mpidr[i], (uintptr_t)secondary_entry, (uint64_t)i);
		uint64_t t = ticks() + RD(cntfrq_el0) / 5;
		while (!sec[i].done && ticks() < t)
			;
		if (sec[i].done) {
			char tag[6] = { 'c', 'p', 'u', (char)('0' + i), ' ', 0 };
			print_cpu(tag, sec[i].mpidr, sec[i].midr, sec[i].mmfr0);
		} else {
			fg = YELLOW;
			puts_("cpu");
			putdec(i);
			puts_(" mpidr ");
			puthex(cpu_mpidr[i], 6);
			puts_(" CPU_ON=");
			putdec(r);
			puts_(" no answer\n");
			fg = WHITE;
		}
	}
	putc_('\n');

	/* GIC last: if the MMIO faults, the exception handler shows it */
	puts_("GIC: ");
	kv("GICD_CTLR ", rd32(GICD_BASE + 0x0), 8);
	kv("     GICD_TYPER ", rd32(GICD_BASE + 0x4), 8);
	kv("     GICD_IIDR  ", rd32(GICD_BASE + 0x8), 8);
	kv("     GICD_PIDR2 ", rd32(GICD_BASE + 0xffe8), 8);
	kv("     GICR0_TYPER", (uint64_t)rd32(GICR_BASE + 0xc) << 32 | rd32(GICR_BASE + 0x8), 16);
	kv("     ICC_SRE    ", RD(S3_0_C12_C12_5), 8);

	fg = CYAN;
	puts_("\nDONE. Take a photo, then hold POWER ~10 s\n");
	fg = WHITE;
	/* stay on screen until the user reboots by hand */
	for (;;)
		__asm__ volatile("wfe");
}
