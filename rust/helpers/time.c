// SPDX-License-Identifier: GPL-2.0

#include <linux/ktime.h>

int rust_helper_ktime_compare(const ktime_t cmp1, const ktime_t cmp2)
{
	return ktime_compare(cmp1, cmp2);
}
