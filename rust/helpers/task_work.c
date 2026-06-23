// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (C) 2025 Google LLC.
 */

#include <linux/task_work.h>

__rust_helper void rust_helper_init_task_work(struct callback_head *twork,
					      task_work_func_t func)
{
	init_task_work(twork, func);
}
