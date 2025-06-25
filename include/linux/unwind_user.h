/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_UNWIND_USER_H
#define _LINUX_UNWIND_USER_H

#include <linux/unwind_user_types.h>
#include <asm/unwind_user.h>

#ifndef ARCH_INIT_USER_FP_FRAME
 #define ARCH_INIT_USER_FP_FRAME
#endif

#ifndef ARCH_INIT_USER_COMPAT_FP_FRAME
 #define ARCH_INIT_USER_COMPAT_FP_FRAME
 #define in_compat_mode(regs) false
#endif

/*
 * If an architecture needs to initialize the state for a specific
 * reason, for example, it may need to do something different
 * in compat mode, it can define arch_unwind_user_init to a
 * function that will perform this initialization.
 */
#ifndef arch_unwind_user_init
static inline void arch_unwind_user_init(struct unwind_user_state *state, struct pt_regs *reg) {}
#endif

/*
 * If an architecture requires some more updates to the state between
 * stack frames, it can define arch_unwind_user_next to a function
 * that will update the state between reading stack frames during
 * the user space stack walk.
 */
#ifndef arch_unwind_user_next
static inline void arch_unwind_user_next(struct unwind_user_state *state) {}
#endif

int unwind_user_start(struct unwind_user_state *state);
int unwind_user_next(struct unwind_user_state *state);

int unwind_user(struct unwind_stacktrace *trace, unsigned int max_entries);

#define for_each_user_frame(state) \
	for (unwind_user_start((state)); !(state)->done; unwind_user_next((state)))

#endif /* _LINUX_UNWIND_USER_H */
