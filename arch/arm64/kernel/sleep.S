/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/errno.h>
#include <linux/linkage.h>
#include <asm/asm-offsets.h>
#include <asm/assembler.h>
#include <asm/smp.h>

	.text
/*
 * Compute the address of the current CPU's entry in sleep_save_stash,
 * i.e. &sleep_save_stash[hash(MPIDR_EL1)], where the hash is an
 * implementation of the MPIDR_EL1 hash algorithm through shifting
 * and OR'ing.
 *
 * Pseudo C-code:
 *
 *	u64 mpidr = MPIDR_EL1 & mpidr_hash.mask;
 *	u32 hash = ((mpidr & 0xff) >> mpidr_hash.shift_aff[0]) |
 *		   ((mpidr & 0xff00) >> mpidr_hash.shift_aff[1]) |
 *		   ((mpidr & 0xff0000) >> mpidr_hash.shift_aff[2]) |
 *		   ((mpidr & 0xff00000000) >> mpidr_hash.shift_aff[3]);
 *	slot = &sleep_save_stash[hash];
 *
 * @slot: output register
 *
 * Clobbers: x2 - x8
 */
	.macro get_sleep_stash_slot slot
	mrs	x3, mpidr_el1
	adr_l	x2, mpidr_hash
	ldr	x8, [x2, #MPIDR_HASH_MASK]
	/*
	 * Following code relies on the struct mpidr_hash
	 * members size.
	 */
	ldp	w4, w5, [x2, #MPIDR_HASH_SHIFTS]
	ldp	w6, w7, [x2, #(MPIDR_HASH_SHIFTS + 8)]
	and	x3, x3, x8			// mask out MPIDR bits
	and	x2, x3, #0xff			// aff0
	lsr	x2, x2, x4			// hash = aff0 >> shift_aff[0]
	and	x8, x3, #0xff00			// aff1
	lsr	x8, x8, x5
	orr	x2, x2, x8			// hash |= aff1 >> shift_aff[1]
	and	x8, x3, #0xff0000		// aff2
	lsr	x8, x8, x6
	orr	x2, x2, x8			// hash |= aff2 >> shift_aff[2]
	and	x8, x3, #0xff00000000		// aff3
	lsr	x8, x8, x7
	orr	x2, x2, x8			// hash |= aff3 >> shift_aff[3]
	ldr_l	\slot, sleep_save_stash
	add	\slot, \slot, x2, lsl #3	// slot = &sleep_save_stash[hash]
	.endm
/*
 * Save CPU state in the provided sleep_stack_data area, and publish its
 * location for cpu_resume()'s use in sleep_save_stash.
 *
 * cpu_resume() will restore this saved state, and return. Because the
 * link-register is saved and restored, it will appear to return from this
 * function. So that the caller can tell the suspend/resume paths apart,
 * __cpu_suspend_enter() will always return a non-zero value, whereas the
 * path through cpu_resume() will return 0.
 *
 *  x0 = struct sleep_stack_data area
 */
SYM_FUNC_START(__cpu_suspend_enter)
	stp	x29, lr, [x0, #SLEEP_STACK_DATA_CALLEE_REGS]
	stp	x19, x20, [x0,#SLEEP_STACK_DATA_CALLEE_REGS+16]
	stp	x21, x22, [x0,#SLEEP_STACK_DATA_CALLEE_REGS+32]
	stp	x23, x24, [x0,#SLEEP_STACK_DATA_CALLEE_REGS+48]
	stp	x25, x26, [x0,#SLEEP_STACK_DATA_CALLEE_REGS+64]
	stp	x27, x28, [x0,#SLEEP_STACK_DATA_CALLEE_REGS+80]

	/* save the sp in cpu_suspend_ctx */
	mov	x2, sp
	str	x2, [x0, #SLEEP_STACK_DATA_SYSTEM_REGS + CPU_CTX_SP]

	/* publish this CPU's sleep_stack_data area in its stash slot */
	get_sleep_stash_slot x1
	str	x0, [x1]
	add	x0, x0, #SLEEP_STACK_DATA_SYSTEM_REGS
	stp	x29, lr, [sp, #-16]!
	bl	cpu_do_suspend
	ldp	x29, lr, [sp], #16
	mov	x0, #1
	ret
SYM_FUNC_END(__cpu_suspend_enter)

	.pushsection ".idmap.text", "a"
SYM_CODE_START(cpu_resume)
	mov	x0, xzr
	bl	init_kernel_el
	mov	x19, x0			// preserve boot mode
	bl	__cpu_setup
	/* enable the MMU early - so we can access sleep_save_stash by va */
	adrp	x1, swapper_pg_dir
	adrp	x2, idmap_pg_dir
	bl	__enable_mmu
	ldr	x8, =_cpu_resume
	br	x8
SYM_CODE_END(cpu_resume)
	.ltorg
	.popsection

SYM_FUNC_START(_cpu_resume)
	mov	x0, x19
	bl	finalise_el2

	/* retrieve this CPU's context pointer from its stash slot */
	get_sleep_stash_slot x1
	ldr	x0, [x1]
	add	x29, x0, #SLEEP_STACK_DATA_CALLEE_REGS
	add	x0, x0, #SLEEP_STACK_DATA_SYSTEM_REGS
	/* load sp from context */
	ldr	x2, [x0, #CPU_CTX_SP]
	mov	sp, x2
	/*
	 * cpu_do_resume expects x0 to contain context address pointer
	 */
	bl	cpu_do_resume

#if defined(CONFIG_KASAN) && defined(CONFIG_KASAN_STACK)
	mov	x0, sp
	bl	kasan_unpoison_task_stack_below
#endif

	ldp	x19, x20, [x29, #16]
	ldp	x21, x22, [x29, #32]
	ldp	x23, x24, [x29, #48]
	ldp	x25, x26, [x29, #64]
	ldp	x27, x28, [x29, #80]
	ldp	x29, lr, [x29]
	mov	x0, #0
	ret
SYM_FUNC_END(_cpu_resume)
