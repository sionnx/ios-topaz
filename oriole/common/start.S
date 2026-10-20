/*
 * oriole (Pixel 6, gs101) probe and shim entry: arm64 Linux Image header so the
 * Pixel bootloader treats us like a kernel. Linux reports "All CPU(s)
 * started at EL2" on this phone, so expect EL2 with MMU/D-cache off and
 * x0 = DTB pointer; EL1 is handled too.
 */
	.section .text.head, "ax"
	.globl _head
_head:
	b	_start			/* code0 */
	.long	0			/* code1 */
	.quad	0			/* text_offset */
	.quad	_end			/* image_size (linked at 0) */
	.quad	0xa			/* flags: LE, 4K pages, place anywhere */
	.quad	0			/* res2 */
	.quad	0			/* res3 */
	.quad	0			/* res4 */
	.ascii	"ARM\x64"		/* magic */
	.long	0			/* res5 (PE offset) */

	.text
	.globl _start
_start:
	msr	daifset, #0xf
	mov	x19, x0			/* DTB pointer */
	msr	spsel, #1
	adr	x1, _stack_top
	mov	sp, x1
	bl	set_vbar
	/* zero .bss */
	adr	x1, _bss_start
	adr	x2, _bss_end
1:	cmp	x1, x2
	b.hs	2f
	str	xzr, [x1], #8
	b	1b
2:	mov	x0, x19
	bl	board_main
3:	wfe
	b	3b

/* PSCI CPU_ON target. x0 = context id = cpu index (1..7). */
	.globl secondary_entry
secondary_entry:
	msr	daifset, #0xf
	mov	x19, x0
	msr	spsel, #1
	adr	x1, _sec_stacks
	add	x1, x1, x0, lsl #12
	add	x1, x1, #0x1000
	mov	sp, x1
	bl	set_vbar
	mov	x0, x19
	bl	secondary_main
4:	wfe
	b	4b

/*
 * enter_el1(x0 = entry PA, x1 = argument): leave EL2 for EL1h with the MMU
 * and caches off, DAIF masked and x0 = argument, as iBoot enters xnu. EL1
 * gets AArch64 and no traps: HCR_EL2 = RW only (IMO/FMO clear, so the GIC
 * CPU interface is the physical one), timers and FP/SIMD untrapped, GIC
 * system registers allowed. VBAR_EL2 stays on our vectors, so anything
 * that still lands in EL2 is reported (exc_handler) instead of lost.
 */
	.globl enter_el1
enter_el1:
	mov	x2, #(1 << 31)		/* HCR_EL2.RW */
	msr	hcr_el2, x2
	mov	x2, #3			/* CNTHCTL_EL2: EL1PCTEN | EL1PCEN */
	msr	cnthctl_el2, x2
	msr	cntvoff_el2, xzr
	mov	x2, #0x33ff		/* CPTR_EL2: RES1 bits, TFP clear */
	msr	cptr_el2, x2
	msr	hstr_el2, xzr
	msr	vttbr_el2, xzr
	msr	ich_hcr_el2, xzr
	mrs	x2, midr_el1
	msr	vpidr_el2, x2
	mrs	x2, mpidr_el1
	msr	vmpidr_el2, x2
	mrs	x2, icc_sre_el2
	orr	x2, x2, #1		/* SRE */
	orr	x2, x2, #8		/* Enable: EL1 may use ICC_SRE_EL1 */
	msr	icc_sre_el2, x2
	isb
	mov	x2, #0x0800		/* SCTLR_EL1 RES1: 29, 28, 23, 22, 20, 11 */
	movk	x2, #0x30d0, lsl #16
	msr	sctlr_el1, x2
	mov	x2, #0x3c5		/* SPSR_EL2: EL1h, DAIF masked */
	msr	spsr_el2, x2
	msr	elr_el2, x0
	mov	x0, x1
	isb
	eret

/* VBAR of whichever EL we run at; clobbers x1, x2. */
set_vbar:
	adr	x1, vectors
	mrs	x2, CurrentEL
	cmp	x2, #(2 << 2)
	b.ne	5f
	msr	vbar_el2, x1
	isb
	ret
5:	msr	vbar_el1, x1
	isb
	ret

/*
 * Exception vectors: every slot ends in exc_handler(slot, regs), which
 * reports and resets. Same layout at EL1 and EL2.
 */
.macro vent slot
	.balign	0x80
	stp	x29, x30, [sp, #-16]!
	mov	x29, #\slot
	b	exc_common
.endm

	.balign	0x800
	.globl vectors
vectors:
	vent 0
	vent 1
	vent 2
	vent 3
	vent 4
	vent 5
	vent 6
	vent 7
	vent 8
	vent 9
	vent 10
	vent 11
	vent 12
	vent 13
	vent 14
	vent 15

exc_common:
	sub	sp, sp, #(30 * 8)
	stp	x0, x1, [sp, #0]
	stp	x2, x3, [sp, #16]
	mov	x0, x29
	mov	x1, sp
	bl	exc_handler
5:	wfe
	b	5b
