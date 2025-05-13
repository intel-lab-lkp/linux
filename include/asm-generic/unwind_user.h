/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_GENERIC_UNWIND_USER_H
#define _ASM_GENERIC_UNWIND_USER_H

#include <asm/unwind_user_types.h>

#ifndef ARCH_INIT_USER_FP_FRAME
 #define ARCH_INIT_USER_FP_FRAME
#endif

#ifndef ARCH_INIT_USER_COMPAT_FP_FRAME
 #define ARCH_INIT_USER_COMPAT_FP_FRAME
 #define in_compat_mode(regs) false
#endif

#ifndef arch_unwind_user_init
static inline void arch_unwind_user_init(struct unwind_user_state *state, struct pt_regs *reg) {}
#endif

#ifndef arch_unwind_user_next
static inline void arch_unwind_user_next(struct unwind_user_state *state) {}
#endif

#endif /* _ASM_GENERIC_UNWIND_USER_H */
