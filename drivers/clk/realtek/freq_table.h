/* SPDX-License-Identifier: GPL-2.0-only */

#include <linux/bitops.h>

struct freq_table {
	u32 val;
	unsigned long rate;
};

/* ofs check */
#define CLK_OFS_INVALID (-1)
#define CLK_OFS_IS_VALID(_ofs) ((_ofs) != CLK_OFS_INVALID)

#define FREQ_TABLE_END    \
	{                 \
		.rate = 0 \
	}
#define IS_FREQ_TABLE_END(_f) ((_f)->rate == 0)

const struct freq_table *ftbl_find_by_rate(const struct freq_table *ftbl,
					   unsigned long rate);
const struct freq_table *
ftbl_find_by_val_with_mask(const struct freq_table *ftbl, u32 mask, u32 value);
