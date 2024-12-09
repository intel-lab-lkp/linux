/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2013 ARM Ltd.
 * Copyright (C) 2013 Linaro.
 *
 * This code is based on glibc cortex strings work originally authored by Linaro
 * be found @
 *
 * http://bazaar.launchpad.net/~linaro-toolchain-dev/cortex-strings/trunk/
 * files/head:/src/aarch64/
 */

#include <linux/linkage.h>
#include <asm/assembler.h>
#include <asm/cache.h>
#include <asm/asm-uaccess.h>

/*
 * Copy a buffer from src to dest (alignment handled by the hardware)
 *
 * Parameters:
 *	x0 - dest
 *	x1 - src
 *	x2 - n
 * Returns:
 *	x0 - bytes not copied
 */
	.macro ldrb1 reg, ptr, val
	KERNEL_MEM_ERR(9997f, ldrb  \reg, [\ptr], \val)
	.endm

	.macro strb1 reg, ptr, val
	strb \reg, [\ptr], \val
	.endm

	.macro ldrh1 reg, ptr, val
	KERNEL_MEM_ERR(9997f, ldrh  \reg, [\ptr], \val)
	.endm

	.macro strh1 reg, ptr, val
	strh \reg, [\ptr], \val
	.endm

	.macro ldr1 reg, ptr, val
	KERNEL_MEM_ERR(9997f, ldr \reg, [\ptr], \val)
	.endm

	.macro str1 reg, ptr, val
	str \reg, [\ptr], \val
	.endm

	.macro ldp1 reg1, reg2, ptr, val
	KERNEL_MEM_ERR(9997f, ldp \reg1, \reg2, [\ptr], \val)
	.endm

	.macro stp1 reg1, reg2, ptr, val
	stp \reg1, \reg2, [\ptr], \val
	.endm

end	.req	x5
SYM_FUNC_START(__memcpy_mc_generic)
	add	end, x0, x2
#include "copy_template.S"
	mov	x0, #0				// Nothing to copy
	ret

	// Exception fixups
9997:	sub	x0, end, dst			// bytes not copied
	ret
SYM_FUNC_END(__memcpy_mc_generic)

#ifdef CONFIG_AS_HAS_MOPS
	.arch_extension mops
SYM_FUNC_START(__memcpy_mc)
alternative_if_not ARM64_HAS_MOPS
	b       __memcpy_mc_generic
alternative_else_nop_endif

dstin   .req    x0
src     .req    x1
count   .req    x2
dst     .req    x3

	mov     dst, dstin
	cpyp    [dst]!, [src]!, count!
	cpym    [dst]!, [src]!, count!
	cpye    [dst]!, [src]!, count!

	mov	x0, #0				// Nothing to copy
	ret
SYM_FUNC_END(__memcpy_mc)
#else
SYM_FUNC_ALIAS(__memcpy_mc, __memcpy_mc_generic)
#endif

EXPORT_SYMBOL(__memcpy_mc)
SYM_FUNC_ALIAS_WEAK(memcpy_mc, __memcpy_mc)
EXPORT_SYMBOL(memcpy_mc)
