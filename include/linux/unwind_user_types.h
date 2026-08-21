/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_UNWIND_USER_TYPES_H
#define _LINUX_UNWIND_USER_TYPES_H

#include <linux/types.h>

/*
 * Unwind types, listed in priority order: lower numbers are attempted first if
 * available.
 */
enum unwind_user_type_bits {
	UNWIND_USER_TYPE_FP_BIT =		0,

	NR_UNWIND_USER_TYPE_BITS,
};

enum unwind_user_type {
	/* Type "none" for the start of stack walk iteration. */
	UNWIND_USER_TYPE_NONE =			0,
	UNWIND_USER_TYPE_FP =			BIT(UNWIND_USER_TYPE_FP_BIT),
};

struct unwind_stacktrace {
	unsigned int	nr;
	unsigned long	*entries;
};

#define UNWIND_USER_RULE_DEREF			BIT(31)

enum unwind_user_cfa_rule {
	UNWIND_USER_CFA_RULE_SP_OFFSET,		/* CFA = SP + offset */
	UNWIND_USER_CFA_RULE_FP_OFFSET,		/* CFA = FP + offset */
	UNWIND_USER_CFA_RULE_REG_OFFSET,	/* CFA = register + offset */
	/* DEREF variants */
	UNWIND_USER_CFA_RULE_SP_OFFSET_DEREF =	/* CFA = *(SP + offset) */
		UNWIND_USER_CFA_RULE_SP_OFFSET | UNWIND_USER_RULE_DEREF,
	UNWIND_USER_CFA_RULE_FP_OFFSET_DEREF =	/* CFA = *(FP + offset) */
		UNWIND_USER_CFA_RULE_FP_OFFSET | UNWIND_USER_RULE_DEREF,
	UNWIND_USER_CFA_RULE_REG_OFFSET_DEREF =	/* CFA = *(register + offset) */
		UNWIND_USER_CFA_RULE_REG_OFFSET | UNWIND_USER_RULE_DEREF,
};

struct unwind_user_cfa_rule_data {
	enum unwind_user_cfa_rule rule;
	s32 offset;
	unsigned int regnum;
};

enum unwind_user_rule {
	UNWIND_USER_RULE_RETAIN,		/* entity = entity */
	UNWIND_USER_RULE_CFA_OFFSET,		/* entity = CFA + offset */
	UNWIND_USER_RULE_REG_OFFSET,		/* entity = register + offset */
	/* DEREF variants */
	UNWIND_USER_RULE_CFA_OFFSET_DEREF =	/* entity = *(CFA + offset) */
		UNWIND_USER_RULE_CFA_OFFSET | UNWIND_USER_RULE_DEREF,
	UNWIND_USER_RULE_REG_OFFSET_DEREF =	/* entity = *(register + offset) */
		UNWIND_USER_RULE_REG_OFFSET | UNWIND_USER_RULE_DEREF,
};

struct unwind_user_rule_data {
	enum unwind_user_rule rule;
	s32 offset;
	unsigned int regnum;
};

struct unwind_user_frame {
	struct unwind_user_cfa_rule_data cfa;
	struct unwind_user_rule_data ra;
	struct unwind_user_rule_data fp;
	bool outermost;
};

struct unwind_user_state {
	unsigned long				ip;
	unsigned long				sp;
	unsigned long				fp;
	unsigned int				ws;
	enum unwind_user_type			current_type;
	unsigned int				available_types;
	bool					topmost;
	bool					done;
};

#endif /* _LINUX_UNWIND_USER_TYPES_H */
