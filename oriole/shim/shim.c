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
 *   - start the panel refresher on CPU7 (command-mode panel: DECON only
 *     sends a frame when kicked; xnu's console just writes memory)
 *   - drop to EL1 and jump to xnu with x0 = boot_args (physical)
 * The shim stays resident below xnu's RAM (0x80000000, xnu gets
 * 0x8_80000000+): CPU7 keeps running from it, and EL2 traps still land in
 * exc_handler.
 */
#include <stdint.h>
#include "../common/con.h"
#include "../common/gs101.h"

#define BOOT_LINE_LENGTH	608
#define PAGE_16K		0x4000UL
#define REFRESH_CPU		7	/* X1, MPIDR 0x700 */
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

/* ---- panel refresher on CPU7 ---- */

static volatile uint64_t refresh_count;

void secondary_main(uint64_t idx)
{
	if (idx != REFRESH_CPU)
		return;
	for (;;) {
		display_kick();
		refresh_count++;
		delay_ms(33);	/* ~30 Hz */
	}
}

static void start_refresher(void)
{
	int64_t r = (int64_t)smc(PSCI_CPU_ON, (uint64_t)REFRESH_CPU << 8, (uintptr_t)secondary_entry,
				 REFRESH_CPU);
	delay_ms(200);
	out("refresher CPU%d: CPU_ON %x, kicks %d\n", REFRESH_CPU, (uint32_t)r, (uint32_t)refresh_count);
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
 * SGI1 enabled; the CPU interface of the boot CPU with PMR 0xf0, Group 1
 * on, EOImode 0. ICC_*_EL1 set here at EL2 carry over to EL1.
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
	wsr("icc_sre_el1", rsr("icc_sre_el1") | 1);
	__asm__ volatile("isb");
	wsr("icc_ctlr_el1", rsr("icc_ctlr_el1") & ~2ULL);	/* EOImode 0 */
	wsr("icc_pmr_el1", 0xf0);
	wsr("icc_bpr1_el1", 0);
	wsr("icc_igrpen1_el1", 1);
	__asm__ volatile("isb");
	out("GIC: GICD ctlr %x -> %x, GICR0 en %x, ICC sre %x pmr %x\n", dctlr, rd32(GICD_BASE),
	    rd32(GICR_BASE + GICR_ISENABLER0), (uint32_t)rsr("icc_sre_el1"), (uint32_t)rsr("icc_pmr_el1"));
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

	stage = "load";
	uint64_t entry = 0, ba = 0;
	const char *err = load_xnu(&entry, &ba);
	if (err) {
		out("LOADER: %s\n", err);
		reset_after(10);
	}
	stage = "gic";
	gic_prepare();
	stage = "refresher";
	start_refresher();

	stage = "jump";
	out("jump to xnu at %lx (EL1), boot_args %lx\n", entry, ba);
	/* Written with the MMU and caches off; drop any stale I-cache lines too. */
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
	enter_el1(entry, ba);
}
