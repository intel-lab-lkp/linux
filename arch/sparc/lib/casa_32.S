/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Compare-and-swap for the 32-bit sparc implementations that have it.
 */

#include <linux/linkage.h>
#include <asm/errno.h>

	.text
	.align	4

/* int __sparc32_casa(u32 *addr, u32 oldval, u32 newval, u32 *prev)
 * Returns 0 and *prev, or -EFAULT.  ASI 0x0b is supervisor data.
 */
ENTRY(__sparc32_casa)
	/* nop + .balignl: LEON errata GRLIB-TN-0010 and TN-0011. */
	nop
	.balignl 16, 0x01000000
1:	casa	[%o0] 0x0b, %o1, %o2
	st	%o2, [%o3]
	retl
	 clr	%o0
ENDPROC(__sparc32_casa)

	.section .fixup,#alloc,#execinstr
	.align	4
2:	retl
	 mov	-EFAULT, %o0
	.previous

	.section __ex_table,#alloc
	.align	4
	.word	1b, 2b
	.previous
