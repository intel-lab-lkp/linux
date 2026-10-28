/* SPDX-License-Identifier: GPL-2.0-only */

#include <linux/linkage.h>
#include <asm/asm-extable.h>
#include <asm/assembler.h>

/*
 * Copy memory whose source may contain an uncorrectable error. A synchronous
 * external abort on a source load stops the copy instead of killing the
 * kernel. Faults of any other kind are handled as they are for memcpy().
 *
 * Parameters:
 *	x0 - dest
 *	x1 - src
 *	x2 - n
 * Returns:
 *	x0 - bytes not copied
 */
SYM_FUNC_START(__memcpy_mc)
	cmp	x2, #8
	b.lo	.Lbytes
.Lwords:
1:	ldr	x3, [x1], #8
	str	x3, [x0], #8
	sub	x2, x2, #8
	cmp	x2, #8
	b.hs	.Lwords
.Lbytes:
	cbz	x2, .Ldone
2:	ldrb	w3, [x1], #1
	strb	w3, [x0], #1
	subs	x2, x2, #1
	b.ne	2b
.Ldone:
	mov	x0, #0
	ret

	/* The faulting load did not complete, so x2 still counts its bytes */
.Lfault:
	mov	x0, x2
	ret
SYM_FUNC_END(__memcpy_mc)

	_asm_extable_kaccess_mc	1b, .Lfault
	_asm_extable_kaccess_mc	2b, .Lfault
