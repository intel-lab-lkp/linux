// SPDX-License-Identifier: GPL-2.0

#include <linux/kernel.h>

void rust_helper_cpu_relax(void)
{
	cpu_relax();
}

void rust_helper___might_sleep(const char *file, int line)
{
	__might_sleep(file, line);
}
