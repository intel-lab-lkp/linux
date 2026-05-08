/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2013 Regents of the University of California
 */

#include <linux/linkage.h>
#include <asm/asm.h>

/* void *memcpy(void *, const void *, size_t) */

/*
 * Plain load/store macros for kernel memory copy (no fixup tables).
 * These are used by copy_template.S which is #include'd below.
 */
	.macro LOAD_B reg, offset, ptr
	lb \reg, \offset(\ptr)
	.endm

	.macro STORE_B reg, offset, ptr
	sb \reg, \offset(\ptr)
	.endm

	.macro LOAD_REG reg, offset, ptr
	REG_L \reg, \offset(\ptr)
	.endm

	.macro STORE_REG reg, offset, ptr
	REG_S \reg, \offset(\ptr)
	.endm

	.macro LOAD_W reg, offset, ptr
	lw \reg, \offset(\ptr)
	.endm

	.macro STORE_W reg, offset, ptr
	sw \reg, \offset(\ptr)
	.endm

SYM_FUNC_START(__memcpy)
#include "memcpy_template.S"
	ret
SYM_FUNC_END(__memcpy)
SYM_FUNC_ALIAS_WEAK(memcpy, __memcpy)
SYM_FUNC_ALIAS(__pi_memcpy, __memcpy)
SYM_FUNC_ALIAS(__pi___memcpy, __memcpy)
