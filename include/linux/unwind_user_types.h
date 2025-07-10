/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_UNWIND_USER_TYPES_H
#define _LINUX_UNWIND_USER_TYPES_H

#include <linux/types.h>
#include <asm/unwind_user_types.h>

#ifndef arch_unwind_user_state
struct arch_unwind_user_state {};
#endif


/*
 * Unwind types, listed in priority order: lower numbers are attempted first if
 * available.
 */
enum unwind_user_type_bits {
	UNWIND_USER_TYPE_SFRAME_BIT =		0,
	UNWIND_USER_TYPE_FP_BIT =		1,

	_NR_UNWIND_USER_TYPE_BITS,
};

enum unwind_user_type {
	/* Type "none" for the start of stack walk iteration. */
	UNWIND_USER_TYPE_NONE =			0,
	UNWIND_USER_TYPE_SFRAME =		(1U << UNWIND_USER_TYPE_SFRAME_BIT),
	UNWIND_USER_TYPE_FP =			(1U << UNWIND_USER_TYPE_FP_BIT),
};

struct unwind_stacktrace {
	unsigned int	nr;
	unsigned long	*entries;
};

struct unwind_user_frame {
	s32 cfa_off;
	s32 ra_off;
	s32 fp_off;
	bool use_fp;
};

struct unwind_user_state {
	unsigned long ip;
	unsigned long sp;
	unsigned long fp;
	struct arch_unwind_user_state arch;
	/* Unwind time used for the most recent unwind traversal iteration. */
	enum unwind_user_type current_type;
	/* Unwind types available in the current context. Bitmask of enum unwind_user_type. */
	unsigned int available_types;
	bool done;
};

#endif /* _LINUX_UNWIND_USER_TYPES_H */
