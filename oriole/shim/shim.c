/*
 * oriole (Pixel 6, gs101) shim: act as iBoot for xnu.
 *
 * The stock bootloader enters at EL2 with the MMU off and x0 = its DTB
 * (probe 1-3, NOTES.md ORIOLE). build.sh appends the xnu payload
 * (tools/mkpayload.py: kernel segments with chained fixups resolved, ADT,
 * boot-args) to this Image at the first 4K boundary after _end. Here we:
 *   - stop both watchdogs, point DPP0 at the framebuffer, log to the panel
 *     and to the ramoops console zone (board_init)
 *   - copy the kernel segments to phys_base + (vmaddr - virt_base), place the
 *     ADT and boot_args behind the kernel
 *   - prepare the GIC as topaz's shim does (xnu hangs after initCPU without)
 *   - start xnu on CPU1 (MPIDR 0x100): EL2 -> EL1 with x0 = boot_args
 *     (physical). The topaz kernel (0080/0082 SweetCPU) panics unless the
 *     boot CPU is MPIDR 0x100, and 0x100 is a plain A55 here.
 *   - keep CPU0 at EL2 as the panel refresher (command-mode panel: DECON
 *     only sends a frame when kicked; xnu's console just writes memory)
 * The shim stays resident below xnu's RAM (0x80000000, xnu gets
 * 0x8_80000000+): CPU0 keeps running from it, and EL2 traps still land in
 * exc_handler.
 */
#include <stdint.h>
#include "../common/con.h"
#include "../common/gs101.h"
#include "ufs.h"
#include "usb.h"
#include "spi.h"

#define BOOT_LINE_LENGTH	608
#define PAGE_16K		0x4000UL
#define XNU_CPU			1	/* A55, MPIDR 0x100 */
#define VTIMER_INTID		27	/* DT /timer: PPI 11 */
#define PTIMER_INTID		30	/* PPI 14 (non-secure physical) */
#define SGI_INTID		1

#define GICR_TYPER		0x08
#define GICR_WAKER		0x14
#define GICR_SGI		0x10000
#define GICR_IGROUPR0		(GICR_SGI + 0x080)
#define GICR_ISENABLER0		(GICR_SGI + 0x100)
#define GICR_IPRIORITYR		(GICR_SGI + 0x400)

extern char secondary_entry[], _head[], _end[];
void enter_el1(uint64_t entry_pa, uint64_t arg);

static const char *stage;	/* set at runtime: no absolute pointers in data */

void exc_handler(uint64_t slot, uint64_t *regs)
{
	con_screen = 1;
	fb_clear(BG_ERR);
	out("\nSHIM EXCEPTION slot %d at EL%d stage %s mpidr %x\n", (uint32_t)slot,
	    (uint32_t)(rsr("CurrentEL") >> 2), stage ? stage : "-", (uint32_t)rsr("mpidr_el1"));
	/* slots 8-15: from a lower EL, i.e. xnu trapped into EL2 */
	out("ESR_EL2 %lx\nELR_EL2 %lx\nFAR_EL2 %lx\nSPSR_EL2 %lx\nHPFAR %lx\n", rsr("esr_el2"),
	    rsr("elr_el2"), rsr("far_el2"), rsr("spsr_el2"), rsr("hpfar_el2"));
	out("ELR_EL1 %lx ESR_EL1 %lx\nX0 %lx X1 %lx X30 %lx\n", rsr("elr_el1"), rsr("esr_el1"),
	    regs[0], regs[1], regs[31]);
	reset_after(10);
}

/* ---- CPU0: panel refresher + xnu console pump; CPU1: xnu ---- */

static volatile uint64_t refresh_count, xnu_entry, xnu_ba, xnu_cpu_up;

/*
 * xnu's console ring (0047 sweet_log_append: every _cnputs byte goes to
 * sweet_log_ring[n % 4 MiB], sweet_log_total counts bytes). build.sh takes
 * the VAs from the kernel's symbol table (-DXNU_LOG_RING_VA/_TOTAL_VA).
 */
#ifndef XNU_LOG_RING_VA
#define XNU_LOG_RING_VA		0
#define XNU_LOG_TOTAL_VA	0
#endif
#define XNU_LOG_SIZE		(4UL << 20)
#define XNU_IDLE_RESET_S	60	/* no new console output for this long: reset */
#ifndef XNU_MAX_RUN_S
#define XNU_MAX_RUN_S		240	/* reset after this long whatever xnu does (build.sh MAX_RUN_S=) */
#endif

/*
 * CPU0's EL2 stage 1, identity, 4K granule, 39-bit VA, one level-1 table of
 * 1 GiB blocks: below 2 GiB Device-nGnRnE; 2-4 GiB (shim, framebuffer,
 * ramoops) Normal non-cacheable, so the panel's DMA and a reset see every
 * write; xnu's 6 GiB Normal write-back inner shareable, so reads of the
 * console ring are coherent with CPU1's caches. Nothing else is mapped.
 */
static uint64_t l1_table[512] __attribute__((aligned(4096)));

static void el2_mmu_on(void)
{
	for (uint64_t gb = 0; gb < 2; gb++)
		l1_table[gb] = (gb << 30) | (1UL << 10) | (0 << 2) | 1;	/* AF, attr0, block */
	for (uint64_t gb = 2; gb < 4; gb++)
		l1_table[gb] = (gb << 30) | (1UL << 10) | (3UL << 8) | (2 << 2) | 1;	/* attr2 NC */
	for (uint64_t pa = XNU_PHYS_BASE; pa < XNU_PHYS_BASE + XNU_MEM_SIZE; pa += 1UL << 30)
		l1_table[pa >> 30] = pa | (1UL << 10) | (3UL << 8) | (1 << 2) | 1;	/* attr1 WB, ISH */
	__asm__ volatile("dsb sy" ::: "memory");
	wsr("mair_el2", 0x44ff00UL);	/* attr0 Device-nGnRnE, attr1 WB RWA, attr2 NC */
	/* RES1 31/23, PS 40-bit, TG0 4K, walks non-cacheable (the table was written with the D-cache off), T0SZ 25 */
	wsr("tcr_el2", (1UL << 31) | (1UL << 23) | (2UL << 16) | 25);
	wsr("ttbr0_el2", (uintptr_t)l1_table);
	__asm__ volatile("isb; tlbi alle2; dsb sy; isb" ::: "memory");
	wsr("sctlr_el2", rsr("sctlr_el2") | (1UL << 12) | (1UL << 2) | 1);	/* I, C, M */
	__asm__ volatile("isb");
}

static uint64_t pump_done;	/* ring bytes already copied to ramoops */

/* Copy the console bytes xnu has finished writing into ramoops. */
static void pump(uint64_t ring_pa, uint64_t total_pa, uint64_t *seen)
{
	uint64_t total = *(volatile uint64_t *)total_pa;
	/* bytes below the previously seen total are written (add, then store) */
	uint64_t upto = *seen;
	*seen = total;
	if (upto - pump_done > XNU_LOG_SIZE)
		pump_done = upto - XNU_LOG_SIZE;
	for (; pump_done < upto; pump_done++)
		prz_putc(*(volatile char *)(ring_pa + pump_done % XNU_LOG_SIZE));
}

static void __attribute__((noreturn)) refresher(uint64_t ring_pa, uint64_t total_pa)
{
	uint64_t seen = 0, last_total = 0, last_change = now(), f = rsr("cntfrq_el0");
	uint64_t deadline = XNU_MAX_RUN_S ? now() + XNU_MAX_RUN_S * f : ~0ULL;

	for (;;) {
		display_kick();
		refresh_count++;
		delay_ms(33);	/* ~30 Hz */
		if (!ring_pa || (refresh_count & 7))
			continue;
		pump(ring_pa, total_pa, &seen);
		if (seen != last_total) {
			last_total = seen;
			last_change = now();
		} else if (now() - last_change > XNU_IDLE_RESET_S * f) {
			pump(ring_pa, total_pa, &seen);
			pump(ring_pa, total_pa, &seen);
			out("\nSHIM: xnu console quiet for %d s (%lx bytes total), reset\n",
			    XNU_IDLE_RESET_S, seen);
			reset_after(0);
		}
		if (now() > deadline) {
			pump(ring_pa, total_pa, &seen);
			pump(ring_pa, total_pa, &seen);
			out("\nSHIM: %d s run limit (%lx bytes total), reset\n", XNU_MAX_RUN_S, seen);
			reset_after(0);
		}
	}
}

static void gic_cpu_interface(void);

void secondary_main(uint64_t idx)
{
	if (idx != XNU_CPU)
		return;
	wsr("sctlr_el2", rsr("sctlr_el2") | (1UL << 12));
	__asm__ volatile("isb");
	gic_cpu_interface();
	xnu_cpu_up = rsr("mpidr_el1") | (1ULL << 63);
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
	enter_el1(xnu_entry, xnu_ba);
}

/* ---- GIC ---- */

static void gicd_wait_rwp(void)
{
	for (int i = 0; i < 1000000 && (rd32(GICD_BASE) & (1U << 31)); i++)
		;
}

/*
 * What topaz needed before xnu (shim.c test_gic + gold_gicr_prepare):
 * distributor Group 1 enabled with affinity routing, every redistributor
 * awake with its SGIs/PPIs in Group 1 at priority 0xa0 and the timers and
 * SGI1 enabled; the CPU interface of the xnu CPU with PMR 0xf0, Group 1
 * on, EOImode 0 (gic_cpu_interface; ICC_*_EL1 set at EL2 carry over).
 */
static void gic_prepare(void)
{
	uint32_t dctlr = rd32(GICD_BASE);
	wr32(GICD_BASE, dctlr | 0x13);	/* NS view: EnableGrp1, EnableGrp1A, ARE_NS */
	gicd_wait_rwp();
	for (int cpu = 0; cpu < NR_CPUS; cpu++) {
		uintptr_t r = GICR_BASE + cpu * GICR_STRIDE;
		uint32_t waker = rd32(r + GICR_WAKER);
		if (waker & 2) {
			wr32(r + GICR_WAKER, waker & ~2U);
			for (int i = 0; i < 100000 && (rd32(r + GICR_WAKER) & 4); i++)
				;
		}
		wr32(r + GICR_IGROUPR0, 0xffffffff);
		for (int id = 0; id < 32; id++)
			wr8(r + GICR_IPRIORITYR + id, 0xa0);
		wr32(r + GICR_ISENABLER0, (1U << VTIMER_INTID) | (1U << PTIMER_INTID) | (1U << SGI_INTID));
	}
	out("GIC: GICD ctlr %x -> %x, GICR1 en %x\n", dctlr, rd32(GICD_BASE),
	    rd32(GICR_BASE + GICR_STRIDE + GICR_ISENABLER0));
}

/* Per-CPU part, run on the CPU that enters xnu (carries over to EL1). */
static void gic_cpu_interface(void)
{
	wsr("icc_sre_el1", rsr("icc_sre_el1") | 1);
	__asm__ volatile("isb");
	wsr("icc_ctlr_el1", rsr("icc_ctlr_el1") & ~2ULL);	/* EOImode 0 */
	wsr("icc_pmr_el1", 0xf0);
	wsr("icc_bpr1_el1", 0);
	wsr("icc_igrpen1_el1", 1);
	__asm__ volatile("isb");
}

/* ---- loader (shim/loader.c, embedded payload only) ---- */

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

/* pexpert/pexpert/arm64/boot.h, Revision/Version 2 */
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

/* MMU off: everything is Device memory, so copy in aligned 8-byte words. */
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

static int magic_ok(const char *m)
{
	static const char want[] = "SWEETXNU";
	for (int i = 0; i < 8; i++)
		if (m[i] != want[i])
			return 0;
	return 1;
}

static uint64_t xnu_pa_of_va;	/* pa = va + this, for the kernel image */

static const char *load_xnu(uint64_t *entry_pa, uint64_t *ba_out)
{
	uintptr_t pl = ((uintptr_t)_end + 0xfff) & ~0xfffUL;
	if (!magic_ok((const char *)pl))
		return "no SWEETXNU payload behind _end";
	const struct payload *p = (const struct payload *)pl;
	if (p->version != 1 || (p->adt_off & 7))
		return "bad payload header";
	if (p->phys_base != XNU_PHYS_BASE || p->mem_size > XNU_MEM_SIZE)
		return "payload RAM is not the 6 GiB at 0x8_80000000 (mkadt --board oriole?)";

	uint64_t pa_of_va_base = p->phys_base - p->virt_base;	/* pa = va + this */
	xnu_pa_of_va = pa_of_va_base;
	out("payload %lx: %d segs, virt %lx entry %lx\n", (uint64_t)pl, p->nsegs, p->virt_base,
	    p->entry_va);
	out("RAM %lx + %lx\ncmdline: %s\n", p->phys_base, p->mem_size, p->cmdline);

	for (uint32_t i = 0; i < p->nsegs; i++) {
		const struct payload_seg *s = &p->segs[i];
		uintptr_t dst = s->vmaddr + pa_of_va_base;
		copy64(dst, pl + s->file_off, s->file_size);
		if (s->vmsize > s->file_size)
			zero64(dst + ((s->file_size + 7) & ~7UL), s->vmsize - ((s->file_size + 7) & ~7UL));
	}
	out("segments copied, kernel end %lx\n", p->kernel_end + pa_of_va_base);

	/* ADT, then boot_args, behind the kernel; everything below topOfKernelData is ours. */
	uintptr_t adt_pa = p->kernel_end + pa_of_va_base;
	copy64(adt_pa, pl + p->adt_off, p->adt_size);
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
	ba->deviceTreeP = adt_pa - pa_of_va_base;	/* xnu reads it with the MMU on: a VA */
	ba->deviceTreeLength = (uint32_t)p->adt_size;
	for (int i = 0; i < BOOT_LINE_LENGTH; i++)
		ba->CommandLine[i] = p->cmdline[i];
	ba->memSizeActual = p->mem_size;
	out("ADT %lx (%x bytes), boot_args %lx, top %lx\n", (uint64_t)adt_pa, (uint32_t)p->adt_size,
	    (uint64_t)ba_pa, (uint64_t)top);

	*entry_pa = p->entry_va + pa_of_va_base;
	*ba_out = ba_pa;
	return 0;
}

void board_main(uint64_t dtb)
{
	board_init();
	stage = "init";
	out("ORIOLE SHIM  (ios-topaz step 2)  EL%d image %lx dtb %lx\n", cur_el,
	    (uint64_t)(uintptr_t)_head, dtb);
	wdt_info();
	if (cur_el != 2) {
		out("not entered at EL2, stopping\n");
		reset_after(10);
	}
	/* I-cache on for our own loops (MMU stays off, data accesses uncached) */
	wsr("sctlr_el2", rsr("sctlr_el2") | (1UL << 12));
	__asm__ volatile("isb");

#ifdef UFS_PROBE
	stage = "ufs";
	ufs_probe();
#endif
#ifdef USB_PROBE
	stage = "usb";
	usb_probe();
#endif
#ifdef SPI_PROBE
	stage = "spi";
	spi_probe();
#endif
	stage = "load";
	uint64_t entry = 0, ba = 0;
	const char *err = load_xnu(&entry, &ba);
	if (err) {
		out("LOADER: %s\n", err);
		reset_after(10);
	}
	stage = "gic";
	gic_prepare();

	stage = "xnu cpu";
	xnu_entry = entry;
	xnu_ba = ba;
	out("xnu entry %lx (EL1) on CPU%d, boot_args %lx\n", entry, XNU_CPU, ba);
	__asm__ volatile("dsb sy" ::: "memory");
	int64_t r = (int64_t)smc(PSCI_CPU_ON, (uint64_t)XNU_CPU << 8, (uintptr_t)secondary_entry, XNU_CPU);
	delay_ms(100);
	out("CPU_ON %x, xnu CPU mpidr %lx; CPU0 refreshes the panel\n", (uint32_t)r, xnu_cpu_up);
	if (r != 0)
		reset_after(10);
	uint64_t ring_pa = XNU_LOG_RING_VA ? XNU_LOG_RING_VA + xnu_pa_of_va : 0;
	uint64_t total_pa = XNU_LOG_TOTAL_VA ? XNU_LOG_TOTAL_VA + xnu_pa_of_va : 0;
	out("xnu console ring %lx total %lx -> ramoops; reset after %d s of quiet or %d s\n", ring_pa,
	    total_pa, XNU_IDLE_RESET_S, XNU_MAX_RUN_S);
	out("--- xnu console ---\n");
	stage = "refresher";
	con_screen = 0;
	el2_mmu_on();
	refresher(ring_pa, total_pa);
}
