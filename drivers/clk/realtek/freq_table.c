// SPDX-License-Identifier: GPL-2.0-only

#include <linux/bitops.h>
#include "freq_table.h"

#define IS_FREQ_TABLE_END(_f) ((_f)->rate == 0)

const struct rtk_freq_table *ftbl_find_by_rate(const struct rtk_freq_table *ftbl,
					       unsigned long rate)
{
	const struct rtk_freq_table *best = NULL;
	unsigned long best_rate = 0;

	for (; !IS_FREQ_TABLE_END(ftbl); ftbl++) {
		if (ftbl->rate == rate)
			return ftbl;

		if (ftbl->rate > rate)
			continue;

		if (ftbl->rate > best_rate) {
			best_rate = ftbl->rate;
			best = ftbl;
		}
	}

	return best;
}

const struct rtk_freq_table *ftbl_find_ceil_by_rate(const struct rtk_freq_table *ftbl,
						    unsigned long rate)
{
	const struct rtk_freq_table *best = NULL;
	unsigned long best_rate = ULONG_MAX;

	for (; !IS_FREQ_TABLE_END(ftbl); ftbl++) {
		if (ftbl->rate < rate)
			continue;

		if (ftbl->rate < best_rate) {
			best_rate = ftbl->rate;
			best = ftbl;
		}
	}

	return best;
}

const struct rtk_freq_table *ftbl_find_by_val_with_mask(const struct rtk_freq_table *ftbl,
							u32 mask, u32 value)
{
	for (; !IS_FREQ_TABLE_END(ftbl); ftbl++) {
		if ((ftbl->val & mask) == (value & mask))
			return ftbl;
	}
	return NULL;
}
