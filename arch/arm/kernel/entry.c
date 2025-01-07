// SPDX-License-Identifier: GPL-2.0
#include <asm/entry.h>
#include <linux/context_tracking.h>

noinstr void irqentry_enter_from_user_mode(struct pt_regs *regs)
{
	/* This context tracking call has inverse naming */
	user_exit_callable();
}

noinstr void irqentry_exit_to_user_mode(struct pt_regs *regs)
{
	/* This context tracking call has inverse naming */
	user_enter_callable();
}
