// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2024 Rockchip Electronics Co., Ltd.
 *
 * Adjustable fractional divider clock implementation.
 * Uses rational best approximation algorithm.
 *
 * Output is calculated as
 *
 *	rate = (m / n) * parent_rate				(1)
 *
 * Current callers use flags = 0 and split each value into 16-bit low and
 * 8-bit high fields. The nonzero field limits are
 *
 *	1 <= m <= max_m = 2^mwidth - 1
 *	1 <= n <= max_n = 2^nwidth - 1				(2)
 *
 * For m < 4, keep even denominators unchanged to preserve the duty ratio.
 * For odd denominators, raise m to at least 4 without changing the ratio:
 *
 *	factor = ceil(4 / m), (m', n') = (factor * m, factor * n)	(3)
 *
 * Check m <= floor(max_m / factor) and n <= floor(max_n / factor) before
 * multiplying. A zero numerator means bypass; if the best fraction has
 * m = 0 or cannot be compensated within (2), choose the closest usable
 * positive ratio. For these field widths, this fallback is needed only
 * when
 *
 *	requested_rate / parent_rate < 6 / max_n			(4)
 *
 * The largest compensated small numerator is 3 * ceil(4 / 3) = 6.
 * At or above this bound, 6 / max_n would beat the overflowing fraction;
 * below it, no numerator above 6 can improve on 6 / max_n.
 *
 * For each m from 1 to 6, try the adjacent usable denominators around
 * parent_rate * m / requested_rate, clamped to the field range. Their
 * step is 2 for m < 4 and 1 otherwise. Minimize the exact frequency error:
 *
 *	error = abs(parent_rate * m - requested_rate * n) / n	(5)
 *
 * Compare errors by cross multiplication, choosing the lower rate on a
 * tie. The minimum usable output is parent_rate / (max_n - 1), since m = 1
 * needs an even denominator. Lower positive targets saturate at this
 * limit without assuming an additional prescaler.
 *
 * determine_rate() and set_rate() share ratio selection. Rate reporting
 * in determine_rate() and recalc_rate() uses the same nearest-Hz rounding:
 *
 *	reported_rate = floor(parent_rate * m / n + 1 / 2)		(6)
 *
 * This keeps integer-Hz rates consistent when programmed again. Division
 * in these formulas is exact unless floor or ceil is specified.
 */

#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/device.h>
#include <linux/math64.h>
#include <linux/slab.h>
#include <linux/rational.h>
#include "clk.h"

struct clk_fractional_divider_v2 {
	struct clk_hw	hw;
	void __iomem	*reg;
	void __iomem	*high_reg;
	u8		mshift;
	u8		mwidth;
	u32		mmask;
	u8		nshift;
	u8		nwidth;
	u32		nmask;
	u8		high_mshift;
	u8		high_mwidth;
	u32		high_mmask;
	u8		high_nshift;
	u8		high_nwidth;
	u32		high_nmask;
	u8		flags;
	spinlock_t	*lock; /* Protects the low and high register pair. */
};

#define to_clk_fd_v2(_hw) container_of(_hw, struct clk_fractional_divider_v2, hw)

static inline u32 clk_fd_v2_readl(struct clk_fractional_divider_v2 *fd,
				  void __iomem *reg)
{
	return readl(reg);
}

static inline void clk_fd_v2_writel(struct clk_fractional_divider_v2 *fd,
				    void __iomem *reg, u32 val)
{
	writel(val, reg);
}

static unsigned long clk_fd_v2_recalc_rate(struct clk_hw *hw,
					   unsigned long parent_rate)
{
	struct clk_fractional_divider_v2 *fd = to_clk_fd_v2(hw);
	unsigned long flags = 0;
	unsigned long m_low, n_low, m_high, n_high, m, n;
	u32 val_low, val_high;

	if (fd->lock)
		spin_lock_irqsave(fd->lock, flags);
	else
		__acquire(fd->lock);

	val_low = clk_fd_v2_readl(fd, fd->reg);
	val_high = clk_fd_v2_readl(fd, fd->high_reg);

	if (fd->lock)
		spin_unlock_irqrestore(fd->lock, flags);
	else
		__release(fd->lock);

	m_low = (val_low & fd->mmask) >> fd->mshift;
	n_low = (val_low & fd->nmask) >> fd->nshift;
	m_high = (val_high & fd->high_mmask) >> fd->high_mshift;
	n_high = (val_high & fd->high_nmask) >> fd->high_nshift;
	m = (m_high << 16) | m_low;
	n = (n_high << 16) | n_low;

	if (fd->flags & CLK_FRAC_DIVIDER_ZERO_BASED) {
		m++;
		n++;
	}

	if (!n || !m)
		return parent_rate;

	return DIV_ROUND_CLOSEST_ULL((u64)parent_rate * m, n);
}

static int clk_fd_v2_get_low_rate_divider(struct clk_fractional_divider_v2 *fd,
					  unsigned long rate, unsigned long parent_rate,
					  unsigned long *m, unsigned long *n)
{
	unsigned long max_m = GENMASK(fd->mwidth - 1, 0);
	unsigned long max_n = GENMASK(fd->nwidth - 1, 0);
	unsigned long num, den, step;
	u64 error, best_error = 0;
	int i;

	/*
	 * Compensation can overflow only for m < 4, at ratios below
	 * 6 / max_n. No numerator above 6 can improve on 6 / max_n.
	 * Numerators below 4 need an even denominator to avoid compensation.
	 */
	*n = 0;
	for (num = 1; num <= min(max_m, 6UL); num++) {
		u64 dividend = (u64)parent_rate * num;

		step = num < 4 ? 2 : 1;
		den = min_t(u64, div64_ul(dividend, rate), max_n);
		den = max(den - den % step, step);
		for (i = 0; i < 2 && den <= max_n; i++, den += step) {
			u64 target = (u64)rate * den;

			error = dividend > target ? dividend - target : target - dividend;
			if (!*n || error * *n < best_error * den ||
			    (error * *n == best_error * den && num * *n < *m * den)) {
				*m = num;
				*n = den;
				best_error = error;
			}
		}
	}

	return *n ? 0 : -EINVAL;
}

static int clk_fd_v2_get_divider(struct clk_hw *hw, unsigned long rate,
				 unsigned long parent_rate,
				 unsigned long *m, unsigned long *n)
{
	struct clk_fractional_divider_v2 *fd = to_clk_fd_v2(hw);
	u32 factor;

	if (!parent_rate)
		return -EINVAL;
	if (!rate || rate > parent_rate)
		rate = parent_rate;

	rational_best_approximation(rate, parent_rate,
				    GENMASK(fd->mwidth - 1, 0),
				    GENMASK(fd->nwidth - 1, 0),
				    m, n);

	if (fd->flags & CLK_FRAC_DIVIDER_ZERO_BASED) {
		(*m)--;
		(*n)--;
	}
	if (!*m && !(fd->flags & CLK_FRAC_DIVIDER_ZERO_BASED))
		return clk_fd_v2_get_low_rate_divider(fd, rate, parent_rate, m, n);
	/*
	 * When compensation the fractional divider,
	 * the [1:0] bits of the numerator register are omitted,
	 * which will lead to a large deviation in the result.
	 * Therefore, it is required that the numerator must
	 * be at least 4.
	 *
	 * Note that there are some exceptions here:
	 * If there is an even frac div, we need to keep the original
	 * numerator(<4) and denominator. Otherwise, it may cause the
	 * issue that the duty ratio is not 50%.
	 */
	if (*m < 4 && *m != 0) {
		if (*n % 2 == 0)
			factor = 1;
		else
			factor = DIV_ROUND_UP(4, *m);

		if (*m > GENMASK(fd->mwidth - 1, 0) / factor ||
		    *n > GENMASK(fd->nwidth - 1, 0) / factor) {
			if (fd->flags & CLK_FRAC_DIVIDER_ZERO_BASED)
				return -EINVAL;
			return clk_fd_v2_get_low_rate_divider(fd, rate, parent_rate, m, n);
		}

		*n *= factor;
		*m *= factor;
	}

	return 0;
}

static int clk_fd_v2_determine_rate(struct clk_hw *hw,
				    struct clk_rate_request *req)
{
	struct clk_fractional_divider_v2 *fd = to_clk_fd_v2(hw);
	unsigned long m, n;
	int ret;

	ret = clk_fd_v2_get_divider(hw, req->rate, req->best_parent_rate, &m, &n);
	if (ret)
		return ret;

	if (fd->flags & CLK_FRAC_DIVIDER_ZERO_BASED) {
		m++;
		n++;
	}

	/* Keep integer-Hz requests stable when set_rate() selects the divider. */
	req->rate = DIV_ROUND_CLOSEST_ULL((u64)req->best_parent_rate * m, n);
	return 0;
}

static int clk_fd_v2_set_rate(struct clk_hw *hw, unsigned long rate,
			      unsigned long parent_rate)
{
	struct clk_fractional_divider_v2 *fd = to_clk_fd_v2(hw);
	unsigned long flags = 0;
	unsigned long m, n, m_low, n_low, m_high, n_high;
	u32 val, val_high;
	int ret;

	ret = clk_fd_v2_get_divider(hw, rate, parent_rate, &m, &n);
	if (ret)
		return ret;

	if (fd->lock)
		spin_lock_irqsave(fd->lock, flags);
	else
		__acquire(fd->lock);

	m_low = m & fd->nmask;
	n_low = n & fd->nmask;
	m_high = (m & GENMASK(fd->mwidth - 1, (fd->mwidth - fd->high_mwidth))) >>
		 (fd->mwidth - fd->high_mwidth);
	n_high = (n & GENMASK(fd->nwidth - 1, (fd->nwidth - fd->high_nwidth))) >>
		 (fd->nwidth - fd->high_nwidth);

	val_high = clk_fd_v2_readl(fd, fd->high_reg);
	val_high &= ~(fd->high_mmask | fd->high_nmask);
	val_high |= (m_high << fd->high_mshift) | (n_high << fd->high_nshift);

	clk_fd_v2_writel(fd, fd->high_reg, val_high);

	val = clk_fd_v2_readl(fd, fd->reg);
	val &= ~(fd->mmask | fd->nmask);
	val |= (m_low << fd->mshift) | (n_low << fd->nshift);
	clk_fd_v2_writel(fd, fd->reg, val);

	if (fd->lock)
		spin_unlock_irqrestore(fd->lock, flags);
	else
		__release(fd->lock);

	return 0;
}

static const struct clk_ops clk_fractional_divider_ops_v2 = {
	.recalc_rate = clk_fd_v2_recalc_rate,
	.determine_rate = clk_fd_v2_determine_rate,
	.set_rate = clk_fd_v2_set_rate,
};

static struct clk_hw *clk_hw_register_fractional_divider_v2(struct device *dev,
							    const char *name,
							    const char *parent_name,
							    unsigned long flags,
							    void __iomem *reg,
							    u8 mshift, u8 mwidth,
							    u8 nshift, u8 nwidth,
							    void __iomem *high_reg,
							    u8 high_mshift, u8 high_mwidth,
							    u8 high_nshift, u8 high_nwidth,
							    u8 clk_divider_flags,
							    spinlock_t *lock)
{
	struct clk_fractional_divider_v2 *fd;
	struct clk_init_data init;
	struct clk_hw *hw;
	int ret;

	fd = kzalloc_obj(*fd);
	if (!fd)
		return ERR_PTR(-ENOMEM);

	init.name = name;
	init.ops = &clk_fractional_divider_ops_v2;
	init.flags = flags;
	init.parent_names = parent_name ? &parent_name : NULL;
	init.num_parents = parent_name ? 1 : 0;

	fd->reg = reg;
	fd->mshift = mshift;
	fd->mwidth = mwidth + high_mwidth;
	fd->mmask = GENMASK(mwidth - 1, 0) << mshift;
	fd->nshift = nshift;
	fd->nwidth = nwidth + high_nwidth;
	fd->nmask = GENMASK(nwidth - 1, 0) << nshift;
	fd->high_reg = high_reg;
	fd->high_mshift = high_mshift;
	fd->high_mwidth = high_mwidth;
	fd->high_mmask = GENMASK(high_mwidth - 1, 0) << high_mshift;
	fd->high_nshift = high_nshift;
	fd->high_nwidth = high_nwidth;
	fd->high_nmask = GENMASK(high_nwidth - 1, 0) << high_nshift;
	fd->flags = clk_divider_flags;
	fd->lock = lock;
	fd->hw.init = &init;

	hw = &fd->hw;
	ret = clk_hw_register(dev, hw);
	if (ret) {
		kfree(fd);
		hw = ERR_PTR(ret);
	}

	return hw;
}

struct clk *rockchip_clk_register_frac_v2(struct device *dev,
					  const char *name,
					  const char *parent_name,
					  unsigned long flags,
					  void __iomem *reg,
					  u8 mshift, u8 width,
					  void __iomem *high_reg,
					  u8 high_mshift, u8 high_width,
					  u8 clk_divider_flags,
					  spinlock_t *lock)
{
	struct clk_hw *hw;

	hw = clk_hw_register_fractional_divider_v2(dev, name, parent_name, flags,
						   reg, mshift, width, 0, width,
						   high_reg, high_mshift, high_width,
						   0, high_width,
						   clk_divider_flags,
						   lock);
	if (IS_ERR(hw))
		return ERR_CAST(hw);
	return hw->clk;
}
