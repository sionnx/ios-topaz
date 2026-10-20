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
 * shows as /sys/fs/pstore/console-ramoops-0 after the WDT0 reset at the end.
 */
#include <stdint.h>
#include "../common/con.h"
#include "../common/gs101.h"

#define PHOTO_DELAY_S	10

extern char secondary_entry[], _head[];

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

void board_main(uint64_t dtb)
{
	board_init();
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
