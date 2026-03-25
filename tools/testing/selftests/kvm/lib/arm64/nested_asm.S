/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ARM64 Nested virtualization assembly helpers
 */

.globl run_l2
.globl after_hvc
.globl do_hvc
run_l2:
	/*
	 * At this point TPIDR_EL2 will contain the gpa of do_hvc from
	 * prepare_eret_destination(). gpa of do_hvc have to be passed in
	 * because we want L2 to issue an hvc after it returns from the user
	 * passed function. In order for that to happen the lr must be
	 * controlled, which at this point holds the value of the address of
	 * the next instruction after this run_l2() call, which is not useful
	 * for L2. Additionally, L1 can't translate gva into gpa, so we can't
	 * calculate it here.
	 *
	 * So first save lr, then move TPIDR_EL2 to lr so when the user supplied
	 * L2 function returns, L2 jumps to do_hvc and let the L1 hvc handler
	 * take control. This implies we expect the L2 code to preserve lr and
	 * calls a regular ret in the end, which is true for normal C functions.
	 * The hvc handler will jump back to after_hvc when finished, and lr
	 * will be restored and we can return run_l2().
	 */
	stp	x29, lr, [sp, #-16]!
	mrs	x0, tpidr_el2
	mov	lr, x0
	eret
after_hvc:
	ldp	x29, lr, [sp], #16
	ret
do_hvc:
	hvc #0
