// SPDX-License-Identifier: GPL-2.0

#include <linux/delay.h>

void rust_helper_fsleep(unsigned long usecs)
{
	fsleep(usecs);
}
