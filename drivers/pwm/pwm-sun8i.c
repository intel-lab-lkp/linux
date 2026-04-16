// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for Allwinner sun8i Pulse Width Modulation Controller
 *
 * (C) Copyright 2025 Richard Genoud, Bootlin <richard.genoud@bootlin.com>
 *
 * Based on drivers/pwm/pwm-sun4i.c with Copyright:
 *
 * Copyright (C) 2014 Alexandre Belloni <alexandre.belloni@bootlin.com>
 *
 * Limitations:
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
#include <linux/limits.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/reset.h>
#include <linux/spinlock.h>
#include <linux/time.h>

/* PWM IRQ Enable Register */
#define SUN8I_PWM_PIER			0x0

/* PWM IRQ Status Register */
#define SUN8I_PWM_PISR			0x4

/* PWM Capture IRQ Enable Register */
#define SUN8I_PWM_CIER			0x10

/* PWM Capture IRQ Status Register */
#define SUN8I_PWM_CISR			0x14

/* PWMCC Pairs Clock Configuration Registers */
#define SUN8I_PWM_PCCR(pair)		(0x20 + ((pair) * 0x4))
#define SUN8I_PWM_PCCR_SRC_SHIFT	7
#define SUN8I_PWM_PCCR_SRC_MASK		1
#define SUN8I_PWM_PCCR_GATE_BIT		4
#define SUN8I_PWM_PCCR_BYPASS_BIT(chan)	((chan) % 2 + 5)
#define SUN8I_PWM_PCCR_DIV_M_SHIFT	0

/* PWMCC Pairs Dead Zone Control Registers */
#define SUN8I_PWM_PDZCR(pair)		(0x30 + ((pair) * 0x4))

/* PWM Enable Register */
#define SUN8I_PWM_PER			0x40
#define SUN8I_PWM_ENABLE(chan)		BIT(chan)

/* PWM Capture Enable Register */
#define SUN8I_PWM_CER			0x44

/* PWM Control Register */
#define SUN8I_PWM_PCR(chan)		(0x60 + (chan) * 0x20)
#define SUN8I_PWM_PCR_PRESCAL_K_SHIFT	0
#define SUN8I_PWM_PCR_PRESCAL_K_WIDTH	8
#define SUN8I_PWM_PCR_ACTIVE_STATE	BIT(8)

/* PWM Period Register */
#define SUN8I_PWM_PPR(chan)		(0x64 + (chan) * 0x20)
#define SUN8I_PWM_PPR_PERIOD_MASK	GENMASK(31, 16)
#define SUN8I_PWM_PPR_DUTY_MASK		GENMASK(15, 0)
#define SUN8I_PWM_PPR_PERIOD_VALUE(reg)	(FIELD_GET(SUN8I_PWM_PPR_PERIOD_MASK, reg) + 1)
#define SUN8I_PWM_PPR_DUTY_VALUE(reg)	FIELD_GET(SUN8I_PWM_PPR_DUTY_MASK, reg)
#define SUN8I_PWM_PPR_PERIOD(prd)	FIELD_PREP(SUN8I_PWM_PPR_PERIOD_MASK, (prd) - 1)
#define SUN8I_PWM_DUTY(dty)		FIELD_PREP(SUN8I_PWM_PPR_DUTY_MASK, dty)
#define SUN8I_PWM_PPR_PERIOD_MAX	(FIELD_MAX(SUN8I_PWM_PPR_PERIOD_MASK) + 1)

/* PWM Count Register */
#define SUN8I_PWM_PCNTR(chan)		(0x68 + (chan) * 0x20)

/* PWM Capture Control Register */
#define SUN8I_PWM_CCR(chan)		(0x6c + (chan) * 0x20)

/* PWM Capture Rise Lock Register */
#define SUN8I_PWM_CRLR(chan)		(0x70 + (chan) * 0x20)

/* PWM Capture Fall Lock Register */
#define SUN8I_PWM_CFLR(chan)		(0x74 + (chan) * 0x20)

#define SUN8I_PWM_PAIR_IDX(chan)	((chan) >> 1)

/*
 * Block diagram of the PWM clock controller:
 *
 *             _____      ______      ________
 * OSC24M --->|     |    |      |    |        |
 * APB1 ----->| Mux |--->| Gate |--->| /div_m |-----> SUN8I_PWM_clock_src_xy
 *            |_____|    |______|    |________|
 *                               ________
 *                              |        |
 *                           +->| /div_k |---> SUN8I_PWM_clock_x
 *                           |  |________|
 *                           |    ______
 *                           |   |      |
 *                           +-->| Gate |----> SUN8I_PWM_bypass_clock_x
 *                           |   |______|
 * SUN8I_PWM_clock_src_xy ---+   ________
 *                           |  |        |
 *                           +->| /div_k |---> SUN8I_PWM_clock_y
 *                           |  |________|
 *                           |    ______
 *                           |   |      |
 *                           +-->| Gate |----> SUN8I_PWM_bypass_clock_y
 *                               |______|
 *
 * NB: when the bypass is set, all the PWM logic is bypassed.
 * So, the duty cycle and polarity can't be modified (we just have a clock).
 * The bypass in PWM mode is used to achieve a 1/2 relative duty cycle with the
 * fastest clock.
 *
 * SUN8I_PWM_clock_x/y serve for the PWM purpose.
 * SUN8I_PWM_bypass_clock_x/y serve for the clock-provider purpose.
 *
 */

/*
 * Table used for /div_m (diviser before obtaining SUN8I_PWM_clock_src_xy)
 * It's actually CLK_DIVIDER_POWER_OF_TWO, but limited to /256
 */
#define CLK_TABLE_DIV_M_ENTRY(i) { \
	.val = (i), .div = 1 << (i) \
}

static const struct clk_div_table clk_table_div_m[] = {
	CLK_TABLE_DIV_M_ENTRY(0),
	CLK_TABLE_DIV_M_ENTRY(1),
	CLK_TABLE_DIV_M_ENTRY(2),
	CLK_TABLE_DIV_M_ENTRY(3),
	CLK_TABLE_DIV_M_ENTRY(4),
	CLK_TABLE_DIV_M_ENTRY(5),
	CLK_TABLE_DIV_M_ENTRY(6),
	CLK_TABLE_DIV_M_ENTRY(7),
	CLK_TABLE_DIV_M_ENTRY(8),
	{ /* sentinel */ }
};

#define SUN8I_PWM_XY_SRC_GATE(_pair, _reg)		\
struct clk_gate gate_xy_##_pair = {			\
	.reg = (void *)(_reg),				\
	.bit_idx = SUN8I_PWM_PCCR_GATE_BIT,		\
	.hw.init = &(struct clk_init_data){		\
		.ops = &clk_gate_ops,			\
	}						\
}

#define SUN8I_PWM_XY_SRC_MUX(_pair, _reg)		\
struct clk_mux mux_xy_##_pair = {			\
	.reg = (void *)(_reg),				\
	.shift = SUN8I_PWM_PCCR_SRC_SHIFT,		\
	.mask = SUN8I_PWM_PCCR_SRC_MASK,		\
	.flags = CLK_MUX_ROUND_CLOSEST,			\
	.hw.init = &(struct clk_init_data){		\
		.ops = &clk_mux_ops,			\
	}						\
}

#define SUN8I_PWM_XY_SRC_DIV(_pair, _reg)		\
struct clk_divider rate_xy_##_pair = {			\
	.reg = (void *)(_reg),				\
	.shift = SUN8I_PWM_PCCR_DIV_M_SHIFT,		\
	.table = clk_table_div_m,			\
	.hw.init = &(struct clk_init_data){		\
		.ops = &clk_divider_ops,		\
	}						\
}

#define SUN8I_PWM_X_DIV(_idx, _reg)			\
struct clk_divider rate_x_##_idx = {			\
	.reg = (void *)(_reg),				\
	.shift = SUN8I_PWM_PCR_PRESCAL_K_SHIFT,		\
	.width = SUN8I_PWM_PCR_PRESCAL_K_WIDTH,		\
	.hw.init = &(struct clk_init_data){		\
		.ops = &clk_divider_ops,		\
	}						\
}

#define SUN8I_PWM_X_BYPASS_GATE(_idx)			\
struct clk_gate gate_x_bypass_##_idx = {		\
	.reg = (void *)SUN8I_PWM_PER,			\
	.bit_idx = _idx,				\
	.hw.init = &(struct clk_init_data){		\
		.ops = &clk_gate_ops,			\
	}						\
}

#define SUN8I_PWM_XY_CLK_SRC(_pair, _reg)			\
	static SUN8I_PWM_XY_SRC_MUX(_pair, _reg);		\
	static SUN8I_PWM_XY_SRC_GATE(_pair, _reg);		\
	static SUN8I_PWM_XY_SRC_DIV(_pair, _reg)

#define SUN8I_PWM_X_CLK(_idx)					\
	static SUN8I_PWM_X_DIV(_idx, SUN8I_PWM_PCR(_idx))

#define SUN8I_PWM_X_BYPASS_CLK(_idx)				\
	SUN8I_PWM_X_BYPASS_GATE(_idx)

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
		.flags = CLK_SET_RATE_PARENT,				\
	}

/*
 * SUN8I_PWM_clock_src_xy generation:
 *             _____      ______      ________
 * OSC24M --->|     |    |      |    |        |
 * APB1 ----->| Mux |--->| Gate |--->| /div_m |-----> SUN8I_PWM_clock_src_xy
 *            |_____|    |______|    |________|
 */
SUN8I_PWM_XY_CLK_SRC(01, SUN8I_PWM_PCCR(0));
SUN8I_PWM_XY_CLK_SRC(23, SUN8I_PWM_PCCR(1));
SUN8I_PWM_XY_CLK_SRC(45, SUN8I_PWM_PCCR(2));

/*
 * SUN8I_PWM_clock_x_div generation:
 *                            ________
 *                           |        | SUN8I_PWM_clock_x/y
 * SUN8I_PWM_clock_src_xy -->| /div_k |--------------->
 *                           |________|
 */
SUN8I_PWM_X_CLK(0);
SUN8I_PWM_X_CLK(1);
SUN8I_PWM_X_CLK(2);
SUN8I_PWM_X_CLK(3);
SUN8I_PWM_X_CLK(4);
SUN8I_PWM_X_CLK(5);

/*
 * SUN8I_PWM_bypass_clock_xy generation:
 *                             ______
 *                            |      |
 * SUN8I_PWM_clock_src_xy --->| Gate |-------> SUN8I_PWM_bypass_clock_x
 *                            |______|
 *
 * The gate is actually SUN8I_PWM_PER register.
 */
SUN8I_PWM_X_BYPASS_CLK(0);
SUN8I_PWM_X_BYPASS_CLK(1);
SUN8I_PWM_X_BYPASS_CLK(2);
SUN8I_PWM_X_BYPASS_CLK(3);
SUN8I_PWM_X_BYPASS_CLK(4);
SUN8I_PWM_X_BYPASS_CLK(5);

struct clk_pwm_data {
	const char *name;
	const char **parent_names;
	unsigned int num_parents;
	struct clk_hw *mux_hw;
	struct clk_hw *rate_hw;
	struct clk_hw *gate_hw;
	unsigned long flags;
};

/* Indexes of REF_CLK_BYPASS and REF_CLK_XY_SRC in the array */
#define CLK_BYPASS_IDX(sun8i_chip, chan) ((sun8i_chip)->data->npwm + (chan))
#define CLK_XY_SRC_IDX(sun8i_chip, chan) \
	((sun8i_chip)->data->npwm * 2 + SUN8I_PWM_PAIR_IDX(chan))
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
	{ /* sentinel */ }
};

enum sun8i_pwm_mode {
	SUN8I_PWM_MODE_NONE,
	SUN8I_PWM_MODE_PWM,
	SUN8I_PWM_MODE_CLK,
};

struct sun8i_pwm_data {
	unsigned int npwm;
};

struct sun8i_pwm_channel {
	struct clk *pwm_clk;
	enum sun8i_pwm_mode mode;
};

struct clk_pwm_pdata {
	struct clk_hw_onecell_data *hw_data;
	spinlock_t lock;
	void __iomem *reg;
};

struct sun8i_pwm_chip {
	struct clk_pwm_pdata *clk_pdata;
	struct sun8i_pwm_channel *channels;
	struct clk *bus_clk;
	struct reset_control *rst;
	void __iomem *base;
	const struct sun8i_pwm_data *data;
};

struct sun8i_pwm_waveform {
	u8 enabled:1;
	u8 active_state:1;
	u8 bypass_en:1;
	u16 duty_ticks;
	u32 period_ticks;
	unsigned long clk_rate;
};

static inline struct sun8i_pwm_chip *sun8i_pwm_from_chip(const struct pwm_chip *chip)
{
	return pwmchip_get_drvdata(chip);
}

static inline u32 sun8i_pwm_readl(struct sun8i_pwm_chip *sun8i_chip,
				  unsigned long offset)
{
	return readl(sun8i_chip->base + offset);
}

static inline void sun8i_pwm_writel(struct sun8i_pwm_chip *sun8i_chip,
				    u32 val, unsigned long offset)
{
	writel(val, sun8i_chip->base + offset);
}

static void sun8i_pwm_set_bypass(struct sun8i_pwm_chip *sun8i_chip,
				 unsigned int idx, bool en_bypass)
{
	unsigned long flags, reg_offset;
	u32 val;

	spin_lock_irqsave(&sun8i_chip->clk_pdata->lock, flags);

	reg_offset = SUN8I_PWM_PCCR(SUN8I_PWM_PAIR_IDX(idx));
	val = sun8i_pwm_readl(sun8i_chip, reg_offset);
	if (en_bypass)
		val |= BIT(SUN8I_PWM_PCCR_BYPASS_BIT(idx));
	else
		val &= ~BIT(SUN8I_PWM_PCCR_BYPASS_BIT(idx));

	sun8i_pwm_writel(sun8i_chip, val, reg_offset);

	spin_unlock_irqrestore(&sun8i_chip->clk_pdata->lock, flags);
}

static int sun8i_pwm_request(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct sun8i_pwm_chip *sun8i_chip = sun8i_pwm_from_chip(chip);
	struct sun8i_pwm_channel *chan = &sun8i_chip->channels[pwm->hwpwm];

	scoped_guard(spinlock_irqsave, &sun8i_chip->clk_pdata->lock) {
		if (chan->mode == SUN8I_PWM_MODE_CLK)
			return -EBUSY;
		chan->mode = SUN8I_PWM_MODE_PWM;
	}

	return clk_prepare_enable(chan->pwm_clk);
}

static void sun8i_pwm_free(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct sun8i_pwm_chip *sun8i_chip = sun8i_pwm_from_chip(chip);
	struct sun8i_pwm_channel *chan = &sun8i_chip->channels[pwm->hwpwm];

	clk_disable_unprepare(chan->pwm_clk);
	chan->mode = SUN8I_PWM_MODE_NONE;
}

static int sun8i_pwm_read_waveform(struct pwm_chip *chip,
				   struct pwm_device *pwm,
				   void *_wfhw)
{
	struct sun8i_pwm_waveform *wfhw = _wfhw;
	struct sun8i_pwm_chip *sun8i_chip = sun8i_pwm_from_chip(chip);
	struct sun8i_pwm_channel *chan = &sun8i_chip->channels[pwm->hwpwm];
	u32 val;

	wfhw->clk_rate = clk_get_rate(chan->pwm_clk);

	val = sun8i_pwm_readl(sun8i_chip, SUN8I_PWM_PER);
	wfhw->enabled = !!(SUN8I_PWM_ENABLE(pwm->hwpwm) & val);

	val = sun8i_pwm_readl(sun8i_chip, SUN8I_PWM_PCCR(SUN8I_PWM_PAIR_IDX(pwm->hwpwm)));
	wfhw->bypass_en = !!(val & BIT(SUN8I_PWM_PCCR_BYPASS_BIT(pwm->hwpwm)));

	val = sun8i_pwm_readl(sun8i_chip, SUN8I_PWM_PCR(pwm->hwpwm));
	wfhw->active_state = !!(val & SUN8I_PWM_PCR_ACTIVE_STATE);

	val = sun8i_pwm_readl(sun8i_chip, SUN8I_PWM_PPR(pwm->hwpwm));
	wfhw->duty_ticks = SUN8I_PWM_PPR_DUTY_VALUE(val);
	wfhw->period_ticks = SUN8I_PWM_PPR_PERIOD_VALUE(val);

	dev_dbg(pwmchip_parent(chip),
		"pwm%u: %s, bypass: %s, polarity: %s, clk_rate=%lu period_ticks=%u duty_ticks=%u\n",
		pwm->hwpwm,
		wfhw->enabled ? "enabled" : "disabled",
		wfhw->bypass_en ? "enabled" : "disabled",
		wfhw->active_state ? "normal" : "inversed",
		wfhw->clk_rate, wfhw->period_ticks, wfhw->duty_ticks);

	return 0;
}

static int sun8i_pwm_round_waveform_fromhw(struct pwm_chip *chip,
					   struct pwm_device *pwm,
					   const void *_wfhw,
					   struct pwm_waveform *wf)
{
	const struct sun8i_pwm_waveform *wfhw = _wfhw;
	u64 tmp, resolution;

	dev_dbg(pwmchip_parent(chip),
		"pwm%u: %s, bypass: %s, polarity: %s, clk_rate=%lu period_ticks=%u duty_ticks=%u\n",
		pwm->hwpwm,
		wfhw->enabled ? "enabled" : "disabled",
		wfhw->bypass_en ? "enabled" : "disabled",
		wfhw->active_state ? "normal" : "inversed",
		wfhw->clk_rate, wfhw->period_ticks, wfhw->duty_ticks);

	wf->duty_offset_ns = 0;

	if (!wfhw->enabled || !wfhw->clk_rate) {
		wf->period_length_ns = 0;
		wf->duty_length_ns = 0;
		return 0;
	}

	if (wfhw->bypass_en) {
		wf->period_length_ns = DIV_ROUND_UP_ULL(NSEC_PER_SEC,
							wfhw->clk_rate);
		wf->duty_length_ns = DIV_ROUND_UP_ULL(wf->period_length_ns, 2);
		return 0;
	}

	tmp = NSEC_PER_SEC * (u64)wfhw->period_ticks;
	wf->period_length_ns = DIV_ROUND_UP_ULL(tmp, wfhw->clk_rate);

	tmp = NSEC_PER_SEC * (u64)wfhw->duty_ticks;
	wf->duty_length_ns = DIV_ROUND_UP_ULL(tmp, wfhw->clk_rate);
	if (!wfhw->active_state) {
		/*
		 * For inverted polarity, we have to fix cases where
		 * computed duty_length_ns > requested duty_length_ns
		 * For that, we subtract the actual resolution of the PWM
		 * registers
		 */
		wf->duty_offset_ns = wf->duty_length_ns;
		wf->duty_length_ns = wf->period_length_ns - wf->duty_length_ns;

		resolution = DIV_ROUND_UP_ULL(NSEC_PER_SEC, wfhw->clk_rate);

		if (wf->duty_offset_ns >= resolution)
			wf->duty_offset_ns -= resolution;
	}

	dev_dbg(pwmchip_parent(chip),
		"pwm%u period_length_ns=%llu duty_length_ns=%llu duty_offset_ns=%llu\n",
		pwm->hwpwm, wf->period_length_ns, wf->duty_length_ns,
		wf->duty_offset_ns);

	return 0;
}

static int sun8i_pwm_write_waveform(struct pwm_chip *chip,
				    struct pwm_device *pwm, const void *_wfhw)
{
	const struct sun8i_pwm_waveform *wfhw = _wfhw;
	struct sun8i_pwm_chip *sun8i_chip = sun8i_pwm_from_chip(chip);
	struct sun8i_pwm_channel *chan = &sun8i_chip->channels[pwm->hwpwm];
	unsigned long flags;
	u32 val;
	int ret;

	ret = clk_set_rate(chan->pwm_clk, wfhw->clk_rate);
	if (ret)
		return ret;

	sun8i_pwm_set_bypass(sun8i_chip, pwm->hwpwm, wfhw->bypass_en);

	val = sun8i_pwm_readl(sun8i_chip, SUN8I_PWM_PCR(pwm->hwpwm));
	if (wfhw->active_state)
		val |= SUN8I_PWM_PCR_ACTIVE_STATE;
	else
		val &= ~SUN8I_PWM_PCR_ACTIVE_STATE;
	sun8i_pwm_writel(sun8i_chip, val, SUN8I_PWM_PCR(pwm->hwpwm));

	val = SUN8I_PWM_DUTY(wfhw->duty_ticks);
	val |= SUN8I_PWM_PPR_PERIOD(wfhw->period_ticks);
	sun8i_pwm_writel(sun8i_chip, val, SUN8I_PWM_PPR(pwm->hwpwm));

	spin_lock_irqsave(&sun8i_chip->clk_pdata->lock, flags);

	val = sun8i_pwm_readl(sun8i_chip, SUN8I_PWM_PER);
	if (wfhw->enabled)
		val |= SUN8I_PWM_ENABLE(pwm->hwpwm);
	else
		val &= ~SUN8I_PWM_ENABLE(pwm->hwpwm);
	sun8i_pwm_writel(sun8i_chip, val, SUN8I_PWM_PER);

	spin_unlock_irqrestore(&sun8i_chip->clk_pdata->lock, flags);

	return 0;
}

static int sun8i_pwm_round_waveform_tohw(struct pwm_chip *chip,
					 struct pwm_device *pwm,
					 const struct pwm_waveform *wf,
					 void *_wfhw)
{
	struct sun8i_pwm_chip *sun8i_chip = sun8i_pwm_from_chip(chip);
	struct sun8i_pwm_channel *chan = &sun8i_chip->channels[pwm->hwpwm];
	struct sun8i_pwm_waveform *wfhw = _wfhw;
	unsigned long max_rate;
	long calc_rate;
	u64 period_ratio, double_duty_ratio, freq, duty_cycle;

	dev_dbg(pwmchip_parent(chip),
		"pwm%u period_length_ns=%llu duty_length_ns=%llu duty_offset_ns=%llu\n",
		pwm->hwpwm, wf->period_length_ns, wf->duty_length_ns,
		wf->duty_offset_ns);

	if (wf->period_length_ns == 0) {
		wfhw->enabled = 0;
		return 0;
	}

	wfhw->enabled = 1;

	duty_cycle = wf->duty_length_ns;
	if (wf->duty_length_ns + wf->duty_offset_ns < wf->period_length_ns)
		wfhw->active_state = 1;
	else
		wfhw->active_state = 0;

	dev_dbg(pwmchip_parent(chip), "polarity: %s\n",
		wfhw->active_state ? "normal" : "inversed");

	/*
	 * Lowest possible period case:
	 * Without bypass, the lowest possible period is when:
	 * duty cycle = 1 and period cycle = 2 (0x10001 in period register)
	 * E.g. if the input clock is 100MHz, we have a lowest period of 20ns.
	 * Now, with the bypass, the period register is ignored and we directly
	 * have the 100MHz clock as PWM output, that can act as a 10ns period
	 * with 5ns duty.
	 * So, to detect this lowest period case, just get the maximum possible
	 * rate from chan->pwm_clk and compare it with requested period and
	 * duty_cycle.
	 *
	 * But, to get the maximum possible rate, we have to use U32_MAX instead
	 * of (unsigned long)-1.
	 * This is because clk_round_rate() uses ultimately DIV_ROUND_UP_ULL()
	 * that in turn do_div(n,base). And base is uint32_t divisor.
	 */
	max_rate = clk_round_rate(chan->pwm_clk, U32_MAX);

	dev_dbg(pwmchip_parent(chip), "max_rate: %ld Hz\n", max_rate);

	period_ratio = mul_u64_u64_div_u64(wf->period_length_ns,
					   max_rate, NSEC_PER_SEC);
	double_duty_ratio = mul_u64_u64_div_u64(duty_cycle, (u64)max_rate * 2,
						NSEC_PER_SEC);
	if (period_ratio == 1) {
		if (double_duty_ratio == 0)
			/* requested period and duty are too small */
			return -EINVAL;
		/*
		 * If the requested period is to small to be generated by the
		 * PWM, but matches the highest clock with a
		 * duty_cycle >= period*2, just bypass the PWM logic
		 */
		freq = div64_u64(NSEC_PER_SEC, wf->period_length_ns);
		wfhw->bypass_en = true;
	} else {
		wfhw->bypass_en = false;
		freq = div64_u64(NSEC_PER_SEC * (u64)SUN8I_PWM_PPR_PERIOD_MAX,
				 wf->period_length_ns);
		/*
		 * Same remark as above, this is to prevent a value to big for
		 * clk_round_rate() to handle
		 */
		if (freq > U32_MAX)
			freq = U32_MAX;
	}

	dev_dbg(pwmchip_parent(chip), "bypass: %s\n",
		wfhw->bypass_en ? "enabled" : "disabled");

	calc_rate = clk_round_rate(chan->pwm_clk, freq);
	if (calc_rate <= 0)
		return calc_rate ? calc_rate : -EINVAL;

	dev_dbg(pwmchip_parent(chip), "calc_rate: %ld Hz\n", calc_rate);

	wfhw->period_ticks = mul_u64_u64_div_u64(calc_rate,
						 wf->period_length_ns,
						 NSEC_PER_SEC);
	if (wfhw->period_ticks > SUN8I_PWM_PPR_PERIOD_MAX)
		wfhw->period_ticks = SUN8I_PWM_PPR_PERIOD_MAX;

	/* min value in period register is 1 */
	if (wfhw->period_ticks == 0)
		return -EINVAL;

	wfhw->duty_ticks = mul_u64_u64_div_u64(calc_rate, duty_cycle,
					       NSEC_PER_SEC);

	if (wfhw->duty_ticks > wfhw->period_ticks)
		wfhw->duty_ticks = wfhw->period_ticks;

	if (!wfhw->active_state)
		wfhw->duty_ticks = wfhw->period_ticks - wfhw->duty_ticks;

	dev_dbg(pwmchip_parent(chip),
		"pwm%u period_ticks=%u duty_cycle=%llu duty_ticks=%u\n",
		pwm->hwpwm, wfhw->period_ticks, duty_cycle, wfhw->duty_ticks);

	wfhw->clk_rate = calc_rate;

	return 0;
}

static const struct pwm_ops sun8i_pwm_ops = {
	.request = sun8i_pwm_request,
	.free = sun8i_pwm_free,
	.sizeof_wfhw = sizeof(struct sun8i_pwm_waveform),
	.round_waveform_tohw = sun8i_pwm_round_waveform_tohw,
	.round_waveform_fromhw = sun8i_pwm_round_waveform_fromhw,
	.read_waveform = sun8i_pwm_read_waveform,
	.write_waveform = sun8i_pwm_write_waveform,
};

static struct clk_hw *sun8i_pwm_of_clk_get(struct of_phandle_args *clkspec,
					   void *data)
{
	struct sun8i_pwm_chip *sun8i_chip = data;
	struct clk_hw_onecell_data *hw_data = sun8i_chip->clk_pdata->hw_data;
	unsigned int idx = clkspec->args[0];
	struct sun8i_pwm_channel *chan;
	struct clk_hw *ret_clk = NULL;
	unsigned long flags;

	if (idx >= sun8i_chip->data->npwm)
		return ERR_PTR(-EINVAL);

	chan = &sun8i_chip->channels[idx];

	spin_lock_irqsave(&sun8i_chip->clk_pdata->lock, flags);

	if (chan->mode == SUN8I_PWM_MODE_PWM) {
		ret_clk = ERR_PTR(-EBUSY);
	} else {
		chan->mode = SUN8I_PWM_MODE_CLK;
		ret_clk = hw_data->hws[CLK_BYPASS_IDX(sun8i_chip, idx)];
	}
	spin_unlock_irqrestore(&sun8i_chip->clk_pdata->lock, flags);

	if (IS_ERR(ret_clk))
		goto out;

	sun8i_pwm_set_bypass(sun8i_chip, idx, true);
out:
	return ret_clk;
}

static int sun8i_add_composite_clk(struct clk_pwm_data *data,
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
		mux->reg = (uintptr_t)mux->reg + reg;
	}

	if (data->gate_hw) {
		struct clk_gate *gate;

		gate_hw = data->gate_hw;
		gate = to_clk_gate(gate_hw);
		gate->lock = lock;
		gate_ops = gate_hw->init->ops;
		gate->reg = (uintptr_t)gate->reg + reg;
	}

	if (data->rate_hw) {
		struct clk_divider *rate;

		rate_hw = data->rate_hw;
		rate = to_clk_divider(rate_hw);
		rate_ops = rate_hw->init->ops;
		rate->lock = lock;
		rate->reg = (uintptr_t)rate->reg + reg;

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

static int sun8i_pwm_init_clocks(struct platform_device *pdev,
				 struct sun8i_pwm_chip *sun8i_chip)
{
	struct clk_pwm_pdata *pdata;
	struct device *dev = &pdev->dev;
	int num_clocks = 0;
	int ret;

	pdata = devm_kzalloc(dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return dev_err_probe(dev, -ENOMEM,
				     "Failed to allocate clk_pwm_pdata\n");

	while (pwmcc_data[num_clocks].name)
		num_clocks++;

	pdata->hw_data = devm_kzalloc(dev, struct_size(pdata->hw_data, hws, num_clocks),
				      GFP_KERNEL);
	if (!pdata->hw_data)
		return dev_err_probe(dev, -ENOMEM,
				     "Failed to allocate hw clocks\n");

	pdata->hw_data->num = num_clocks;
	pdata->reg = sun8i_chip->base;

	spin_lock_init(&pdata->lock);

	for (int i = 0; i < num_clocks; i++) {
		struct clk_hw **hw = &pdata->hw_data->hws[i];

		ret = sun8i_add_composite_clk(&pwmcc_data[i], pdata->reg,
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

	sun8i_chip->clk_pdata = pdata;

	return 0;
}

static void sun8i_pwm_unregister_clk(void *data)
{
	struct clk_hw_onecell_data *hw_data = data;

	for (unsigned int i = 0; i < hw_data->num; i++)
		clk_hw_unregister_composite(hw_data->hws[i]);
}

static int sun8i_pwm_probe(struct platform_device *pdev)
{
	const struct sun8i_pwm_data *data;
	struct device *dev = &pdev->dev;
	struct sun8i_pwm_chip *sun8i_chip;
	struct pwm_chip *chip;
	int ret;

	data = of_device_get_match_data(dev);
	if (!data)
		return dev_err_probe(dev, -ENODEV,
				     "Missing specific data structure\n");

	chip = devm_pwmchip_alloc(dev, data->npwm, sizeof(*sun8i_chip));
	if (IS_ERR(chip))
		return dev_err_probe(dev, PTR_ERR(chip),
				     "Failed to allocate pwmchip\n");

	sun8i_chip = sun8i_pwm_from_chip(chip);
	sun8i_chip->data = data;
	sun8i_chip->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(sun8i_chip->base))
		return dev_err_probe(dev, PTR_ERR(sun8i_chip->base),
				     "Failed to get PWM base address\n");

	sun8i_chip->bus_clk = devm_clk_get_enabled(dev, "bus");
	if (IS_ERR(sun8i_chip->bus_clk))
		return dev_err_probe(dev, PTR_ERR(sun8i_chip->bus_clk),
				     "Failed to get bus clock\n");

	sun8i_chip->channels = devm_kmalloc_array(dev, data->npwm,
						  sizeof(*(sun8i_chip->channels)),
						  GFP_KERNEL);
	if (!sun8i_chip->channels)
		return dev_err_probe(dev, -ENOMEM,
				     "Failed to allocate %d channels array\n",
				     data->npwm);

	chip->ops = &sun8i_pwm_ops;

	ret = sun8i_pwm_init_clocks(pdev, sun8i_chip);
	if (ret)
		return ret;

	for (unsigned int i = 0; i < data->npwm; i++) {
		struct sun8i_pwm_channel *chan = &sun8i_chip->channels[i];
		struct clk_hw **hw = &sun8i_chip->clk_pdata->hw_data->hws[i];

		chan->pwm_clk = devm_clk_hw_get_clk(dev, *hw, NULL);
		if (IS_ERR(chan->pwm_clk)) {
			ret = dev_err_probe(dev, PTR_ERR(chan->pwm_clk),
					    "Failed to register PWM clock %d\n", i);
			return ret;
		}
		chan->mode = SUN8I_PWM_MODE_NONE;
	}

	ret = devm_of_clk_add_hw_provider(dev, sun8i_pwm_of_clk_get, sun8i_chip);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to add HW clock provider\n");

	ret = devm_add_action_or_reset(dev, sun8i_pwm_unregister_clk,
				       sun8i_chip->clk_pdata->hw_data);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to add devm action\n");

	/* Deassert reset */
	sun8i_chip->rst = devm_reset_control_get_shared_deasserted(dev, NULL);
	if (IS_ERR(sun8i_chip->rst))
		return dev_err_probe(dev, PTR_ERR(sun8i_chip->rst),
				     "Failed to get reset control\n");

	ret = devm_pwmchip_add(dev, chip);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to add PWM chip\n");

	platform_set_drvdata(pdev, chip);

	return 0;
}

static const struct sun8i_pwm_data sun50i_h616_pwm_data = {
	.npwm = 6,
};

static const struct of_device_id sun8i_pwm_dt_ids[] = {
	{
		.compatible = "allwinner,sun50i-h616-pwm",
		.data = &sun50i_h616_pwm_data,
	}, {
		/* sentinel */
	}
};
MODULE_DEVICE_TABLE(of, sun8i_pwm_dt_ids);

static struct platform_driver sun8i_pwm_driver = {
	.driver = {
		.name = "sun8i-pwm",
		.of_match_table = sun8i_pwm_dt_ids,
	},
	.probe = sun8i_pwm_probe,
};
module_platform_driver(sun8i_pwm_driver);

MODULE_AUTHOR("Richard Genoud <richard.genoud@bootlin.com>");
MODULE_DESCRIPTION("Allwinner sun8i PWM driver");
MODULE_LICENSE("GPL");
