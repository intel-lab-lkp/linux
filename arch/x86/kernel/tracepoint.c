// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2013 Seiji Aguchi <seiji.aguchi@hds.com>
 */
#include <linux/jump_label.h>
#include <linux/atomic.h>

#include <asm/trace/exceptions.h>

DEFINE_STATIC_KEY_FALSE(trace_pagefault_enter_key);
DEFINE_STATIC_KEY_FALSE(trace_pagefault_exit_key);

int trace_pagefault_enter_reg(void)
{
	static_branch_inc(&trace_pagefault_enter_key);
	return 0;
}

void trace_pagefault_enter_unreg(void)
{
	static_branch_dec(&trace_pagefault_enter_key);
}

int trace_pagefault_exit_reg(void)
{
	static_branch_inc(&trace_pagefault_exit_key);
	return 0;
}

void trace_pagefault_exit_unreg(void)
{
	static_branch_dec(&trace_pagefault_exit_key);
}
