// SPDX-License-Identifier: GPL-2.0

#include <linux/completion.h>

__rust_helper void rust_helper_init_completion(struct completion *x)
{
	init_completion(x);
}

__rust_helper void rust_helper_wait_for_completion(struct completion *x)
{
	wait_for_completion(x);
}
