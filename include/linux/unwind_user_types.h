/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_UNWIND_USER_TYPES_H
#define _LINUX_UNWIND_USER_TYPES_H

#include <linux/types.h>

enum unwind_user_type {
	UNWIND_USER_TYPE_NONE,
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
	enum unwind_user_type type;
	bool done;
};

#endif /* _LINUX_UNWIND_USER_TYPES_H */
