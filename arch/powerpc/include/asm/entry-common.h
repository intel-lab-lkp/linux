/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ARCH_POWERPC_ENTRY_COMMON_H
#define ARCH_POWERPC_ENTRY_COMMON_H

#include <linux/user-return-notifier.h>

static inline void arch_exit_to_user_mode_prepare(struct pt_regs *regs,
						  unsigned long ti_work)
{
	if (ti_work & _TIF_USER_RETURN_NOTIFY)
		fire_user_return_notifiers();
}

#define arch_exit_to_user_mode_prepare arch_exit_to_user_mode_prepare

#endif
