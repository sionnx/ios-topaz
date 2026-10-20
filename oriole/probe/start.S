/*
 * oriole (Pixel 6, gs101) probe entry: arm64 Linux Image header so the
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
	bl	probe_main
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
