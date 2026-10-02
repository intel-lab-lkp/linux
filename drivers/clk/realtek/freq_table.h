/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CLK_REALTEK_FREQ_TABLE_H
#define __CLK_REALTEK_FREQ_TABLE_H

struct rtk_freq_table {
	u32 val;
	unsigned long rate;
};

#define FREQ_TABLE_END    \
	{                 \
		.rate = 0 \
	}

const struct rtk_freq_table *ftbl_find_by_rate(const struct rtk_freq_table *ftbl,
					       unsigned long rate);
const struct rtk_freq_table *ftbl_find_ceil_by_rate(const struct rtk_freq_table *ftbl,
						    unsigned long rate);
const struct rtk_freq_table *
ftbl_find_by_val_with_mask(const struct rtk_freq_table *ftbl, u32 mask, u32 value);

#endif /* __CLK_REALTEK_FREQ_TABLE_H */
