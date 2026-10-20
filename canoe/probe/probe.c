/*
 * canoe (OnePlus 15, CPH2747, SM8850 "canoe") bring-up probe, step 1.
 *
 * One `fastboot boot` of this image answers what deciding a port to this
 * phone depends on, without writing any partition:
 *   - does the (really-unlocked, props-spoofed) bootloader accept and run an
 *     unsigned `fastboot boot` image at all (if not, nothing reaches ramoops)
 *   - entry exception level (Qualcomm + Gunyah: expect EL1, unlike Pixel 6)
 *   - **ID_AA64MMFR0_EL1 TGran16**: whether the Oryon cores have the 16K
 *     translation granule the iOS userland needs (Linux hides this from EL0)
 *   - LSE, RCpc, PAN, PARange on each of the 8 cores (via PSCI CPU_ON)
 *   - PSCI/SMCCC version and which reset calls work (for auto-reset later)
 *   - GICv3 distributor/redistributor state
 * Output goes to the bootloader splash framebuffer (0xfc800000) and to the
 * ramoops console zone (0xbffdbf000), which Android shows as
 * /sys/fs/pstore/console-ramoops-0 after a reset. The framebuffer is a
 * bonus; every answer is in ramoops, so filming the screen is not required.
 */
#include <stdarg.h>
#include <stdint.h>
#include "../../shim/font.h"

/* reserved-memory splash_region "cont_splash_region", left lit by UEFI/ABL */
#define FB_BASE		0xfc800000UL
#define FB_W		1272
#define FB_H		2772
#define FB_STRIDE	(FB_W * 4)	/* a8r8g8b8 guess; the panel is 1272x2772 */

/* reserved-memory ramoops-region; record_size 0 so the console zone is first */
#define RAMOOPS_BASE	0xbffdbf000UL
#define RAMOOPS_CONSOLE	0x40000UL	/* console-size (256 KiB) */
#define PRZ_SIG		0x43474244U	/* "DBGC": persistent_ram sig */

/* interrupt-controller@17000000: GICD + GICR (stride 0x40000) */
#define GICD_BASE	0x17000000UL
#define GICR_BASE	0x17080000UL
#define GICR_STRIDE	0x40000UL

/* qcom,msm-imem@14680000 restart_reason@65c, for a bootloader reboot */
#define IMEM_RESTART	0x1468065cUL
#define RESTART_BOOTLOADER	0x77665500U

#define PSCI_VERSION		0x84000000UL
#define PSCI_CPU_OFF		0x84000002UL
#define PSCI_CPU_ON		0xc4000003UL
#define PSCI_MIGRATE_INFO_TYPE	0x84000006UL
#define PSCI_SYSTEM_OFF		0x84000008UL
#define PSCI_SYSTEM_RESET	0x84000009UL
#define PSCI_FEATURES		0x8400000aUL
#define PSCI_SYSTEM_RESET2	0xc4000012UL
#define SMCCC_VERSION		0x80000000UL

/* MPIDRs from /cpus (Aff2 set on the two prime cores) */
#define NR_CPUS		8
static const uint32_t cpu_mpidr[NR_CPUS] = {
	0x0, 0x100, 0x200, 0x300, 0x400, 0x500, 0x10000, 0x10100,
};

#define PHOTO_DELAY_S	40

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define wsr(r, v) __asm__ volatile("msr " r ", %0" : : "r"((uint64_t)(v)))
static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline uint64_t rd64(uintptr_t a) { return *(volatile uint64_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

extern char secondary_entry[], _head[];
static unsigned cur_el;

/* ---- framebuffer text + ramoops console ---- */
#define SCALE	2
#define CH_W	(8 * SCALE)
#define CH_H	(16 * SCALE)
#define MARGIN	24
#define COLS	((FB_W - 2 * MARGIN) / CH_W)
#define ROWS	((FB_H - 2 * MARGIN) / CH_H)
#define FG	0xffffffffU
#define BG	0xff202060U
#define BG_ERR	0xff800000U

static uint32_t con_bg = BG;
static int con_row, con_col;
static uint32_t prz_start, prz_size;

static void fb_clear(uint32_t bg)
{
	volatile uint32_t *fb = (volatile uint32_t *)FB_BASE;
	for (unsigned i = 0; i < FB_W * FB_H; i++)
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
		fb_clear(con_bg);
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

static void prz_init(void)
{
	wr32(RAMOOPS_BASE + 0, PRZ_SIG);
	wr32(RAMOOPS_BASE + 4, 0);
	wr32(RAMOOPS_BASE + 8, 0);
	prz_start = prz_size = 0;
}

static void out_char(char ch)
{
	fb_char(ch);
	if (prz_size < RAMOOPS_CONSOLE - 12) {
		*(volatile uint8_t *)(RAMOOPS_BASE + 12 + prz_start) = (uint8_t)ch;
		prz_start++;
		prz_size++;
		wr32(RAMOOPS_BASE + 8, prz_size);
		wr32(RAMOOPS_BASE + 4, prz_start);
	}
}

static void out_hex(uint64_t v, int digits)
{
	for (int i = digits - 1; i >= 0; i--)
		out_char("0123456789abcdef"[(v >> (4 * i)) & 0xf]);
}

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
		} else if (*p) {
			out_char('%');
			out_char(*p);
		}
	}
	va_end(ap);
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

static const char *stage;

static void __attribute__((noreturn)) finish(void)
{
	out("\nPROBE DONE; reset in %d s, then try PSCI/imem reboot\n", PHOTO_DELAY_S);
	delay_ms(PHOTO_DELAY_S * 1000);
	__asm__ volatile("dsb sy" ::: "memory");
	smc(PSCI_SYSTEM_RESET, 0, 0, 0);
	out("PSCI reset returned; imem restart_reason + spin\n");
	wr32(IMEM_RESTART, RESTART_BOOTLOADER);
	__asm__ volatile("dsb sy" ::: "memory");
	for (;;)
		__asm__ volatile("wfi");
}

void exc_handler(uint64_t slot, uint64_t *regs)
{
	fb_clear(BG_ERR);
	out("\nEXCEPTION slot %d at EL%d stage %s mpidr %x\n", (uint32_t)slot, cur_el,
	    stage ? stage : "-", (uint32_t)rsr("mpidr_el1"));
	if (cur_el == 2)
		out("ESR %lx ELR %lx\nFAR %lx SPSR %lx\n", rsr("esr_el2"), rsr("elr_el2"),
		    rsr("far_el2"), rsr("spsr_el2"));
	else
		out("ESR %lx ELR %lx\nFAR %lx SPSR %lx\n", rsr("esr_el1"), rsr("elr_el1"),
		    rsr("far_el1"), rsr("spsr_el1"));
	out("X0 %lx X1 %lx X30 %lx\n", regs[0], regs[1], regs[31]);
	finish();
}

/* ---- per-CPU ID via PSCI CPU_ON ---- */
struct cpuinfo {
	uint64_t done, el, midr, mpidr, mmfr0, mmfr1, isar0, isar1, pfr0;
};
static volatile struct cpuinfo cpus[NR_CPUS];

static void fill(volatile struct cpuinfo *c)
{
	c->el = rsr("CurrentEL") >> 2;
	c->midr = rsr("midr_el1");
	c->mpidr = rsr("mpidr_el1");
	c->mmfr0 = rsr("id_aa64mmfr0_el1");
	c->mmfr1 = rsr("id_aa64mmfr1_el1");
	c->isar0 = rsr("id_aa64isar0_el1");
	c->isar1 = rsr("id_aa64isar1_el1");
	c->pfr0 = rsr("id_aa64pfr0_el1");
	__asm__ volatile("dsb sy" ::: "memory");
	c->done = 1;
	__asm__ volatile("dsb sy" ::: "memory");
}

void secondary_main(uint64_t idx)
{
	if (idx < NR_CPUS)
		fill(&cpus[idx]);
	smc(PSCI_CPU_OFF, 0, 0, 0);
}

static void print_cpu(int i, volatile struct cpuinfo *c)
{
	uint64_t m0 = c->mmfr0, i0 = c->isar0, i1 = c->isar1, m1 = c->mmfr1;
	out("CPU%d EL%d midr %x mpidr %x\n", i, (uint32_t)c->el, (uint32_t)c->midr,
	    (uint32_t)c->mpidr);
	/* TGran16 [23:20] (>=1 means 16K supported), TGran16_2 [35:32], PARange [3:0] */
	out("  TGran16=%x TGran16_2=%x PA=%x lse=%x rcpc=%x pan=%x\n", (uint32_t)((m0 >> 20) & 0xf),
	    (uint32_t)((m0 >> 32) & 0xf), (uint32_t)(m0 & 0xf), (uint32_t)((i0 >> 20) & 0xf),
	    (uint32_t)((i1 >> 20) & 0xf), (uint32_t)((m1 >> 20) & 0xf));
	out("  mmfr0 %lx isar1 %lx pfr0 %lx\n", c->mmfr0, c->isar1, c->pfr0);
}

static void smp(void)
{
	stage = "smp";
	uint64_t self = rsr("mpidr_el1") & 0xffffff;
	int64_t ret[NR_CPUS] = { 0 };
	for (int i = 0; i < NR_CPUS; i++) {
		if (cpu_mpidr[i] == self)
			continue;
		ret[i] = (int64_t)smc(PSCI_CPU_ON, cpu_mpidr[i], (uintptr_t)secondary_entry, i);
	}
	delay_ms(1500);
	for (int i = 0; i < NR_CPUS; i++) {
		if (cpu_mpidr[i] == self) {
			fill(&cpus[i]);
			print_cpu(i, &cpus[i]);
		} else if (cpus[i].done)
			print_cpu(i, &cpus[i]);
		else
			out("CPU%d (mpidr %x) no answer, CPU_ON ret %x\n", i, cpu_mpidr[i],
			    (uint32_t)ret[i]);
	}
}

static void psci_info(void)
{
	stage = "psci";
	out("PSCI version %x SMCCC %x migrate_type %x\n", (uint32_t)smc(PSCI_VERSION, 0, 0, 0),
	    (uint32_t)smc(SMCCC_VERSION, 0, 0, 0), (uint32_t)smc(PSCI_MIGRATE_INFO_TYPE, 0, 0, 0));
	out("PSCI features: on %x off %x reset %x reset2 %x sysoff %x\n",
	    (uint32_t)smc(PSCI_FEATURES, PSCI_CPU_ON, 0, 0),
	    (uint32_t)smc(PSCI_FEATURES, PSCI_CPU_OFF, 0, 0),
	    (uint32_t)smc(PSCI_FEATURES, PSCI_SYSTEM_RESET, 0, 0),
	    (uint32_t)smc(PSCI_FEATURES, PSCI_SYSTEM_RESET2, 0, 0),
	    (uint32_t)smc(PSCI_FEATURES, PSCI_SYSTEM_OFF, 0, 0));
}

static void gic_info(void)
{
	stage = "gic";
	out("GICD ctlr %x typer %x iidr %x\n", rd32(GICD_BASE), rd32(GICD_BASE + 4),
	    rd32(GICD_BASE + 8));
	for (int i = 0; i < NR_CPUS; i++) {
		uintptr_t r = GICR_BASE + (uintptr_t)i * GICR_STRIDE;
		uint64_t typer = rd64(r + 8);
		out("GICR%d aff %x waker %x last %d\n", i, (uint32_t)(typer >> 32), rd32(r + 0x14),
		    (uint32_t)((typer >> 4) & 1));
		if (typer & (1 << 4))
			break;
	}
	if (cur_el == 2)
		out("ICC_SRE_EL2 %x ", (uint32_t)rsr("icc_sre_el2"));
	out("ICC_SRE_EL1 %x\n", (uint32_t)rsr("icc_sre_el1"));
}

void board_main(uint64_t dtb)
{
	cur_el = rsr("CurrentEL") >> 2;
	prz_init();
	fb_clear(BG);
	stage = "id";
	out("CANOE PROBE 1  (ios-topaz, OnePlus 15 / SM8850)\n");
	out("EL%d  image %lx  dtb %lx\n", cur_el, (uint64_t)(uintptr_t)_head, dtb);
	if (dtb && (dtb & 3) == 0)
		out("DTB magic %x size %x\n", __builtin_bswap32(rd32(dtb)),
		    __builtin_bswap32(rd32(dtb + 4)));
	out("CNTFRQ %d CTR %x\n", (uint32_t)rsr("cntfrq_el0"), (uint32_t)rsr("ctr_el0"));
	if (cur_el == 2)
		out("HCR_EL2 %lx SCTLR_EL2 %lx\n", rsr("hcr_el2"), rsr("sctlr_el2"));
	out("SCTLR_EL1 %lx TCR_EL1 %lx\n", rsr("sctlr_el1"), rsr("tcr_el1"));
	fill(&cpus[0]);
	print_cpu(0, &cpus[0]);
	psci_info();
	smp();
	gic_info();
	finish();
}
