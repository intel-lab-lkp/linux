/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_USER_UNWIND_H
#define _LINUX_USER_UNWIND_H

#include <linux/types.h>
#include <linux/sframe.h>

enum user_unwind_type {
	USER_UNWIND_TYPE_AUTO,
	USER_UNWIND_TYPE_FP,
};

struct user_unwind_frame {
	s32 cfa_off;
	s32 ra_off;
	s32 fp_off;
	bool use_fp;
};

struct user_unwind_state {
	unsigned long ip, sp, fp;
	enum user_unwind_type type;
	bool done;
};

extern int user_unwind_start(struct user_unwind_state *state, enum user_unwind_type);
extern int user_unwind_next(struct user_unwind_state *state);

#define for_each_user_frame(state, type) \
	for (user_unwind_start(&state, type); !state.done; user_unwind_next(&state))

#endif /* _LINUX_USER_UNWIND_H */
