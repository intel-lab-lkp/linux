// SPDX-License-Identifier: GPL-2.0

#include <linux/preempt.h>

void rust_helper_preempt_disable(void)
{
	preempt_disable();
}

void rust_helper_preempt_enable(void)
{
	preempt_enable();
}

