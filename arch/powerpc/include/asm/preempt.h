/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_POWERPC_PREEMPT_H
#define __ASM_POWERPC_PREEMPT_H

#include <asm-generic/preempt.h>

#if defined(CONFIG_PREEMPT_DYNAMIC) && defined(CONFIG_HAVE_PREEMPT_DYNAMIC_KEY)
DECLARE_STATIC_KEY_TRUE(sk_dynamic_irqentry_exit_cond_resched);
#endif

#endif /* __ASM_POWERPC_PREEMPT_H */
