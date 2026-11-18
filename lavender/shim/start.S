/*
 * lavender probe shim entry. Qualcomm ABL treats us as an arm64 Linux
 * Image: enters at EL1, MMU and D-cache off, x0 = DTB pointer.
 * Everything is PC-relative: the image runs wherever ABL loads it.
 */
	.section .text.head, "ax"
	.globl _head
_head:
	b	_start			/* code0 */
	.long	0			/* code1 */
	.quad	0x80000			/* text_offset */
	.quad	_end			/* image_size (linked at 0) */
	.quad	0xa			/* flags: LE, 4K pages */
	.quad	0, 0, 0			/* res2..res4 */
	.ascii	"ARM\x64"		/* magic */
	.long	0			/* res5 */

	.text
	.globl _start
_start:
	msr	daifset, #0xf
	mov	x19, x0			/* DTB from ABL */
	msr	spsel, #1
	adr	x1, _stack_top
	mov	sp, x1
	adr	x1, vectors
	msr	vbar_el1, x1
	isb
	adr	x1, _bss_start		/* zero .bss */
	adr	x2, _bss_end
1:	cmp	x1, x2
	b.hs	2f
	str	xzr, [x1], #8
	b	1b
2:	mov	x0, x19
	adr	x1, _head
	bl	shim_main
3:	wfe
	b	3b

/* PSCI CPU_ON target, x0 = context id = cpu slot (0..7) */
	.globl secondary_entry
secondary_entry:
	msr	daifset, #0xf
	msr	spsel, #1
	adr	x1, _sec_stacks
	add	x1, x1, x0, lsl #12
	add	x1, x1, #0x1000
	mov	sp, x1
	adr	x1, vectors
	msr	vbar_el1, x1
	isb
	bl	secondary_main
4:	wfe
	b	4b

/* Every exception: show slot + ESR/ELR/FAR on screen, then stop. */
.macro vent slot
	.balign	0x80
	mov	x0, #\slot
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
	mrs	x1, esr_el1
	mrs	x2, elr_el1
	mrs	x3, far_el1
	bl	exc_handler
5:	wfe
	b	5b
