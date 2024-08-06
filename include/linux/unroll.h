/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2024 Intel Corporation */

#ifndef _LINUX_UNROLL_H
#define _LINUX_UNROLL_H

#ifdef CONFIG_CC_IS_CLANG
#define __pick_unrolled(x, y)	_Pragma(#x)
#elif CONFIG_GCC_VERSION >= 80000
#define __pick_unrolled(x, y)	_Pragma(#y)
#else
#define __pick_unrolled(x, y)	/* not supported */
#endif

/**
 * unrolled - loop attributes to ask the compiler to unroll it
 *
 * Usage:
 *
 * #define BATCH 4
 *	unrolled_count(BATCH)
 *	for (u32 i = 0; i < BATCH; i++)
 *		// loop body without cross-iteration dependencies
 *
 * This is only a hint and the compiler is free to disable unrolling if it
 * thinks the count is suboptimal and may hurt performance and/or hugely
 * increase object code size.
 * Not having any cross-iteration dependencies (i.e. when iter x + 1 depends
 * on what iter x will do with variables) is not a strict requirement, but
 * provides best performance and object code size.
 * Available only on Clang and GCC 8.x onwards.
 */

/* Ask the compiler to pick an optimal unroll count, Clang only */
#define unrolled							    \
	__pick_unrolled(clang loop unroll(enable), /* nothing */)

/* Unroll each @n iterations of a loop */
#define unrolled_count(n)						    \
	__pick_unrolled(clang loop unroll_count(n), GCC unroll n)

/* Unroll the whole loop */
#define unrolled_full							    \
	__pick_unrolled(clang loop unroll(full), GCC unroll 65534)

/* Never unroll a loop */
#define unrolled_none							    \
	__pick_unrolled(clang loop unroll(disable), GCC unroll 1)

#endif /* _LINUX_UNROLL_H */
