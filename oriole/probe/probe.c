/*
 * oriole (Pixel 6, gs101) bring-up probe, step 1 of the port. One
 * `fastboot boot` answers what the XNU port depends on:
 *   - entry EL and EL2 state (Linux says all CPUs start at EL2 here)
 *   - ID registers on each cluster (A55 / A76 / X1): 16K granule, LSE, PAN
 *   - PSCI: version, features, CPU_ON for the 7 secondaries
 *   - GICv3 distributor/redistributor state, Exynos watchdog state
 *   - which DRAM address the display DMA scans out (expected 0xfac00000)
 * Output goes to the bootloader framebuffer (mainline simple-framebuffer at
 * 0xfac00000) and to the ramoops console zone at 0xfd3ff000, which Android
 * shows as /sys/fs/pstore/console-ramoops-0 after the PSCI reset at the end.
 */
#include <stdarg.h>
#include <stdint.h>
#include "../../shim/font.h"

#define FB_BASE		0xfac00000UL	/* mainline gs101: splash@fac00000 */
#define FB_W		1080
#define FB_H		2400
#define FB_STRIDE	(FB_W * 4)

#define RAMOOPS_BASE	0xfd3ff000UL	/* live DT: ramoops_mem, console zone first */
#define RAMOOPS_CONSOLE	0x200000UL	/* console-size */
#define PRZ_SIG		0x43474244U	/* "DBGC": PERSISTENT_RAM_SIG ^ 0 */

#define GICD_BASE	0x10400000UL
#define GICR_BASE	0x10440000UL
#define GICR_STRIDE	0x20000UL
#define WDT_CL0		0x10060000UL
#define WDT_CL1		0x10070000UL
#define DPP_DMA(ch)		(0x1c0b0000UL + (ch) * 0x1000UL)
#define RDMA_IN_CTRL_0		0x08
#define RDMA_SRC_SIZE		0x10
#define RDMA_IMG_SIZE		0x18
#define RDMA_BASEADDR_Y8	0x40
#define DECON0			0x1c300000UL
#define DECON_TRIG_CON		0x30
#define DECON_TRIG_HW		((1U << 13) | (1U << 12) | (1U << 6) | (1U << 5) | 1U)
#define DECON_SHD_REG_UP_REQ	0x50
#define DECON_SHD_UP_ALL	((1U << 31) | (1U << 20) | 0x3fU)

#define PSCI_VERSION		0x84000000UL
#define PSCI_CPU_SUSPEND	0xc4000001UL
#define PSCI_CPU_OFF		0x84000002UL
#define PSCI_CPU_ON		0xc4000003UL
#define PSCI_MIGRATE_INFO_TYPE	0x84000006UL
#define PSCI_SYSTEM_OFF		0x84000008UL
#define PSCI_SYSTEM_RESET	0x84000009UL
#define PSCI_FEATURES		0x8400000aUL
#define PSCI_SYSTEM_SUSPEND	0xc400000eUL
#define PSCI_SYSTEM_RESET2	0xc4000012UL
#define SMCCC_VERSION		0x80000000UL

#define NR_CPUS		8
#define PHOTO_DELAY_S	10

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define wsr(r, v) __asm__ volatile("msr " r ", %0" : : "r"((uint64_t)(v)))

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline uint64_t rd64(uintptr_t a) { return *(volatile uint64_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

extern char secondary_entry[], _head[];

static unsigned cur_el;

/* ---- output: framebuffer text + ramoops console ---- */

#define SCALE	2
#define CH_W	(8 * SCALE)
#define CH_H	(16 * SCALE)
#define MARGIN	24
#define COLS	((FB_W - 2 * MARGIN) / CH_W)
#define ROWS	((FB_H - 2 * MARGIN) / CH_H)
#define FG	0xffffffffU
#define BG	0xff202060U
#define BG_ERR	0xff800000U

static void display_kick(void)
{
	wr32(DECON0 + DECON_TRIG_CON, DECON_TRIG_HW);
	wr32(DECON0 + DECON_SHD_REG_UP_REQ, DECON_SHD_UP_ALL);
}

static uint32_t con_bg = BG;
static int con_row, con_col;
static uint32_t prz_len;

static void fb_clear(uint32_t bg)
{
	volatile uint32_t *fb = (volatile uint32_t *)FB_BASE;
	for (int i = 0; i < FB_W * FB_H; i++)
		fb[i] = bg;
	con_bg = bg;
	con_row = con_col = 0;
}

static void fb_char(char ch)
{
	if (ch == '\n' || con_col >= COLS) {
		con_row++;
		con_col = 0;
		if (ch == '\n')
			return;
	}
	if (con_row >= ROWS)
		return;
	if (ch < 0x20 || ch > 0x7e)
		ch = '?';
	const unsigned char *g = font8x16[ch - 0x20];
	int x0 = MARGIN + con_col * CH_W, y0 = MARGIN + con_row * CH_H;
	for (int gy = 0; gy < 16; gy++)
		for (int gx = 0; gx < 8; gx++) {
			uint32_t c = (g[gy] & (0x80 >> gx)) ? FG : con_bg;
			for (int sy = 0; sy < SCALE; sy++) {
				volatile uint32_t *p = (volatile uint32_t *)FB_BASE +
					(y0 + gy * SCALE + sy) * (FB_STRIDE / 4) + x0 + gx * SCALE;
				for (int sx = 0; sx < SCALE; sx++)
					p[sx] = c;
			}
		}
	con_col++;
}

/* persistent_ram_buffer { u32 sig; u32 start; u32 size; u8 data[]; } */
static void prz_init(void)
{
	wr32(RAMOOPS_BASE + 0, PRZ_SIG);
	wr32(RAMOOPS_BASE + 4, 0);
	wr32(RAMOOPS_BASE + 8, 0);
	prz_len = 0;
}

static void prz_char(char ch)
{
	if (prz_len >= RAMOOPS_CONSOLE - 12)
		return;
	*(volatile uint8_t *)(RAMOOPS_BASE + 12 + prz_len) = (uint8_t)ch;
	prz_len++;
	/* start == size: the whole buffer is one unwrapped record */
	wr32(RAMOOPS_BASE + 8, prz_len);
	wr32(RAMOOPS_BASE + 4, prz_len);
}

static void out_char(char ch)
{
	fb_char(ch);
	prz_char(ch);
}

static void out_hex(uint64_t v, int digits)
{
	for (int i = digits - 1; i >= 0; i--)
		out_char("0123456789abcdef"[(v >> (4 * i)) & 0xf]);
}

/* %x (u32), %lx (u64, 16 digits), %d (u32 decimal), %s, %c */
static void out(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	for (const char *p = fmt; *p; p++) {
		if (*p != '%') {
			out_char(*p);
			continue;
		}
		p++;
		if (*p == 'l' && p[1] == 'x') {
			p++;
			out_hex(va_arg(ap, uint64_t), 16);
		} else if (*p == 'x') {
			out_hex(va_arg(ap, uint32_t), 8);
		} else if (*p == 'd') {
			char buf[12];
			int n = 0;
			uint32_t v = va_arg(ap, uint32_t);
			do
				buf[n++] = '0' + v % 10;
			while ((v /= 10) && n < 11);
			while (n)
				out_char(buf[--n]);
		} else if (*p == 's') {
			for (const char *s = va_arg(ap, const char *); *s; s++)
				out_char(*s);
		} else if (*p == 'c') {
			out_char((char)va_arg(ap, int));
		} else {
			out_char('%');
			if (!*p)
				break;
			out_char(*p);
		}
	}
	va_end(ap);
	display_kick();
}

/* ---- helpers ---- */

static uint64_t smc(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile("smc #0"
			 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
			 :
			 : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
			   "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

static uint64_t now(void)
{
	__asm__ volatile("isb");
	return rsr("cntpct_el0");
}

static void delay_ms(unsigned ms)
{
	uint64_t end = now() + rsr("cntfrq_el0") / 1000 * ms;
	while (now() < end)
		;
}

/* WTCON/WTDAT/WTCNT of both watchdogs as the bootloader left them. */
static uint32_t wdt_regs[2][3];

/*
 * PSCI SYSTEM_RESET is NOT_SUPPORTED on this firmware (probe 2). Fall back to
 * WDT0: the bootloader runs it with reset enabled (WTCON 0x1af39), so restore
 * that WTCON with a tiny count. The ramoops console zone survives this kind
 * of reset (as it survived Power + Volume up).
 */
static void __attribute__((noreturn)) reset_after(unsigned s)
{
	out("RESET in %d s (PSCI SYSTEM_RESET, then WDT0)\n", s);
	delay_ms(s * 1000);
	__asm__ volatile("dsb sy" ::: "memory");
	smc(PSCI_SYSTEM_RESET, 0, 0, 0);
	uint32_t con = wdt_regs[0][0];
	if (!(con & (1U << 5)) || !(con & 1U))
		con = 0x1af39;
	out("PSCI reset returned; WDT0 con %x cnt 0x100\n", con);
	wr32(WDT_CL0, 0);
	wr32(WDT_CL0 + 4, 0x100);
	wr32(WDT_CL0 + 8, 0x100);
	wr32(WDT_CL0, con);
	__asm__ volatile("dsb sy" ::: "memory");
	delay_ms(5000);
	out("still alive 5 s after WDT0: cnt %x con %x\n", rd32(WDT_CL0 + 8), rd32(WDT_CL0));
	for (;;)
		__asm__ volatile("wfe");
}

static const char *stage;	/* set at runtime: no absolute pointers in data */

void exc_handler(uint64_t slot, uint64_t *regs)
{
	fb_clear(BG_ERR);
	display_kick();
	out("\nEXCEPTION slot %d at EL%d stage %s mpidr %x\n", (uint32_t)slot, cur_el, stage ? stage : "-",
	    (uint32_t)rsr("mpidr_el1"));
	if (cur_el == 2)
		out("ESR %lx\nELR %lx\nFAR %lx\nSPSR %lx\n", rsr("esr_el2"), rsr("elr_el2"),
		    rsr("far_el2"), rsr("spsr_el2"));
	else
		out("ESR %lx\nELR %lx\nFAR %lx\nSPSR %lx\n", rsr("esr_el1"), rsr("elr_el1"),
		    rsr("far_el1"), rsr("spsr_el1"));
	out("X0 %lx X1 %lx\nX30 %lx\n", regs[0], regs[1], regs[31]);
	reset_after(PHOTO_DELAY_S);
}

/* Dependent-add loop with the I-cache on: ticks of the 24.576 MHz counter. */
static uint64_t speed_ticks(void)
{
	uint64_t sctlr = cur_el == 2 ? rsr("sctlr_el2") : rsr("sctlr_el1");
	if (cur_el == 2)
		wsr("sctlr_el2", sctlr | (1UL << 12));
	else
		wsr("sctlr_el1", sctlr | (1UL << 12));
	__asm__ volatile("isb");
	uint64_t t = now(), x = 0;
	for (uint64_t i = 0; i < 20000000; i++)
		__asm__ volatile("add %0, %0, #1" : "+r"(x));
	t = now() - t;
	if (cur_el == 2)
		wsr("sctlr_el2", sctlr);
	else
		wsr("sctlr_el1", sctlr);
	__asm__ volatile("isb");
	return t;
}

/* ---- per-CPU identification ---- */

struct cpuinfo {
	uint64_t done, el, midr, mpidr, mmfr0, mmfr1, mmfr2, pfr0, isar0, isar1, ticks;
};
static volatile struct cpuinfo cpus[NR_CPUS];

static void fill_cpuinfo(volatile struct cpuinfo *c)
{
	c->el = rsr("CurrentEL") >> 2;
	c->midr = rsr("midr_el1");
	c->mpidr = rsr("mpidr_el1");
	c->mmfr0 = rsr("id_aa64mmfr0_el1");
	c->mmfr1 = rsr("id_aa64mmfr1_el1");
	c->mmfr2 = rsr("id_aa64mmfr2_el1");
	c->pfr0 = rsr("id_aa64pfr0_el1");
	c->isar0 = rsr("id_aa64isar0_el1");
	c->isar1 = rsr("id_aa64isar1_el1");
	c->ticks = speed_ticks();
	__asm__ volatile("dsb sy" ::: "memory");
	c->done = 1;
	__asm__ volatile("dsb sy" ::: "memory");
}

void secondary_main(uint64_t idx)
{
	cur_el = rsr("CurrentEL") >> 2;
	if (idx < NR_CPUS)
		fill_cpuinfo(&cpus[idx]);
	smc(PSCI_CPU_OFF, 0, 0, 0);
}

static const char *core_name(uint64_t midr)
{
	switch ((midr >> 4) & 0xfff) {
	case 0xd05: return "A55";
	case 0xd0b: return "A76";
	case 0xd44: return "X1";
	default: return "?";
	}
}

static void print_cpu(int i, volatile struct cpuinfo *c)
{
	uint64_t m0 = c->mmfr0, m1 = c->mmfr1, i0 = c->isar0, i1 = c->isar1;
	out("CPU%d %s EL%d midr %x mpidr %x ticks %d\n", i, core_name(c->midr), (uint32_t)c->el,
	    (uint32_t)c->midr, (uint32_t)c->mpidr, (uint32_t)c->ticks);
	/* TGran16 [23:20] (1 = 16K), TGran16_2 [35:32], PARange [3:0] */
	out("  tg16=%d tg16_2=%d pa=%d lse=%d rcpc=%d pan=%d vh=%d\n", (uint32_t)(m0 >> 20) & 0xf,
	    (uint32_t)(m0 >> 32) & 0xf, (uint32_t)m0 & 0xf, (uint32_t)(i0 >> 20) & 0xf,
	    (uint32_t)(i1 >> 20) & 0xf, (uint32_t)(m1 >> 20) & 0xf, (uint32_t)(m1 >> 8) & 0xf);
	out("  mmfr0 %lx mmfr1 %lx\n  mmfr2 %lx pfr0 %lx\n", c->mmfr0, c->mmfr1, c->mmfr2, c->pfr0);
}

/* ---- tests ---- */

static void psci_info(void)
{
	stage = "psci";
	out("PSCI version %x SMCCC %x migrate_type %x\n", (uint32_t)smc(PSCI_VERSION, 0, 0, 0),
	    (uint32_t)smc(SMCCC_VERSION, 0, 0, 0), (uint32_t)smc(PSCI_MIGRATE_INFO_TYPE, 0, 0, 0));
	out("PSCI features: on %x off %x susp %x sysoff %x\n",
	    (uint32_t)smc(PSCI_FEATURES, PSCI_CPU_ON, 0, 0),
	    (uint32_t)smc(PSCI_FEATURES, PSCI_CPU_OFF, 0, 0),
	    (uint32_t)smc(PSCI_FEATURES, PSCI_CPU_SUSPEND, 0, 0),
	    (uint32_t)smc(PSCI_FEATURES, PSCI_SYSTEM_OFF, 0, 0));
	out("  reset %x reset2 %x syssusp %x\n",
	    (uint32_t)smc(PSCI_FEATURES, PSCI_SYSTEM_RESET, 0, 0),
	    (uint32_t)smc(PSCI_FEATURES, PSCI_SYSTEM_RESET2, 0, 0),
	    (uint32_t)smc(PSCI_FEATURES, PSCI_SYSTEM_SUSPEND, 0, 0));
}

static void smp_test(void)
{
	stage = "smp";
	uint64_t self = rsr("mpidr_el1") & 0xffffff;
	int64_t ret[NR_CPUS] = {0};

	for (int i = 0; i < NR_CPUS; i++) {
		uint64_t target = (uint64_t)i << 8;	/* DynamIQ: Aff1 = core */
		if (target == self)
			continue;
		ret[i] = (int64_t)smc(PSCI_CPU_ON, target, (uintptr_t)secondary_entry, i);
	}
	delay_ms(1500);
	for (int i = 0; i < NR_CPUS; i++) {
		if (((uint64_t)i << 8) == self)
			continue;
		if (cpus[i].done)
			print_cpu(i, &cpus[i]);
		else
			out("CPU%d no answer, CPU_ON ret %x\n", i, (uint32_t)ret[i]);
	}
}

static void gic_info(void)
{
	stage = "gic";
	out("GICD ctlr %x typer %x iidr %x\n", rd32(GICD_BASE), rd32(GICD_BASE + 4),
	    rd32(GICD_BASE + 8));
	for (int i = 0; i < NR_CPUS; i++) {
		uintptr_t r = GICR_BASE + i * GICR_STRIDE;
		uint64_t typer = rd64(r + 8);
		out("GICR%d aff %x waker %x last %d\n", i, (uint32_t)(typer >> 32), rd32(r + 0x14),
		    (uint32_t)(typer >> 4) & 1);
		if (typer & (1 << 4))
			break;
	}
	if (cur_el == 2)
		out("ICC_SRE_EL2 %x ", (uint32_t)rsr("icc_sre_el2"));
	out("ICC_SRE_EL1 %x\n", (uint32_t)rsr("icc_sre_el1"));
}

/*
 * Exynos watchdogs: the oriole U-Boot port clears WTCON first thing. Stop
 * them before anything else and report the values the bootloader left.
 */
static void wdt_stop(void)
{
	for (int i = 0; i < 2; i++) {
		uintptr_t b = i ? WDT_CL1 : WDT_CL0;
		wdt_regs[i][0] = rd32(b);
		wdt_regs[i][1] = rd32(b + 4);
		wdt_regs[i][2] = rd32(b + 8);
		wr32(b, 0);
	}
}

static void wdt_info(void)
{
	for (int i = 0; i < 2; i++)
		out("WDT%d was con %x dat %x cnt %x, now con %x\n", i, wdt_regs[i][0],
		    wdt_regs[i][1], wdt_regs[i][2], rd32(i ? WDT_CL1 : WDT_CL0));
}

/*
 * Under `fastboot boot` the panel still scans out the fastboot menu buffer.
 * Point DPP0's DMA at 0xfac00000 and latch it, as the oriole U-Boot port
 * does (oriole_take_over_display): RDMA_SRC_SIZE/IMG_SIZE, BASEADDR_Y8,
 * then DECON0 TRIG_CON + SHD_REG_UP_REQ. Pixel format stays as the
 * bootloader set it (RDMA_IN_CTRL_0 bits 13:8, reported).
 */
static uint32_t dpp_base[6], dpp_ctrl[6], dpp_size[6];

static void display_take_over(void)
{
	for (int ch = 0; ch < 6; ch++) {
		dpp_base[ch] = rd32(DPP_DMA(ch) + RDMA_BASEADDR_Y8);
		dpp_ctrl[ch] = rd32(DPP_DMA(ch) + RDMA_IN_CTRL_0);
		dpp_size[ch] = rd32(DPP_DMA(ch) + RDMA_IMG_SIZE);
	}
	uint32_t size = (FB_H << 16) | FB_W;
	wr32(DPP_DMA(0) + RDMA_SRC_SIZE, size);
	wr32(DPP_DMA(0) + RDMA_IMG_SIZE, size);
	wr32(DPP_DMA(0) + RDMA_BASEADDR_Y8, (uint32_t)FB_BASE);
	display_kick();
}

static void display_info(void)
{
	for (int ch = 0; ch < 6; ch++)
		out("DPP%d was base %x ctrl %x size %x\n", ch, dpp_base[ch], dpp_ctrl[ch], dpp_size[ch]);
}

/*
 * The DPP DMA blocks hold the scanout address of each layer. Print every
 * word that looks like a DRAM address, so we learn where the bootloader's
 * framebuffer really is. Done last: an unclocked block may hang the bus.
 */
static void dpp_scan(void)
{
	stage = "dpp";
	static const uint32_t blocks[] = {
		0x1c0b0000, 0x1c0b1000, 0x1c0b2000, 0x1c0b3000, 0x1c0b4000, 0x1c0b5000,
	};
	for (unsigned b = 0; b < sizeof(blocks) / sizeof(blocks[0]); b++) {
		out("DPP %x:", blocks[b]);
		int hits = 0;
		for (uint32_t off = 0; off < 0x1000 && hits < 6; off += 4) {
			uint32_t v = rd32(blocks[b] + off);
			if (v >= 0x80000000U && (v & 0xfff) == 0) {
				out(" +%x=%x", off, v);
				hits++;
			}
		}
		out("\n");
	}
}

void probe_main(uint64_t dtb)
{
	cur_el = rsr("CurrentEL") >> 2;
	wdt_stop();
	display_take_over();
	prz_init();
	fb_clear(BG);
	display_kick();
	stage = "id";
	out("ORIOLE PROBE 3  (ios-topaz, Pixel 6 step 1)\n");
	out("EL%d  image %lx  dtb %lx\n", cur_el, (uint64_t)(uintptr_t)_head, dtb);
	if (dtb && (dtb & 3) == 0) {
		uint32_t magic = __builtin_bswap32(rd32(dtb)), size = __builtin_bswap32(rd32(dtb + 4));
		out("DTB magic %x size %x\n", magic, size);
	}
	out("CNTFRQ %d  CTR %x\n", (uint32_t)rsr("cntfrq_el0"), (uint32_t)rsr("ctr_el0"));
	if (cur_el == 2) {
		out("HCR_EL2 %lx\nSCTLR_EL2 %lx\n", rsr("hcr_el2"), rsr("sctlr_el2"));
		out("CNTHCTL %x CPTR %x MDCR %x\n", (uint32_t)rsr("cnthctl_el2"),
		    (uint32_t)rsr("cptr_el2"), (uint32_t)rsr("mdcr_el2"));
		out("VTTBR %lx CNTVOFF %lx\n", rsr("vttbr_el2"), rsr("cntvoff_el2"));
	}
	out("SCTLR_EL1 %lx\n", rsr("sctlr_el1"));
	fill_cpuinfo(&cpus[0]);
	print_cpu(0, &cpus[0]);
	psci_info();
	smp_test();
	gic_info();
	wdt_info();
	display_info();
	out("photo time: %d s, then DPP scan + reset\n", PHOTO_DELAY_S);
	delay_ms(PHOTO_DELAY_S * 1000);
	dpp_scan();
	stage = "done";
	out("PROBE DONE\n");
	reset_after(5);
}
