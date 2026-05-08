/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2013 Regents of the University of California
 */

#include <linux/linkage.h>
#include <asm/asm.h>
#include <asm/asm-extable.h>

	.macro fixup op reg addr lbl
100:
	\op \reg, \addr
	_asm_extable	100b, \lbl
	.endm

/* int memcpy_mc(void *, const void *, size_t) */

/*
 * Load macros with fixup for MC-aware copy: only source (load) operations
 * are protected by exception table entries, since the destination buffer
 * is known-good kernel memory. Store operations are plain.
 */
	.macro LOAD_B reg, offset, ptr
	fixup lb \reg, \offset(\ptr), 6f
	.endm

	.macro STORE_B reg, offset, ptr
	fixup sb \reg, \offset(\ptr), 6f
	.endm

	.macro LOAD_REG reg, offset, ptr
	fixup REG_L \reg, \offset(\ptr), 6f
	.endm

	.macro STORE_REG reg, offset, ptr
	fixup REG_S \reg, \offset(\ptr), 6f
	.endm

	.macro LOAD_W reg, offset, ptr
	fixup lw \reg, \offset(\ptr), 6f
	.endm

	.macro STORE_W reg, offset, ptr
	fixup sw \reg, \offset(\ptr), 6f
	.endm

SYM_FUNC_START(__memcpy_mc)
#include "memcpy_template.S"
	move a0, zero
	ret
6:
	move a0, a2
	ret
SYM_FUNC_END(__memcpy_mc)
SYM_FUNC_ALIAS_WEAK(memcpy_mc, __memcpy_mc)
SYM_FUNC_ALIAS(__pi_memcpy_mc, __memcpy_mc)
SYM_FUNC_ALIAS(__pi___memcpy_mc, __memcpy_mc)
