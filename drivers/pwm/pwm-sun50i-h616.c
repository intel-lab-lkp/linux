// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for Allwinner H616 Pulse Width Modulation Controller
 *
 * (C) Copyright 2025 Richard Genoud, Bootlin <richard.genoud@bootlin.com>
 *
 * Based on drivers/pwm/pwm-sun4i.c with Copyright:
 *
 * Copyright (C) 2014 Alexandre Belloni <alexandre.belloni@bootlin.com>
 *
 * Limitations:
 * - When outputing the source clock directly, the PWM logic is bypassed and the
 *   currently running period is not guaranteed to be completed.
 * - As the channels are paired (0/1, 2/3, 4/5), they share the same clock
 *   source and prescaler(div_m), but they also have their own prescaler(div_k)
 *   and bypass.
 *
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/reset.h>
#include <linux/spinlock.h>
#include <linux/time.h>

#ifndef UINT32_MAX
#define UINT32_MAX 0xffffffffU
#endif

/* PWM IRQ Enable Register */
#define PWM_IER				0x0

/* PWM IRQ Status Register */
#define PWM_ISR				0x4

/* PWM Capture IRQ Enable Register */
#define PWM_CIER			0x10

/* PWM Capture IRQ Status Register */
#define PWM_CISR			0x14

/* PWMCC Pairs Clock Configuration Registers */
#define PWM_XY_CLK_CR(pair)		(0x20 + ((pair) * 0x4))
#define PWM_XY_CLK_CR_SRC_SHIFT		7
#define PWM_XY_CLK_CR_SRC_MASK		1
#define PWM_XY_CLK_CR_GATE_BIT		4
#define PWM_XY_CLK_CR_BYPASS_BIT(chan)	((chan) % 2 + 5)
#define PWM_XY_CLK_CR_DIV_M_SHIFT	0

/* PWMCC Pairs Dead Zone Control Registers */
#define PWM_XY_DZ(pair)			(0x30 + ((pair) * 0x4))

/* PWM Enable Register */
#define PWM_ENR				0x40
#define PWM_ENABLE(x)			BIT(x)

/* PWM Capture Enable Register */
#define PWM_CER				0x44

/* PWM Control Register */
#define PWM_CTRL_REG(chan)		(0x60 + (chan) * 0x20)
#define PWM_CTRL_PRESCAL_K_SHIFT	0
#define PWM_CTRL_PRESCAL_K_WIDTH	8
#define PWM_CTRL_ACTIVE_STATE		BIT(8)

/* PWM Period Register */
#define PWM_PERIOD_REG(ch)		(0x64 + (ch) * 0x20)
#define PWM_PERIOD_MASK			GENMASK(31, 16)
#define PWM_DUTY_MASK			GENMASK(15, 0)
#define PWM_REG_PERIOD(reg)		(FIELD_GET(PWM_PERIOD_MASK, reg) + 1)
#define PWM_REG_DUTY(reg)		FIELD_GET(PWM_DUTY_MASK, reg)
#define PWM_PERIOD(prd)			FIELD_PREP(PWM_PERIOD_MASK, (prd) - 1)
#define PWM_DUTY(dty)			FIELD_PREP(PWM_DUTY_MASK, dty)
#define PWM_PERIOD_MAX			FIELD_MAX(PWM_PERIOD_MASK)


/* PWM Count Register */
#define PWM_CNT_REG(x)			(0x68 + (x) * 0x20)

/* PWM Capture Control Register */
#define PWM_CCR(x)			(0x6c + (x) * 0x20)

/* PWM Capture Rise Lock Register */
#define PWM_CRLR(x)			(0x70 + (x) * 0x20)

/* PWM Capture Fall Lock Register */
#define PWM_CFLR(x)			(0x74 + (x) * 0x20)

#define PWM_PAIR_IDX(chan)		((chan) >> 2)

/*
 * Block diagram of the PWM clock controller:
 *
 *             _____      ______      ________
 * OSC24M --->|     |    |      |    |        |
 * APB1 ----->| Mux |--->| Gate |--->| /div_m |-----> PWM_clock_src_xy
 *            |_____|    |______|    |________|
 *                           ________
 *                          |        |
 *                       +->| /div_k |---> PWM_clock_x
 *                       |  |________|
 *                       |    ______
 *                       |   |      |
 *                       +-->| Gate |----> PWM_bypass_clock_x
 *                       |   |______|
 * PWM_clock_src_xy -----+   ________
 *                       |  |        |
 *                       +->| /div_k |---> PWM_clock_y
 *                       |  |________|
 *                       |    ______
 *                       |   |      |
 *                       +-->| Gate |----> PWM_bypass_clock_y
 *                           |______|
 *
 * NB: when the bypass is set, all the PWM logic is bypassed.
 * So, the duty cycle and polarity can't be modified (we just have a clock).
 * The bypass in PWM mode is used to achieve a 1/2 duty cycle with the fastest
 * clock.
 *
 * PWM_clock_x/y serve for the PWM purpose.
 * PWM_bypass_clock_x/y serve for the clock-provider purpose.
 *
 */

/*
 * Table used for /div_m (diviser before obtaining PWM_clock_src_xy)
 * It's actually CLK_DIVIDER_POWER_OF_TWO, but limited to /256
 */
static const struct clk_div_table clk_table_div_m[] = {
	{ .val = 0, .div = 1, },
	{ .val = 1, .div = 2, },
	{ .val = 2, .div = 4, },
	{ .val = 3, .div = 8, },
	{ .val = 4, .div = 16, },
	{ .val = 5, .div = 32, },
	{ .val = 6, .div = 64, },
	{ .val = 7, .div = 128, },
	{ .val = 8, .div = 256, },
	{ .val = 0, .div = 0, }, /* last entry */
};

#define PWM_XY_SRC_GATE(_pair, _reg)			\
struct clk_gate gate_xy_##_pair = {			\
	.reg = (void *)_reg,				\
	.bit_idx = PWM_XY_CLK_CR_GATE_BIT,		\
	.hw.init = &(struct clk_init_data){		\
		.ops =  &clk_gate_ops,			\
	}						\
}

#define PWM_XY_SRC_MUX(_pair, _reg)			\
struct clk_mux mux_xy_##_pair = {			\
	.reg = (void *)_reg,				\
	.shift = PWM_XY_CLK_CR_SRC_SHIFT,		\
	.mask = PWM_XY_CLK_CR_SRC_MASK,			\
	.flags = CLK_MUX_ROUND_CLOSEST,			\
	.hw.init = &(struct clk_init_data){		\
		.ops =  &clk_mux_ops,			\
	}						\
}

#define PWM_XY_SRC_DIV(_pair, _reg)			\
struct clk_divider rate_xy_##_pair = {			\
	.reg = (void *)_reg,				\
	.shift = PWM_XY_CLK_CR_DIV_M_SHIFT,		\
	.table = clk_table_div_m,			\
	.hw.init = &(struct clk_init_data){		\
		.ops =  &clk_divider_ops,		\
	}						\
}

#define PWM_X_DIV(_idx, _reg)				\
struct clk_divider rate_x_##_idx = {			\
	.reg = (void *)_reg,				\
	.shift = PWM_CTRL_PRESCAL_K_SHIFT,		\
	.width = PWM_CTRL_PRESCAL_K_WIDTH,		\
	.hw.init = &(struct clk_init_data){		\
		.ops =  &clk_divider_ops,		\
	}						\
}

#define PWM_X_BYPASS_GATE(_idx)				\
struct clk_gate gate_x_bypass_##_idx = {		\
	.reg = (void *)PWM_ENR,				\
	.bit_idx = _idx,				\
	.hw.init = &(struct clk_init_data){		\
		.ops =  &clk_gate_ops,			\
	}						\
}

#define PWM_XY_CLK_SRC(_pair, _reg)			\
	static PWM_XY_SRC_MUX(_pair, _reg);		\
	static PWM_XY_SRC_GATE(_pair, _reg);		\
	static PWM_XY_SRC_DIV(_pair, _reg)

#define PWM_X_CLK(_idx)					\
	static PWM_X_DIV(_idx, PWM_CTRL_REG(_idx))

#define PWM_X_BYPASS_CLK(_idx)						\
	PWM_X_BYPASS_GATE(_idx)

#define REF_CLK_XY_SRC(_pair)						\
	{								\
		.name = "pwm-clk-src" #_pair,				\
		.mux_hw = &mux_xy_##_pair.hw,				\
		.gate_hw = &gate_xy_##_pair.hw,				\
		.rate_hw = &rate_xy_##_pair.hw,				\
	}

#define REF_CLK_X(_idx, _pair)						\
	{								\
		.name = "pwm-clk" #_idx,				\
		.parent_names = (const char *[]){ "pwm-clk-src" #_pair }, \
		.num_parents = 1,					\
		.rate_hw = &rate_x_##_idx.hw,				\
		.flags = CLK_SET_RATE_PARENT,				\
	}

#define REF_CLK_BYPASS(_idx, _pair)					\
	{								\
		.name = "pwm-clk-bypass" #_idx,				\
		.parent_names = (const char *[]){ "pwm-clk-src" #_pair }, \
		.num_parents = 1,					\
		.gate_hw = &gate_x_bypass_##_idx.hw,			\
		.flags = CLK_SET_RATE_PARENT,	\
	}

/*
 * PWM_clock_src_xy generation:
 *             _____      ______      ________
 * OSC24M --->|     |    |      |    |        |
 * APB1 ----->| Mux |--->| Gate |--->| /div_m |-----> PWM_clock_src_xy
 *            |_____|    |______|    |________|
 */
PWM_XY_CLK_SRC(01, PWM_XY_CLK_CR(0));
PWM_XY_CLK_SRC(23, PWM_XY_CLK_CR(1));
PWM_XY_CLK_SRC(45, PWM_XY_CLK_CR(2));

/*
 * PWM_clock_x_div generation:
 *                       ________
 *                      |        | PWM_clock_x/y
 * PWM_clock_src_xy --->| /div_k |--------------->
 *                      |________|
 */
PWM_X_CLK(0);
PWM_X_CLK(1);
PWM_X_CLK(2);
PWM_X_CLK(3);
PWM_X_CLK(4);
PWM_X_CLK(5);

/*
 * PWM_bypass_clock_xy generation:
 *                        ______
 *                       |      |
 * PWM_clock_src_xy ---->| Gate |-------> PWM_bypass_clock_x
 *                       |______|
 *
 * The gate is actually PWM_ENR register.
 */
PWM_X_BYPASS_CLK(0);
PWM_X_BYPASS_CLK(1);
PWM_X_BYPASS_CLK(2);
PWM_X_BYPASS_CLK(3);
PWM_X_BYPASS_CLK(4);
PWM_X_BYPASS_CLK(5);

struct clk_pwm_data {
	const char *name;
	const char **parent_names;
	unsigned int num_parents;
	struct clk_hw *mux_hw;
	struct clk_hw *rate_hw;
	struct clk_hw *gate_hw;
	unsigned long flags;
};

#define CLK_BYPASS(h616chip, ch) ((h616chip)->data->npwm + (ch))
#define CLK_XY_SRC_IDX(h616chip, ch) ((h616chip)->data->npwm * 2 + ((ch) >> 1))
static struct clk_pwm_data pwmcc_data[] = {
	REF_CLK_X(0, 01),
	REF_CLK_X(1, 01),
	REF_CLK_X(2, 23),
	REF_CLK_X(3, 23),
	REF_CLK_X(4, 45),
	REF_CLK_X(5, 45),
	REF_CLK_BYPASS(0, 01),
	REF_CLK_BYPASS(1, 01),
	REF_CLK_BYPASS(2, 23),
	REF_CLK_BYPASS(3, 23),
	REF_CLK_BYPASS(4, 45),
	REF_CLK_BYPASS(5, 45),
	REF_CLK_XY_SRC(01),
	REF_CLK_XY_SRC(23),
	REF_CLK_XY_SRC(45),
	{ /* sentinel */ },
};

enum h616_pwm_mode {
	H616_PWM_MODE_NONE,
	H616_PWM_MODE_PWM,
	H616_PWM_MODE_CLK,
};

struct h616_pwm_data {
	unsigned int npwm;
};

struct h616_pwm_channel {
	struct clk *pwm_clk;
	unsigned long rate;
	unsigned int entire_cycles;
	unsigned int active_cycles;
	enum h616_pwm_mode mode;
	bool bypass;
};

struct clk_pwm_pdata {
	struct clk_hw_onecell_data *hw_data;
	spinlock_t lock;
	void __iomem *reg;
};

struct h616_pwm_chip {
	struct clk_pwm_pdata *clk_pdata;
	struct h616_pwm_channel *channels;
	struct clk *bus_clk;
	struct reset_control *rst;
	void __iomem *base;
	const struct h616_pwm_data *data;
};

static inline struct h616_pwm_chip *to_h616_pwm_chip(struct pwm_chip *chip)
{
	return pwmchip_get_drvdata(chip);
}

static inline u32 h616_pwm_readl(struct h616_pwm_chip *h616chip,
				 unsigned long offset)
{
	return readl(h616chip->base + offset);
}

static inline void h616_pwm_writel(struct h616_pwm_chip *h616chip,
				   u32 val, unsigned long offset)
{
	writel(val, h616chip->base + offset);
}

static void h616_pwm_set_bypass(struct h616_pwm_chip *h616chip, unsigned int idx,
				bool en_bypass)
{
	unsigned long flags;
	u32 val;

	spin_lock_irqsave(&h616chip->clk_pdata->lock, flags);

	val = h616_pwm_readl(h616chip, PWM_XY_CLK_CR(PWM_PAIR_IDX(idx)));
	if (en_bypass)
		val |= BIT(PWM_XY_CLK_CR_BYPASS_BIT(idx));
	else
		val &= ~BIT(PWM_XY_CLK_CR_BYPASS_BIT(idx));

	h616_pwm_writel(h616chip, val, PWM_XY_CLK_CR(PWM_PAIR_IDX(idx)));

	spin_unlock_irqrestore(&h616chip->clk_pdata->lock, flags);
}

static int h616_pwm_request(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct h616_pwm_chip *h616chip = to_h616_pwm_chip(chip);
	struct h616_pwm_channel *chan = &h616chip->channels[pwm->hwpwm];
	struct device *dev = pwmchip_parent(chip);
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&h616chip->clk_pdata->lock, flags);

	if (chan->mode == H616_PWM_MODE_CLK)
		ret = -EBUSY;
	else
		chan->mode = H616_PWM_MODE_PWM;

	spin_unlock_irqrestore(&h616chip->clk_pdata->lock, flags);
	if (ret)
		goto out;

	ret = clk_prepare_enable(chan->pwm_clk);
	if (ret < 0) {
		dev_err(dev, "Failed to enable clock %s: %d\n",
			__clk_get_name(chan->pwm_clk), ret);
	}
out:
	return ret;
}

static void h616_pwm_free(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct h616_pwm_chip *h616chip = to_h616_pwm_chip(chip);
	struct h616_pwm_channel *chan = &h616chip->channels[pwm->hwpwm];

	clk_disable_unprepare(chan->pwm_clk);
	chan->mode = H616_PWM_MODE_NONE;
}

static int h616_pwm_get_state(struct pwm_chip *chip,
			      struct pwm_device *pwm,
			      struct pwm_state *state)
{
	struct h616_pwm_chip *h616chip = to_h616_pwm_chip(chip);
	struct h616_pwm_channel *chan = &h616chip->channels[pwm->hwpwm];
	u64 clk_rate, tmp;
	u32 val;

	clk_rate = clk_get_rate(chan->pwm_clk);
	if (!clk_rate)
		return -EINVAL;

	val = h616_pwm_readl(h616chip, PWM_ENR);
	state->enabled = !!(PWM_ENABLE(pwm->hwpwm) & val);

	val = h616_pwm_readl(h616chip, PWM_XY_CLK_CR(PWM_PAIR_IDX(pwm->hwpwm)));
	if (val & BIT(PWM_XY_CLK_CR_BYPASS_BIT(pwm->hwpwm))) {
		/*
		 * When bypass is enabled, the PWM logic is inactive.
		 * The PWM_clock_src_xy is directly routed to PWM_clock_x
		 */
		state->period = DIV_ROUND_UP_ULL(NSEC_PER_SEC, clk_rate);
		state->duty_cycle = DIV_ROUND_UP_ULL(state->period, 2);
		state->polarity = PWM_POLARITY_NORMAL;
		return 0;
	}

	state->enabled &= !!(BIT(PWM_XY_CLK_CR_GATE_BIT) & val);

	val = h616_pwm_readl(h616chip, PWM_CTRL_REG(pwm->hwpwm));
	if (val & PWM_CTRL_ACTIVE_STATE)
		state->polarity = PWM_POLARITY_NORMAL;
	else
		state->polarity = PWM_POLARITY_INVERSED;

	val = h616_pwm_readl(h616chip, PWM_PERIOD_REG(pwm->hwpwm));

	tmp = NSEC_PER_SEC * PWM_REG_DUTY(val);
	state->duty_cycle = DIV_ROUND_CLOSEST_ULL(tmp, clk_rate);

	tmp = NSEC_PER_SEC * PWM_REG_PERIOD(val);
	state->period = DIV_ROUND_CLOSEST_ULL(tmp, clk_rate);

	return 0;
}

static int h616_pwm_calc(struct pwm_chip *chip, unsigned int idx,
			 const struct pwm_state *state)
{
	struct h616_pwm_chip *h616chip = to_h616_pwm_chip(chip);
	struct h616_pwm_channel *chan = &h616chip->channels[idx];
	unsigned int cnt, duty_cnt;
	unsigned long max_rate;
	long calc_rate;
	u64 duty, period, freq;

	duty = state->duty_cycle;
	period = state->period;

	max_rate = clk_round_rate(chan->pwm_clk, UINT32_MAX);

	dev_dbg(pwmchip_parent(chip), "max_rate: %ld Hz\n", max_rate);

	if ((period * max_rate >= NSEC_PER_SEC) &&
	    (period * max_rate < 2 * NSEC_PER_SEC) &&
	    (duty * max_rate * 2 >= NSEC_PER_SEC)) {
		/*
		 * If the requested period is to small to be generated by the
		 * PWM, we can just select the highest clock and bypass the
		 * PWM logic
		 */
		dev_dbg(pwmchip_parent(chip), "Setting bypass (period=%lld)\n",
			period);
		freq = div64_u64(NSEC_PER_SEC, period);
		chan->bypass = true;
		duty = period / 2;
	} else {
		chan->bypass = false;
		freq = div64_u64(NSEC_PER_SEC * (u64)PWM_PERIOD_MAX, period);
		if (freq > UINT32_MAX)
			freq = UINT32_MAX;
	}

	calc_rate = clk_round_rate(chan->pwm_clk, freq);
	if (calc_rate <= 0) {
		dev_err(pwmchip_parent(chip),
			"Invalid source clock frequency %llu\n", freq);
		return calc_rate ? calc_rate : -EINVAL;
	}

	dev_dbg(pwmchip_parent(chip), "calc_rate: %ld Hz\n", calc_rate);

	cnt = mul_u64_u64_div_u64(calc_rate, period, NSEC_PER_SEC);
	if ((cnt == 0) || (cnt > PWM_PERIOD_MAX)) {
		dev_err(pwmchip_parent(chip), "Period out of range\n");
		return -EINVAL;
	}

	duty_cnt = mul_u64_u64_div_u64(calc_rate, duty, NSEC_PER_SEC);

	if (duty_cnt >= cnt)
		duty_cnt = cnt - 1;

	dev_dbg(pwmchip_parent(chip), "period=%llu cnt=%u duty=%llu duty_cnt=%u\n",
		period, cnt, duty, duty_cnt);

	chan->active_cycles = duty_cnt;
	chan->entire_cycles = cnt;

	chan->rate = calc_rate;

	return 0;
}

static int h616_pwm_apply(struct pwm_chip *chip, struct pwm_device *pwm,
			  const struct pwm_state *state)
{
	struct h616_pwm_chip *h616chip = to_h616_pwm_chip(chip);
	struct h616_pwm_channel *chan = &h616chip->channels[pwm->hwpwm];
	struct pwm_state cstate;
	unsigned long flags;
	u32 val;
	int ret;

	ret = h616_pwm_calc(chip, pwm->hwpwm, state);
	if (ret) {
		dev_err(pwmchip_parent(chip), "period exceeds the maximum value\n");
		return ret;
	}

	pwm_get_state(pwm, &cstate);

	ret = clk_set_rate(chan->pwm_clk, chan->rate);
	if (ret) {
		dev_err(pwmchip_parent(chip), "failed to set PWM %d clock rate to %lu\n",
			pwm->hwpwm, chan->rate);
		return ret;
	}

	h616_pwm_set_bypass(h616chip, pwm->hwpwm, chan->bypass);

	/*
	 * If bypass is set, the PWM logic (polarity, duty) can't be applied
	 */

	if (chan->bypass && (state->polarity == PWM_POLARITY_INVERSED)) {
		dev_warn(pwmchip_parent(chip),
			 "Can't set inversed polarity with bypass enabled\n");
	} else {
		val = h616_pwm_readl(h616chip, PWM_CTRL_REG(pwm->hwpwm));
		val &= ~PWM_CTRL_ACTIVE_STATE;
		if (state->polarity == PWM_POLARITY_NORMAL)
			val |= PWM_CTRL_ACTIVE_STATE;
		h616_pwm_writel(h616chip, val, PWM_CTRL_REG(pwm->hwpwm));
	}

	if (chan->bypass && (state->duty_cycle * 2 != state->period)) {
		dev_warn(pwmchip_parent(chip),
			 "Can't set a duty cycle with bypass enabled\n");
	}

	if (!chan->bypass) {
		val = PWM_DUTY(chan->active_cycles);
		val |= PWM_PERIOD(chan->entire_cycles);
		h616_pwm_writel(h616chip, val, PWM_PERIOD_REG(pwm->hwpwm));
	}

	if (state->enabled == cstate.enabled)
		return 0;

	if (cstate.enabled) {
		unsigned long delay_us;

		/*
		 * We need a full period to elapse before
		 * disabling the channel.
		 */
		delay_us = DIV_ROUND_UP_ULL(cstate.period, NSEC_PER_USEC);
		fsleep(delay_us);
	}

	spin_lock_irqsave(&h616chip->clk_pdata->lock, flags);

	val = h616_pwm_readl(h616chip, PWM_ENR);
	if (state->enabled)
		val |= PWM_ENABLE(pwm->hwpwm);
	else
		val &= ~PWM_ENABLE(pwm->hwpwm);
	h616_pwm_writel(h616chip, val, PWM_ENR);

	spin_unlock_irqrestore(&h616chip->clk_pdata->lock, flags);

	return 0;
}

static const struct pwm_ops h616_pwm_ops = {
	.apply = h616_pwm_apply,
	.get_state = h616_pwm_get_state,
	.request = h616_pwm_request,
	.free = h616_pwm_free,
};

static struct clk_hw *h616_pwm_of_clk_get(struct of_phandle_args *clkspec,
					  void *data)
{
	struct h616_pwm_chip *h616chip = data;
	struct clk_hw_onecell_data *hw_data = h616chip->clk_pdata->hw_data;
	unsigned int idx = clkspec->args[0];
	struct h616_pwm_channel *chan;
	struct clk_hw *ret_clk = NULL;
	unsigned long flags;

	if (idx >= h616chip->data->npwm)
		return ERR_PTR(-EINVAL);

	chan = &h616chip->channels[idx];

	spin_lock_irqsave(&h616chip->clk_pdata->lock, flags);

	if (chan->mode == H616_PWM_MODE_PWM) {
		ret_clk = ERR_PTR(-EBUSY);
	} else {
		chan->mode = H616_PWM_MODE_CLK;
		ret_clk = hw_data->hws[CLK_BYPASS(h616chip, idx)];
	}
	spin_unlock_irqrestore(&h616chip->clk_pdata->lock, flags);

	if (IS_ERR(ret_clk))
		goto out;

	chan->bypass = true;
	h616_pwm_set_bypass(h616chip, idx, chan->bypass);
out:
	return ret_clk;
}

static int h616_add_composite_clk(struct clk_pwm_data *data,
				  void __iomem *reg, spinlock_t *lock,
				  struct device *dev, struct clk_hw **hw)
{
	const struct clk_ops *mux_ops = NULL, *gate_ops = NULL, *rate_ops = NULL;
	struct clk_hw *mux_hw = NULL, *gate_hw = NULL, *rate_hw = NULL;
	struct device_node *node = dev->of_node;

	if (data->mux_hw) {
		struct clk_mux *mux;

		mux_hw = data->mux_hw;
		mux = to_clk_mux(mux_hw);
		mux->lock = lock;
		mux_ops = mux_hw->init->ops;
		mux->reg = (u64)mux->reg + reg;
	}

	if (data->gate_hw) {
		struct clk_gate *gate;

		gate_hw = data->gate_hw;
		gate = to_clk_gate(gate_hw);
		gate->lock = lock;
		gate_ops = gate_hw->init->ops;
		gate->reg = (u64)gate->reg + reg;
	}

	if (data->rate_hw) {
		struct clk_divider *rate;

		rate_hw = data->rate_hw;
		rate = to_clk_divider(rate_hw);
		rate_ops = rate_hw->init->ops;
		rate->lock = lock;
		rate->reg = (u64)rate->reg + reg;

		if (rate->table) {
			const struct clk_div_table *clkt;
			int table_size = 0;

			for (clkt = rate->table; clkt->div; clkt++)
				table_size++;
			rate->width = order_base_2(table_size);
		}
	}

	/*
	 * Retrieve the parent clock names from DTS for pwm-clk-srcxy
	 */
	if (!data->parent_names) {
		data->num_parents = of_clk_get_parent_count(node);
		if (data->num_parents == 0)
			return -ENOENT;

		data->parent_names = devm_kzalloc(dev,
						  sizeof(*data->parent_names),
						  GFP_KERNEL);
		for (unsigned int i = 0; i < data->num_parents; i++)
			data->parent_names[i] = of_clk_get_parent_name(node, i);
	}

	*hw = clk_hw_register_composite(dev, data->name, data->parent_names,
					data->num_parents, mux_hw,
					mux_ops, rate_hw, rate_ops,
					gate_hw, gate_ops, data->flags);

	return PTR_ERR_OR_ZERO(*hw);
}

static int h616_pwm_init_clocks(struct platform_device *pdev,
				struct h616_pwm_chip *h616chip)
{
	struct clk_pwm_pdata *pdata;
	struct device *dev = &pdev->dev;
	int num_clocks = 0;
	int ret;

	pdata = devm_kzalloc(dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;

	while (pwmcc_data[num_clocks].name)
		num_clocks++;

	pdata->hw_data = devm_kzalloc(dev, struct_size(pdata->hw_data, hws, num_clocks),
				      GFP_KERNEL);
	if (!pdata->hw_data)
		return -ENOMEM;

	pdata->hw_data->num = num_clocks;
	pdata->reg = h616chip->base;

	spin_lock_init(&pdata->lock);

	for (int i = 0; i < num_clocks; i++) {
		struct clk_hw **hw = &pdata->hw_data->hws[i];

		ret = h616_add_composite_clk(&pwmcc_data[i], pdata->reg,
					     &pdata->lock, dev, hw);
		if (ret) {
			dev_err_probe(dev, ret,
				      "Failed to register hw clock %s\n",
				      pwmcc_data[i].name);
			for (i--; i >= 0; i--)
				clk_hw_unregister_composite(pdata->hw_data->hws[i]);
			return ret;
		}
	}

	h616chip->clk_pdata = pdata;

	return 0;
}

static int h616_pwm_probe(struct platform_device *pdev)
{
	const struct h616_pwm_data *data;
	struct device *dev = &pdev->dev;
	struct h616_pwm_chip *h616chip;
	struct pwm_chip *chip;
	int ret;

	data = of_device_get_match_data(dev);
	if (!data)
		return -ENODEV;

	chip = devm_pwmchip_alloc(dev, data->npwm, sizeof(*h616chip));
	if (IS_ERR(chip))
		return dev_err_probe(dev, PTR_ERR(chip),
				     "Failed to allocate pwmchip\n");

	h616chip = to_h616_pwm_chip(chip);
	h616chip->data = data;
	h616chip->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(h616chip->base))
		return dev_err_probe(dev, PTR_ERR(h616chip->base),
				     "Failed to get PWM base address\n");

	h616chip->bus_clk = devm_clk_get_enabled(dev, "bus");
	if (IS_ERR(h616chip->bus_clk))
		return dev_err_probe(dev, PTR_ERR(h616chip->bus_clk),
				     "Failed to get bus clock\n");

	h616chip->channels = devm_kmalloc_array(dev, data->npwm,
						sizeof(*(h616chip->channels)),
						GFP_KERNEL);
	if (!h616chip->channels)
		return dev_err_probe(dev, -ENOMEM,
				     "Failed to allocate %d channels array\n",
				     data->npwm);

	h616chip->rst = devm_reset_control_get_shared(dev, NULL);
	if (IS_ERR(h616chip->rst))
		return dev_err_probe(dev, PTR_ERR(h616chip->rst),
				     "Failed to get reset control\n");

	chip->ops = &h616_pwm_ops;

	ret = h616_pwm_init_clocks(pdev, h616chip);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to initialize clocks\n");

	for (unsigned int i = 0; i < data->npwm; i++) {
		struct h616_pwm_channel *chan = &h616chip->channels[i];
		struct clk_hw **hw = &h616chip->clk_pdata->hw_data->hws[i];

		chan->pwm_clk = devm_clk_hw_get_clk(dev, *hw, NULL);
		if (IS_ERR(chan->pwm_clk)) {
			ret = dev_err_probe(dev, PTR_ERR(chan->pwm_clk),
					    "Failed to register PWM clock %d\n", i);
			goto err_get_clk;
		}
		chan->mode = H616_PWM_MODE_NONE;
	}

	ret = devm_of_clk_add_hw_provider(dev, h616_pwm_of_clk_get, h616chip);
	if (ret) {
		dev_err_probe(dev, ret, "Failed to add HW clock provider\n");
		goto err_add_clk_provider;
	}

	/* Deassert reset */
	ret = reset_control_deassert(h616chip->rst);
	if (ret) {
		dev_err_probe(dev, ret, "Cannot deassert reset control\n");
		goto err_ctrl_deassert;
	}

	ret = pwmchip_add(chip);
	if (ret < 0) {
		dev_err_probe(dev, ret, "Failed to add PWM chip\n");
		goto err_pwm_add;
	}

	platform_set_drvdata(pdev, chip);

	return 0;

err_pwm_add:
	reset_control_assert(h616chip->rst);

err_ctrl_deassert:
err_add_clk_provider:
err_get_clk:
	for (int i = 0; i < h616chip->clk_pdata->hw_data->num; i++)
		clk_hw_unregister_composite(h616chip->clk_pdata->hw_data->hws[i]);

	return ret;
}

static const struct h616_pwm_data sun50i_h616_pwm_data = {
	.npwm = 6,
};

static const struct of_device_id h616_pwm_dt_ids[] = {
	{
		.compatible = "allwinner,sun50i-h616-pwm",
		.data = &sun50i_h616_pwm_data,
	}, {
		/* sentinel */
	},
};
MODULE_DEVICE_TABLE(of, h616_pwm_dt_ids);


static struct platform_driver h616_pwm_driver = {
	.driver = {
		.name = "h616-pwm",
		.of_match_table = h616_pwm_dt_ids,
	},
	.probe = h616_pwm_probe,
};
module_platform_driver(h616_pwm_driver);

MODULE_AUTHOR("Richard Genoud <richard.genoud@bootlin.com>");
MODULE_DESCRIPTION("Allwinner H616 PWM driver");
MODULE_LICENSE("GPL");
