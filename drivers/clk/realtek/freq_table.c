// SPDX-License-Identifier: GPL-2.0-only

#include "freq_table.h"

const struct freq_table *ftbl_find_by_rate(const struct freq_table *ftbl,
					   unsigned long rate)
{
	unsigned long best_rate = 0;
	const struct freq_table *best = NULL;

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

const struct freq_table *
ftbl_find_by_val_with_mask(const struct freq_table *ftbl, u32 mask, u32 value)
{
	for (; !IS_FREQ_TABLE_END(ftbl); ftbl++) {
		if ((ftbl->val & mask) == (value & mask))
			return ftbl;
	}
	return NULL;
};
