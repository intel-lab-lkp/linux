/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_UNWIND_USER_H
#define _LINUX_UNWIND_USER_H

#include <linux/unwind_user_types.h>

int unwind_user_start(struct unwind_user_state *state);
int unwind_user_next(struct unwind_user_state *state);

int unwind_user(struct unwind_stacktrace *trace, unsigned int max_entries);

#define for_each_user_frame(state) \
	for (unwind_user_start((state)); !(state)->done; unwind_user_next((state)))

#endif /* _LINUX_UNWIND_USER_H */
