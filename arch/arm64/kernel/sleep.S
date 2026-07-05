/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/errno.h>
#include <linux/linkage.h>
#include <asm/asm-offsets.h>
#include <asm/assembler.h>
#include <asm/smp.h>

	.text
/*
 * Implementation of MPIDR_EL1 hash algorithm through shifting
 * and OR'ing.
 *
 * @dst: register containing hash result
 * @rs0: register containing affinity level 0 bit shift
 * @rs1: register containing affinity level 1 bit shift
 * @rs2: register containing affinity level 2 bit shift
 * @rs3: register containing affinity level 3 bit shift
 * @mpidr: register containing MPIDR_EL1 value
 * @mask: register containing MPIDR mask
 *
 * Pseudo C-code:
 *
 *u32 dst;
 *
 *compute_mpidr_hash(u32 rs0, u32 rs1, u32 rs2, u32 rs3, u64 mpidr, u64 mask) {
 *	u32 aff0, aff1, aff2, aff3;
 *	u64 mpidr_masked = mpidr & mask;
 *	aff0 = mpidr_masked & 0xff;
 *	aff1 = mpidr_masked & 0xff00;
 *	aff2 = mpidr_masked & 0xff0000;
 *	aff3 = mpidr_masked & 0xff00000000;
 *	dst = (aff0 >> rs0 | aff1 >> rs1 | aff2 >> rs2 | aff3 >> rs3);
 *}
 * Input registers: rs0, rs1, rs2, rs3, mpidr, mask
 * Output register: dst
 * Note: input and output registers must be disjoint register sets
         (eg: a macro instance with mpidr = x1 and dst = x1 is invalid)
         This is enforced at build time by the assertions below.
 */
	.macro mpidr_hash_assert_distinct reg, regs:vararg
	.irp	r, \regs
	.ifc	\reg, \r
	.error	"compute_mpidr_hash: register arguments must be distinct"
	.endif
	.endr
	.endm

	.macro compute_mpidr_hash dst, rs0, rs1, rs2, rs3, mpidr, mask
	/* \dst is written before any input is consumed */
	mpidr_hash_assert_distinct \dst, \rs0, \rs1, \rs2, \rs3, \mpidr, \mask
	/* \mpidr and \mask are clobbered while other inputs are still live */
	mpidr_hash_assert_distinct \mpidr, \rs0, \rs1, \rs2, \rs3, \mask
	mpidr_hash_assert_distinct \mask, \rs1, \rs2, \rs3
	and	\mpidr, \mpidr, \mask		// mask out MPIDR bits
	and	\dst, \mpidr, #0xff		// mask=aff0
	lsr	\dst ,\dst, \rs0		// dst=aff0>>rs0
	and	\mask, \mpidr, #0xff00		// mask = aff1
	lsr	\mask ,\mask, \rs1
	orr	\dst, \dst, \mask		// dst|=(aff1>>rs1)
	and	\mask, \mpidr, #0xff0000	// mask = aff2
	lsr	\mask ,\mask, \rs2
	orr	\dst, \dst, \mask		// dst|=(aff2>>rs2)
	and	\mask, \mpidr, #0xff00000000	// mask = aff3
	lsr	\mask ,\mask, \rs3
	orr	\dst, \dst, \mask		// dst|=(aff3>>rs3)
	.endm

/*
 * Compute the address of the current CPU's entry in sleep_save_stash,
 * i.e. &sleep_save_stash[hash(MPIDR_EL1)].
 *
 * @slot: output register
 *
 * Clobbers: x2 - x8
 */
	.macro get_sleep_stash_slot slot
	mpidr_hash_assert_distinct \slot, x2
	mrs	x3, mpidr_el1
	adr_l	x2, mpidr_hash
	ldr	x8, [x2, #MPIDR_HASH_MASK]
	/*
	 * Following code relies on the struct mpidr_hash
	 * members size.
	 */
	ldp	w4, w5, [x2, #MPIDR_HASH_SHIFTS]
	ldp	w6, w7, [x2, #(MPIDR_HASH_SHIFTS + 8)]
	compute_mpidr_hash x2, x4, x5, x6, x7, x3, x8
	ldr_l	\slot, sleep_save_stash
	add	\slot, \slot, x2, lsl #3
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

	/* find this CPU's slot in sleep_save_stash */
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

	/* grab this CPU's context pointer from sleep_save_stash */
	get_sleep_stash_slot x0
	ldr	x0, [x0]
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
