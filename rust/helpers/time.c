// SPDX-License-Identifier: GPL-2.0

#include <linux/delay.h>
#include <linux/ktime.h>

void rust_helper_fsleep(unsigned long usecs)
{
	fsleep(usecs);
}

ktime_t rust_helper_ktime_add_ns(const ktime_t kt, const u64 nsec)
{
	return ktime_add_ns(kt, nsec);
}

int rust_helper_ktime_compare(const ktime_t cmp1, const ktime_t cmp2)
{
	return ktime_compare(cmp1, cmp2);
}
