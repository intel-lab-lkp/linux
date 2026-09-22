// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for Allwinner sun8i Pulse Width Modulation Controller
 *
 * (C) Copyright 2025 Richard Genoud, Bootlin <richard.genoud@bootlin.com>
 * (C) Copyright 2026 James Hilliard <james.hilliard1@gmail.com>
 *
 * Based on drivers/pwm/pwm-sun4i.c with Copyright:
 *
 * Copyright (C) 2014 Alexandre Belloni <alexandre.belloni@bootlin.com>
 *
 * Channels are paired (0/1, 2/3, 4/5). Each pair shares a clock source and
 * first prescaler (div_m), while each channel has its own second prescaler
 * (div_k) and bypass path.
 *
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/cleanup.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/limits.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/reset.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/time.h>

#define SUN8I_PWM_NPWM			6
#define SUN8I_PWM_NPAIRS			(SUN8I_PWM_NPWM / 2)

/* PWMCC Pairs Clock Configuration Registers */
#define SUN8I_PWM_PCCR(pair)		(0x20 + ((pair) * 0x4))
#define SUN8I_PWM_PCCR_SRC_SHIFT	7
#define SUN8I_PWM_PCCR_SRC_MASK		GENMASK(1, 0)
#define SUN8I_PWM_PCCR_GATE_BIT		4
#define SUN8I_PWM_PCCR_GATE		BIT(SUN8I_PWM_PCCR_GATE_BIT)
#define SUN8I_PWM_PCCR_BYPASS_BIT(chan)	((chan) % 2 + 5)
#define SUN8I_PWM_PCCR_DIV_M_SHIFT	0
#define SUN8I_PWM_PCCR_DIV_M_WIDTH	4

/* PWM Enable Register */
#define SUN8I_PWM_PER			0x40
#define SUN8I_PWM_ENABLE(chan)		BIT(chan)

/* PWM Control Register */
#define SUN8I_PWM_PCR(chan)		(0x60 + (chan) * 0x20)
#define SUN8I_PWM_PCR_PRESCAL_K_MASK	GENMASK(7, 0)
#define SUN8I_PWM_PCR_ACTIVE_STATE	BIT(8)
#define SUN8I_PWM_PCR_MODE		BIT(9)
#define SUN8I_PWM_PCR_PULSE_START	BIT(10)
#define SUN8I_PWM_PCR_PERIOD_READY	BIT(11) /* 0: ready, 1: busy */
#define SUN8I_PWM_PCR_WAVEFORM_MASK	(SUN8I_PWM_PCR_PRESCAL_K_MASK | \
					 SUN8I_PWM_PCR_ACTIVE_STATE)

/* PWM Period Register */
#define SUN8I_PWM_PPR(chan)		(0x64 + (chan) * 0x20)
#define SUN8I_PWM_PPR_PERIOD_MASK	GENMASK(31, 16)
#define SUN8I_PWM_PPR_DUTY_MASK		GENMASK(15, 0)
#define SUN8I_PWM_PPR_PERIOD_VALUE(reg)	(FIELD_GET(SUN8I_PWM_PPR_PERIOD_MASK, reg) + 1)
#define SUN8I_PWM_PPR_DUTY_VALUE(reg)	FIELD_GET(SUN8I_PWM_PPR_DUTY_MASK, reg)
#define SUN8I_PWM_PPR_PERIOD(prd)	FIELD_PREP(SUN8I_PWM_PPR_PERIOD_MASK, (prd) - 1)
#define SUN8I_PWM_PPR_DUTY(dty)		FIELD_PREP(SUN8I_PWM_PPR_DUTY_MASK, dty)
#define SUN8I_PWM_PPR_PERIOD_MAX	(FIELD_MAX(SUN8I_PWM_PPR_PERIOD_MASK) + 1)

/* PWM pair dead-zone control registers. */
#define SUN8I_PWM_PDZCR(pair)		(0x30 + ((pair) * 0x4))
#define SUN8I_PWM_PDZCR_ENABLE		BIT(0)

#define SUN8I_PWM_PERIOD_READY_MARGIN_US	50
#define SUN8I_PWM_PERIOD_READY_MIN_POLL_US	10
#define SUN8I_PWM_PERIOD_READY_MAX_POLL_US	10000
#define SUN8I_PWM_PERIOD_READY_POLL_COUNT	1024

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

/* /div_m is a power-of-two divider limited to /256. */
static const struct clk_div_table sun8i_pwm_div_m_table[] = {
	{ .val = 0, .div = 1 },
	{ .val = 1, .div = 2 },
	{ .val = 2, .div = 4 },
	{ .val = 3, .div = 8 },
	{ .val = 4, .div = 16 },
	{ .val = 5, .div = 32 },
	{ .val = 6, .div = 64 },
	{ .val = 7, .div = 128 },
	{ .val = 8, .div = 256 },
	{ /* sentinel */ }
};

enum sun8i_pwm_mode {
	SUN8I_PWM_MODE_NONE,
	SUN8I_PWM_MODE_PWM,
	SUN8I_PWM_MODE_CLK,
};

struct sun8i_pwm_chip;

struct sun8i_pwm_pair {
	struct clk_mux mux;
	struct clk_hw gate_hw;
	struct clk_divider divider;
	struct clk_hw *hw;
	struct notifier_block rate_nb;
	struct sun8i_pwm_chip *chip;
	unsigned int index;
};

struct sun8i_pwm_channel {
	struct clk_hw bypass_hw;
	struct sun8i_pwm_chip *chip;
	unsigned int index;
	/* Separate CCF consumer, held only from PWM request to free. */
	struct clk *pair_clk;
	enum sun8i_pwm_mode mode;
	u64 pending_period_ns;
	bool rate_exclusive;
};

struct sun8i_pwm_chip {
	struct sun8i_pwm_pair pairs[SUN8I_PWM_NPAIRS];
	struct sun8i_pwm_channel channels[SUN8I_PWM_NPWM];
	struct clk *bus_clk;
	void __iomem *base;
	/* Protects shared registers, channel ownership and rate_exclusive. */
	spinlock_t lock;
};

struct sun8i_pwm_waveform {
	u32 pcr;
	u32 ppr;
	u32 pair_rate;
	bool enabled;
	bool bypass_en;
};

static inline struct sun8i_pwm_chip *sun8i_pwm_from_chip(const struct pwm_chip *chip)
{
	return pwmchip_get_drvdata(chip);
}

static unsigned long sun8i_pwm_get_pair_rate(struct sun8i_pwm_chip *sun8i_chip,
					     unsigned int idx)
{
	struct sun8i_pwm_pair *pair = &sun8i_chip->pairs[SUN8I_PWM_PAIR_IDX(idx)];

	/* Readback also runs on channels without a PWM request. */
	return clk_get_rate(pair->hw->clk);
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

static void sun8i_pwm_set_bypass_locked(struct sun8i_pwm_chip *sun8i_chip,
					unsigned int idx, bool enable)
{
	unsigned long reg_offset;
	u32 val;

	lockdep_assert_held(&sun8i_chip->lock);

	reg_offset = SUN8I_PWM_PCCR(SUN8I_PWM_PAIR_IDX(idx));
	val = sun8i_pwm_readl(sun8i_chip, reg_offset);
	if (enable)
		val |= BIT(SUN8I_PWM_PCCR_BYPASS_BIT(idx));
	else
		val &= ~BIT(SUN8I_PWM_PCCR_BYPASS_BIT(idx));

	sun8i_pwm_writel(sun8i_chip, val, reg_offset);
}

static void sun8i_pwm_set_enabled_locked(struct sun8i_pwm_chip *sun8i_chip,
					 unsigned int idx, bool enable)
{
	u32 val;

	lockdep_assert_held(&sun8i_chip->lock);

	val = sun8i_pwm_readl(sun8i_chip, SUN8I_PWM_PER);
	if (enable)
		val |= SUN8I_PWM_ENABLE(idx);
	else
		val &= ~SUN8I_PWM_ENABLE(idx);
	sun8i_pwm_writel(sun8i_chip, val, SUN8I_PWM_PER);
}

static bool
sun8i_pwm_channel_is_enabled_locked(struct sun8i_pwm_chip *sun8i_chip,
				    unsigned int idx)
{
	unsigned int pair = SUN8I_PWM_PAIR_IDX(idx);
	u32 pccr, per;

	lockdep_assert_held(&sun8i_chip->lock);

	pccr = sun8i_pwm_readl(sun8i_chip, SUN8I_PWM_PCCR(pair));
	per = sun8i_pwm_readl(sun8i_chip, SUN8I_PWM_PER);

	return (pccr & SUN8I_PWM_PCCR_GATE) &&
	       (per & SUN8I_PWM_ENABLE(idx));
}

static u32 sun8i_pwm_pair_enable_mask(unsigned int pair)
{
	return GENMASK(pair * 2 + 1, pair * 2);
}

static inline struct sun8i_pwm_pair *
sun8i_pwm_pair_from_gate_hw(struct clk_hw *hw)
{
	return container_of(hw, struct sun8i_pwm_pair, gate_hw);
}

static int sun8i_pwm_pair_gate_enable(struct clk_hw *hw)
{
	struct sun8i_pwm_pair *pair = sun8i_pwm_pair_from_gate_hw(hw);
	struct sun8i_pwm_chip *sun8i_chip = pair->chip;
	u32 pccr;

	guard(spinlock_irqsave)(&sun8i_chip->lock);
	pccr = sun8i_pwm_readl(sun8i_chip, SUN8I_PWM_PCCR(pair->index));
	pccr |= SUN8I_PWM_PCCR_GATE;
	sun8i_pwm_writel(sun8i_chip, pccr, SUN8I_PWM_PCCR(pair->index));

	return 0;
}

static void sun8i_pwm_pair_gate_disable(struct clk_hw *hw)
{
	struct sun8i_pwm_pair *pair = sun8i_pwm_pair_from_gate_hw(hw);
	struct sun8i_pwm_chip *sun8i_chip = pair->chip;
	unsigned long reg = SUN8I_PWM_PCCR(pair->index);
	u32 pccr, per;

	/* CCF counts do not include outputs awaiting firmware-state handoff. */
	guard(spinlock_irqsave)(&sun8i_chip->lock);
	per = sun8i_pwm_readl(sun8i_chip, SUN8I_PWM_PER);
	if (!(per & sun8i_pwm_pair_enable_mask(pair->index))) {
		pccr = sun8i_pwm_readl(sun8i_chip, reg);
		pccr &= ~SUN8I_PWM_PCCR_GATE;
		sun8i_pwm_writel(sun8i_chip, pccr, reg);
	}
}

static int sun8i_pwm_pair_gate_is_enabled(struct clk_hw *hw)
{
	struct sun8i_pwm_pair *pair = sun8i_pwm_pair_from_gate_hw(hw);
	struct sun8i_pwm_chip *sun8i_chip = pair->chip;
	unsigned long reg = SUN8I_PWM_PCCR(pair->index);

	guard(spinlock_irqsave)(&sun8i_chip->lock);

	return !!(sun8i_pwm_readl(sun8i_chip, reg) & SUN8I_PWM_PCCR_GATE);
}

static const struct clk_ops sun8i_pwm_pair_gate_ops = {
	.enable = sun8i_pwm_pair_gate_enable,
	.disable = sun8i_pwm_pair_gate_disable,
	.is_enabled = sun8i_pwm_pair_gate_is_enabled,
};

static int sun8i_pwm_pair_rate_notifier(struct notifier_block *nb,
					unsigned long event, void *data)
{
	struct sun8i_pwm_pair *pair =
		container_of(nb, struct sun8i_pwm_pair, rate_nb);
	struct sun8i_pwm_chip *sun8i_chip = pair->chip;
	bool active;

	if (event != PRE_RATE_CHANGE)
		return NOTIFY_DONE;

	guard(spinlock_irqsave)(&sun8i_chip->lock);
	active = sun8i_pwm_readl(sun8i_chip, SUN8I_PWM_PER) &
		 sun8i_pwm_pair_enable_mask(pair->index);

	return active ? NOTIFY_BAD : NOTIFY_OK;
}

static inline struct sun8i_pwm_channel *
sun8i_pwm_channel_from_hw(struct clk_hw *hw)
{
	return container_of(hw, struct sun8i_pwm_channel, bypass_hw);
}

static int sun8i_pwm_bypass_prepare(struct clk_hw *hw)
{
	struct sun8i_pwm_channel *chan = sun8i_pwm_channel_from_hw(hw);
	struct sun8i_pwm_chip *sun8i_chip = chan->chip;

	guard(spinlock_irqsave)(&sun8i_chip->lock);
	if (chan->mode != SUN8I_PWM_MODE_NONE)
		return -EBUSY;

	chan->mode = SUN8I_PWM_MODE_CLK;
	return 0;
}

static void sun8i_pwm_bypass_unprepare(struct clk_hw *hw)
{
	struct sun8i_pwm_channel *chan = sun8i_pwm_channel_from_hw(hw);
	struct sun8i_pwm_chip *sun8i_chip = chan->chip;

	guard(spinlock_irqsave)(&sun8i_chip->lock);
	if (chan->mode != SUN8I_PWM_MODE_CLK)
		return;

	chan->mode = SUN8I_PWM_MODE_NONE;
}

static int sun8i_pwm_bypass_enable(struct clk_hw *hw)
{
	struct sun8i_pwm_channel *chan = sun8i_pwm_channel_from_hw(hw);
	struct sun8i_pwm_chip *sun8i_chip = chan->chip;
	unsigned int pair = SUN8I_PWM_PAIR_IDX(chan->index);
	u32 pccr, per;

	guard(spinlock_irqsave)(&sun8i_chip->lock);
	if (chan->mode != SUN8I_PWM_MODE_CLK)
		return -EBUSY;

	pccr = sun8i_pwm_readl(sun8i_chip, SUN8I_PWM_PCCR(pair));
	per = sun8i_pwm_readl(sun8i_chip, SUN8I_PWM_PER);
	if (!(pccr & BIT(SUN8I_PWM_PCCR_BYPASS_BIT(chan->index))) ||
	    !(per & SUN8I_PWM_ENABLE(chan->index))) {
		sun8i_pwm_set_enabled_locked(sun8i_chip, chan->index, false);
		sun8i_pwm_set_bypass_locked(sun8i_chip, chan->index, true);
		sun8i_pwm_set_enabled_locked(sun8i_chip, chan->index, true);
	}

	return 0;
}

static void sun8i_pwm_bypass_disable(struct clk_hw *hw)
{
	struct sun8i_pwm_channel *chan = sun8i_pwm_channel_from_hw(hw);
	struct sun8i_pwm_chip *sun8i_chip = chan->chip;

	guard(spinlock_irqsave)(&sun8i_chip->lock);
	if (chan->mode != SUN8I_PWM_MODE_CLK)
		return;

	sun8i_pwm_set_enabled_locked(sun8i_chip, chan->index, false);
	sun8i_pwm_set_bypass_locked(sun8i_chip, chan->index, false);
}

static int sun8i_pwm_bypass_is_enabled(struct clk_hw *hw)
{
	struct sun8i_pwm_channel *chan = sun8i_pwm_channel_from_hw(hw);
	struct sun8i_pwm_chip *sun8i_chip = chan->chip;
	unsigned int pair = SUN8I_PWM_PAIR_IDX(chan->index);
	bool enabled;
	u32 val;

	guard(spinlock_irqsave)(&sun8i_chip->lock);

	val = sun8i_pwm_readl(sun8i_chip, SUN8I_PWM_PCCR(pair));
	enabled = (val & SUN8I_PWM_PCCR_GATE) &&
		  (val & BIT(SUN8I_PWM_PCCR_BYPASS_BIT(chan->index)));

	val = sun8i_pwm_readl(sun8i_chip, SUN8I_PWM_PER);
	enabled = enabled && (val & SUN8I_PWM_ENABLE(chan->index));

	return enabled;
}

static const struct clk_ops sun8i_pwm_bypass_ops = {
	.prepare = sun8i_pwm_bypass_prepare,
	.unprepare = sun8i_pwm_bypass_unprepare,
	.enable = sun8i_pwm_bypass_enable,
	.disable = sun8i_pwm_bypass_disable,
	.is_enabled = sun8i_pwm_bypass_is_enabled,
};

static void sun8i_pwm_put_rate(struct sun8i_pwm_chip *sun8i_chip,
			       unsigned int idx)
{
	struct sun8i_pwm_channel *chan = &sun8i_chip->channels[idx];
	bool exclusive;

	scoped_guard(spinlock_irqsave, &sun8i_chip->lock) {
		exclusive = chan->rate_exclusive;
		chan->rate_exclusive = false;
	}
	/* CCF takes the prepare mutex and may call back into this driver. */
	if (exclusive)
		clk_rate_exclusive_put(chan->pair_clk);
}

static int sun8i_pwm_request(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct sun8i_pwm_chip *sun8i_chip = sun8i_pwm_from_chip(chip);
	struct sun8i_pwm_channel *chan = &sun8i_chip->channels[pwm->hwpwm];
	unsigned int idx = pwm->hwpwm;
	struct clk_hw *parent = sun8i_chip->pairs[SUN8I_PWM_PAIR_IDX(idx)].hw;
	bool was_enabled;
	int ret;

	scoped_guard(spinlock_irqsave, &sun8i_chip->lock) {
		if (chan->mode != SUN8I_PWM_MODE_NONE)
			return -EBUSY;

		was_enabled =
			sun8i_pwm_channel_is_enabled_locked(sun8i_chip, idx);
		chan->mode = SUN8I_PWM_MODE_PWM;
	}

	chan->pair_clk = clk_hw_get_clk(parent, NULL);
	if (IS_ERR(chan->pair_clk)) {
		ret = PTR_ERR(chan->pair_clk);
		goto err_clear_clock;
	}

	if (was_enabled) {
		ret = clk_rate_exclusive_get(chan->pair_clk);
		if (ret)
			goto err_put_clock;
		scoped_guard(spinlock_irqsave, &sun8i_chip->lock)
			chan->rate_exclusive = true;
	}

	ret = clk_prepare_enable(chan->pair_clk);
	if (ret) {
		sun8i_pwm_put_rate(sun8i_chip, idx);
		goto err_put_clock;
	}

	return 0;

err_put_clock:
	clk_put(chan->pair_clk);
err_clear_clock:
	chan->pair_clk = NULL;
	scoped_guard(spinlock_irqsave, &sun8i_chip->lock)
		chan->mode = SUN8I_PWM_MODE_NONE;

	return ret;
}

static void sun8i_pwm_free(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct sun8i_pwm_chip *sun8i_chip = sun8i_pwm_from_chip(chip);
	struct sun8i_pwm_channel *chan = &sun8i_chip->channels[pwm->hwpwm];

	scoped_guard(spinlock_irqsave, &sun8i_chip->lock) {
		if (chan->mode != SUN8I_PWM_MODE_PWM)
			return;

		sun8i_pwm_set_enabled_locked(sun8i_chip, pwm->hwpwm, false);
		sun8i_pwm_set_bypass_locked(sun8i_chip, pwm->hwpwm, false);
	}

	sun8i_pwm_put_rate(sun8i_chip, pwm->hwpwm);
	clk_disable_unprepare(chan->pair_clk);
	clk_put(chan->pair_clk);
	chan->pair_clk = NULL;

	scoped_guard(spinlock_irqsave, &sun8i_chip->lock)
		chan->mode = SUN8I_PWM_MODE_NONE;
}

static int sun8i_pwm_read_waveform(struct pwm_chip *chip,
				   struct pwm_device *pwm,
				   void *_wfhw)
{
	struct sun8i_pwm_waveform *wfhw = _wfhw;
	struct sun8i_pwm_chip *sun8i_chip = sun8i_pwm_from_chip(chip);
	unsigned int pair = SUN8I_PWM_PAIR_IDX(pwm->hwpwm);
	unsigned long pair_rate;
	u32 pccr, pcr, pdzcr, per, ppr;

	pair_rate = sun8i_pwm_get_pair_rate(sun8i_chip, pwm->hwpwm);
	if (pair_rate > U32_MAX)
		return -ERANGE;

	scoped_guard(spinlock_irqsave, &sun8i_chip->lock) {
		pccr = sun8i_pwm_readl(sun8i_chip, SUN8I_PWM_PCCR(pair));
		per = sun8i_pwm_readl(sun8i_chip, SUN8I_PWM_PER);
		pcr = sun8i_pwm_readl(sun8i_chip, SUN8I_PWM_PCR(pwm->hwpwm));
		ppr = sun8i_pwm_readl(sun8i_chip, SUN8I_PWM_PPR(pwm->hwpwm));
		pdzcr = sun8i_pwm_readl(sun8i_chip, SUN8I_PWM_PDZCR(pair));
	}

	*wfhw = (struct sun8i_pwm_waveform) {
		.enabled = !!(pccr & SUN8I_PWM_PCCR_GATE) &&
			   !!(per & SUN8I_PWM_ENABLE(pwm->hwpwm)),
		.bypass_en = !!(pccr &
				 BIT(SUN8I_PWM_PCCR_BYPASS_BIT(pwm->hwpwm))),
		.pcr = pcr & SUN8I_PWM_PCR_WAVEFORM_MASK,
		.ppr = ppr,
		.pair_rate = pair_rate,
	};

	/* The waveform API cannot describe coupled dead-zone or pulse mode. */
	if (wfhw->enabled && !wfhw->bypass_en &&
	    ((pdzcr & SUN8I_PWM_PDZCR_ENABLE) || (pcr & SUN8I_PWM_PCR_MODE)))
		return -EOPNOTSUPP;

	return 0;
}

static u64 sun8i_pwm_ticks_to_ns(u32 ticks, u16 div_k, u32 pair_rate)
{
	return DIV_ROUND_UP_ULL(NSEC_PER_SEC * (u64)ticks * div_k,
				pair_rate);
}

static u64 sun8i_pwm_period_ns(const struct sun8i_pwm_waveform *wfhw)
{
	if (wfhw->bypass_en)
		return DIV_ROUND_UP_ULL(NSEC_PER_SEC, wfhw->pair_rate);

	return sun8i_pwm_ticks_to_ns(SUN8I_PWM_PPR_PERIOD_VALUE(wfhw->ppr),
		FIELD_GET(SUN8I_PWM_PCR_PRESCAL_K_MASK, wfhw->pcr) + 1,
		wfhw->pair_rate);
}

static void
__sun8i_pwm_round_waveform_fromhw(const struct sun8i_pwm_waveform *wfhw,
				  struct pwm_waveform *wf)
{
	u32 pair_rate = wfhw->pair_rate;
	u32 period_ticks = SUN8I_PWM_PPR_PERIOD_VALUE(wfhw->ppr);
	u32 duty_ticks = SUN8I_PWM_PPR_DUTY_VALUE(wfhw->ppr);
	u16 div_k = FIELD_GET(SUN8I_PWM_PCR_PRESCAL_K_MASK, wfhw->pcr) + 1;

	wf->duty_offset_ns = 0;

	if (!wfhw->enabled || !pair_rate) {
		wf->period_length_ns = 0;
		wf->duty_length_ns = 0;
		return;
	}

	wf->period_length_ns = sun8i_pwm_period_ns(wfhw);
	if (wfhw->bypass_en) {
		wf->duty_length_ns =
			DIV_ROUND_UP_ULL(NSEC_PER_SEC,
					 2ULL * pair_rate);
		return;
	}

	duty_ticks = min(duty_ticks, period_ticks);
	if (!(wfhw->pcr & SUN8I_PWM_PCR_ACTIVE_STATE)) {
		/* Constant outputs have no edge and therefore no offset. */
		if (duty_ticks && duty_ticks < period_ticks)
			wf->duty_offset_ns =
				sun8i_pwm_ticks_to_ns(duty_ticks, div_k, pair_rate);
		duty_ticks = period_ticks - duty_ticks;
	}
	wf->duty_length_ns = sun8i_pwm_ticks_to_ns(duty_ticks, div_k, pair_rate);
}

static int sun8i_pwm_round_waveform_fromhw(struct pwm_chip *chip,
					   struct pwm_device *pwm,
					   const void *_wfhw,
					   struct pwm_waveform *wf)
{
	const struct sun8i_pwm_waveform *wfhw = _wfhw;

	__sun8i_pwm_round_waveform_fromhw(wfhw, wf);

	dev_dbg(pwmchip_parent(chip),
		"pwm#%u: pair-rate=%u, pcr=%#x, ppr=%#x, bypass=%u -> %llu/%llu [+%llu]\n",
		pwm->hwpwm, wfhw->pair_rate, wfhw->pcr, wfhw->ppr,
		wfhw->bypass_en,
		wf->duty_length_ns, wf->period_length_ns,
		wf->duty_offset_ns);

	return 0;
}

static bool
sun8i_pwm_pair_rate_constrained(struct sun8i_pwm_chip *sun8i_chip,
				unsigned int idx)
{
	unsigned int sibling = idx ^ 1;

	guard(spinlock_irqsave)(&sun8i_chip->lock);
	return sun8i_chip->channels[sibling].mode == SUN8I_PWM_MODE_CLK ||
	       sun8i_chip->channels[sibling].rate_exclusive ||
	       sun8i_pwm_channel_is_enabled_locked(sun8i_chip, sibling);
}

static int sun8i_pwm_clear_idle_deadzone(struct sun8i_pwm_chip *sun8i_chip,
					 unsigned int idx)
{
	unsigned int pair = SUN8I_PWM_PAIR_IDX(idx);
	u32 pdzcr, per;

	guard(spinlock_irqsave)(&sun8i_chip->lock);
	pdzcr = sun8i_pwm_readl(sun8i_chip, SUN8I_PWM_PDZCR(pair));
	if (!(pdzcr & SUN8I_PWM_PDZCR_ENABLE))
		return 0;

	per = sun8i_pwm_readl(sun8i_chip, SUN8I_PWM_PER);
	if (per & sun8i_pwm_pair_enable_mask(pair))
		return -EOPNOTSUPP;

	pdzcr &= ~SUN8I_PWM_PDZCR_ENABLE;
	sun8i_pwm_writel(sun8i_chip, pdzcr, SUN8I_PWM_PDZCR(pair));

	return 0;
}

static u64 sun8i_pwm_max_period_ns(struct sun8i_pwm_chip *sun8i_chip,
				   unsigned int idx)
{
	unsigned long pair_rate;
	u32 pcr;
	u16 div_k;

	pair_rate = sun8i_pwm_get_pair_rate(sun8i_chip, idx);
	if (!pair_rate || pair_rate > U32_MAX)
		return 0;

	scoped_guard(spinlock_irqsave, &sun8i_chip->lock) {
		pcr = sun8i_pwm_readl(sun8i_chip, SUN8I_PWM_PCR(idx));
	}
	div_k = FIELD_GET(SUN8I_PWM_PCR_PRESCAL_K_MASK, pcr) + 1;

	return sun8i_pwm_ticks_to_ns(SUN8I_PWM_PPR_PERIOD_MAX, div_k,
				     pair_rate);
}

static int sun8i_pwm_wait_period_ready(struct pwm_chip *chip, unsigned int idx)
{
	struct sun8i_pwm_chip *sun8i_chip = sun8i_pwm_from_chip(chip);
	struct sun8i_pwm_channel *chan = &sun8i_chip->channels[idx];
	unsigned long poll_us;
	u64 period_ns, timeout_us;
	u32 val;
	int ret;

	/* PPR contains the pending value, not necessarily the active period. */
	period_ns = chan->pending_period_ns;
	if (!period_ns)
		period_ns = sun8i_pwm_max_period_ns(sun8i_chip, idx);
	timeout_us = DIV_ROUND_UP_ULL(period_ns, NSEC_PER_USEC) +
		     SUN8I_PWM_PERIOD_READY_MARGIN_US;
	poll_us = clamp_t(u64,
			  DIV_ROUND_UP_ULL(timeout_us,
					   SUN8I_PWM_PERIOD_READY_POLL_COUNT),
			  SUN8I_PWM_PERIOD_READY_MIN_POLL_US,
			  SUN8I_PWM_PERIOD_READY_MAX_POLL_US);

	ret = readl_poll_timeout(sun8i_chip->base + SUN8I_PWM_PCR(idx), val,
				 !(val & SUN8I_PWM_PCR_PERIOD_READY), poll_us,
				 timeout_us);
	if (!ret)
		chan->pending_period_ns = 0;
	else
		dev_err_ratelimited(pwmchip_parent(chip),
				    "pwm#%u: update timeout after %llu us\n",
				    idx, timeout_us);

	return ret;
}

/*
 * Protect the rate and quiesce the output if a live update is unsafe. On
 * failure restore the old clock before restarting an output we stopped.
 * CCF operations must stay outside the register lock.
 */
static int sun8i_pwm_prepare_update(struct pwm_chip *chip, unsigned int idx,
				    const struct sun8i_pwm_waveform *wfhw,
				  struct sun8i_pwm_waveform *old)
{
	struct sun8i_pwm_chip *sun8i_chip = sun8i_pwm_from_chip(chip);
	struct sun8i_pwm_channel *chan = &sun8i_chip->channels[idx];
	unsigned long rate, bus_rate;
	bool had_exclusive, stop;
	u32 pccr;
	int ret, restore_ret;

	scoped_guard(spinlock_irqsave, &sun8i_chip->lock)
		had_exclusive = chan->rate_exclusive;

	if (!had_exclusive) {
		ret = clk_rate_exclusive_get(chan->pair_clk);
		if (ret)
			return ret;
		scoped_guard(spinlock_irqsave, &sun8i_chip->lock)
			chan->rate_exclusive = true;
	}

	/* Sample the old rate only after excluding other rate changes. */
	rate = sun8i_pwm_get_pair_rate(sun8i_chip, idx);
	if (!rate || rate > U32_MAX) {
		ret = -ERANGE;
		goto put_rate;
	}
	old->pair_rate = rate;
	scoped_guard(spinlock_irqsave, &sun8i_chip->lock) {
		old->enabled = sun8i_pwm_channel_is_enabled_locked(sun8i_chip, idx);
		pccr = sun8i_pwm_readl(sun8i_chip,
				       SUN8I_PWM_PCCR(SUN8I_PWM_PAIR_IDX(idx)));
		old->bypass_en = !!(pccr & BIT(SUN8I_PWM_PCCR_BYPASS_BIT(idx)));
		old->pcr = sun8i_pwm_readl(sun8i_chip, SUN8I_PWM_PCR(idx));
		old->ppr = sun8i_pwm_readl(sun8i_chip, SUN8I_PWM_PPR(idx));
	}

	if (rate != wfhw->pair_rate &&
	    sun8i_pwm_pair_rate_constrained(sun8i_chip, idx)) {
		ret = -EBUSY;
		goto put_rate;
	}

	/* Live PPR updates require unchanged configuration and a faster PCLK. */
	bus_rate = clk_get_rate(sun8i_chip->bus_clk);
	stop = old->enabled &&
		(old->bypass_en != wfhw->bypass_en || rate != wfhw->pair_rate ||
		 (!wfhw->bypass_en &&
		  (((old->pcr ^ wfhw->pcr) &
		    (SUN8I_PWM_PCR_WAVEFORM_MASK | SUN8I_PWM_PCR_MODE)) ||
		   bus_rate <= DIV_ROUND_UP_ULL(wfhw->pair_rate,
			FIELD_GET(SUN8I_PWM_PCR_PRESCAL_K_MASK, wfhw->pcr) + 1))));
	if (stop) {
		scoped_guard(spinlock_irqsave, &sun8i_chip->lock)
			sun8i_pwm_set_enabled_locked(sun8i_chip, idx, false);
	}

	if (rate == wfhw->pair_rate)
		return 0;

	ret = clk_set_rate(chan->pair_clk, wfhw->pair_rate);
	if (!ret && sun8i_pwm_get_pair_rate(sun8i_chip, idx) == wfhw->pair_rate)
		return 0;
	if (!ret)
		ret = -EINVAL;

	if (sun8i_pwm_get_pair_rate(sun8i_chip, idx) != rate) {
		restore_ret = clk_set_rate(chan->pair_clk, rate);
		if (restore_ret || sun8i_pwm_get_pair_rate(sun8i_chip, idx) != rate) {
			dev_err(pwmchip_parent(chip),
				"pwm#%u: failed to restore pair clock rate %lu\n",
				idx, rate);
			goto put_rate;
		}
	}
	if (stop) {
		scoped_guard(spinlock_irqsave, &sun8i_chip->lock)
			sun8i_pwm_set_enabled_locked(sun8i_chip, idx, true);
	}
put_rate:
	if (!had_exclusive)
		sun8i_pwm_put_rate(sun8i_chip, idx);

	return ret;
}

static int sun8i_pwm_write_waveform(struct pwm_chip *chip,
				    struct pwm_device *pwm, const void *_wfhw)
{
	const struct sun8i_pwm_waveform *wfhw = _wfhw;
	struct sun8i_pwm_chip *sun8i_chip = sun8i_pwm_from_chip(chip);
	struct sun8i_pwm_channel *chan = &sun8i_chip->channels[pwm->hwpwm];
	struct sun8i_pwm_waveform old;
	u64 old_ns = 0, new_ns;
	u32 pcr;
	int ret;

	if (!wfhw->enabled) {
		scoped_guard(spinlock_irqsave, &sun8i_chip->lock) {
			sun8i_pwm_set_enabled_locked(sun8i_chip, pwm->hwpwm, false);
			sun8i_pwm_set_bypass_locked(sun8i_chip, pwm->hwpwm, false);
		}
		sun8i_pwm_put_rate(sun8i_chip, pwm->hwpwm);
		return 0;
	}

	if (!wfhw->bypass_en) {
		ret = sun8i_pwm_clear_idle_deadzone(sun8i_chip, pwm->hwpwm);
		if (ret)
			return ret;
		ret = sun8i_pwm_wait_period_ready(chip, pwm->hwpwm);
		if (ret)
			return ret;
	}

	ret = sun8i_pwm_prepare_update(chip, pwm->hwpwm, wfhw, &old);
	if (ret)
		return ret;

	if (!wfhw->bypass_en) {
		/* Apply only waveform fields; never replay status or W1S bits. */
		pcr = old.pcr & ~(SUN8I_PWM_PCR_WAVEFORM_MASK |
				  SUN8I_PWM_PCR_MODE | SUN8I_PWM_PCR_PULSE_START |
				  SUN8I_PWM_PCR_PERIOD_READY);
		sun8i_pwm_writel(sun8i_chip, pcr | wfhw->pcr,
				 SUN8I_PWM_PCR(pwm->hwpwm));

		if (old.enabled && !old.bypass_en)
			old_ns = sun8i_pwm_period_ns(&old);
		new_ns = sun8i_pwm_period_ns(wfhw);
		chan->pending_period_ns = max(old_ns, new_ns);
		sun8i_pwm_writel(sun8i_chip, wfhw->ppr, SUN8I_PWM_PPR(pwm->hwpwm));
	}

	scoped_guard(spinlock_irqsave, &sun8i_chip->lock) {
		sun8i_pwm_set_bypass_locked(sun8i_chip, pwm->hwpwm, wfhw->bypass_en);
		sun8i_pwm_set_enabled_locked(sun8i_chip, pwm->hwpwm, true);
	}

	return 0;
}

struct sun8i_pwm_rounding {
	const struct pwm_waveform *requested;
	struct sun8i_pwm_waveform best;
	struct pwm_waveform best_wf;
	u32 current_pair_rate;
	bool have_best;
};

static void
sun8i_pwm_choose_waveform(struct sun8i_pwm_rounding *rounding,
			  const struct sun8i_pwm_waveform *candidate)
{
	const struct pwm_waveform *requested = rounding->requested;
	const struct pwm_waveform *best = &rounding->best_wf;
	struct pwm_waveform wf;
	bool better;

	__sun8i_pwm_round_waveform_fromhw(candidate, &wf);
	/* A zero-duty, zero-offset cycle-mode candidate always exists. */
	if (wf.duty_length_ns > requested->duty_length_ns ||
	    wf.duty_offset_ns > requested->duty_offset_ns)
		return;

	if (!rounding->have_best) {
		better = true;
	} else if (wf.period_length_ns != best->period_length_ns) {
		/* Prefer the longest period below the request, else the shortest. */
		if (wf.period_length_ns > requested->period_length_ns)
			better = wf.period_length_ns < best->period_length_ns;
		else
			better = best->period_length_ns > requested->period_length_ns ||
				 wf.period_length_ns > best->period_length_ns;
	} else if (wf.duty_length_ns != best->duty_length_ns) {
		better = wf.duty_length_ns > best->duty_length_ns;
	} else if (wf.duty_offset_ns != best->duty_offset_ns) {
		better = wf.duty_offset_ns > best->duty_offset_ns;
	} else {
		/* Prefer the current pair rate when waveforms are equivalent. */
		better = candidate->pair_rate == rounding->current_pair_rate &&
			 rounding->best.pair_rate != rounding->current_pair_rate;
	}
	if (!better)
		return;

	rounding->best = *candidate;
	rounding->best_wf = wf;
	rounding->have_best = true;
}

static struct sun8i_pwm_waveform
sun8i_pwm_round_cycle(const struct pwm_waveform *requested, u32 pair_rate,
		      u16 div_k)
{
	struct sun8i_pwm_waveform candidate = {
		.enabled = true,
		.pair_rate = pair_rate,
		.pcr = FIELD_PREP(SUN8I_PWM_PCR_PRESCAL_K_MASK, div_k - 1),
	};
	u64 denominator = NSEC_PER_SEC * (u64)div_k;
	u64 duty_ticks, period_ticks;
	u32 inactive_ticks;

	/* Floor division rounds down, except for the minimum one-tick period. */
	period_ticks = mul_u64_u64_div_u64(requested->period_length_ns,
					   pair_rate, denominator);
	period_ticks = clamp_t(u64, period_ticks, 1, SUN8I_PWM_PPR_PERIOD_MAX);
	duty_ticks = mul_u64_u64_div_u64(requested->duty_length_ns,
					 pair_rate, denominator);
	duty_ticks = min(duty_ticks, period_ticks);

	/*
	 * Zero active ticks encode either constant level. For a toggling
	 * waveform, active-low mode places the rising edge after the inactive
	 * part, which is the only non-zero offset supported by the hardware.
	 */
	if (!duty_ticks) {
		candidate.pcr |= SUN8I_PWM_PCR_ACTIVE_STATE;
	} else if (duty_ticks == period_ticks) {
		duty_ticks = 0;
	} else {
		inactive_ticks = period_ticks - duty_ticks;
		if (sun8i_pwm_ticks_to_ns(inactive_ticks, div_k, pair_rate) <=
		    requested->duty_offset_ns)
			duty_ticks = inactive_ticks;
		else
			candidate.pcr |= SUN8I_PWM_PCR_ACTIVE_STATE;
	}

	candidate.ppr = SUN8I_PWM_PPR_PERIOD(period_ticks) |
			SUN8I_PWM_PPR_DUTY(duty_ticks);

	return candidate;
}

static void
sun8i_pwm_consider_pair_rate(struct sun8i_pwm_rounding *rounding, u32 pair_rate)
{
	struct sun8i_pwm_waveform candidate;

	for (unsigned int div_k = 1; div_k <= 256; div_k++) {
		candidate = sun8i_pwm_round_cycle(rounding->requested, pair_rate, div_k);
		sun8i_pwm_choose_waveform(rounding, &candidate);
	}

	candidate = (struct sun8i_pwm_waveform) {
		.pair_rate = pair_rate,
		.enabled = true,
		.bypass_en = true,
	};
	sun8i_pwm_choose_waveform(rounding, &candidate);
}

static int sun8i_pwm_round_waveform_tohw(struct pwm_chip *chip,
					 struct pwm_device *pwm,
					 const struct pwm_waveform *wf,
					 void *_wfhw)
{
	struct sun8i_pwm_chip *sun8i_chip = sun8i_pwm_from_chip(chip);
	struct sun8i_pwm_pair *pair =
		&sun8i_chip->pairs[SUN8I_PWM_PAIR_IDX(pwm->hwpwm)];
	struct sun8i_pwm_waveform *wfhw = _wfhw;
	struct sun8i_pwm_rounding rounding = {
		.requested = wf,
	};
	unsigned long current_rate;

	if (!wf->period_length_ns) {
		*wfhw = (struct sun8i_pwm_waveform) {
			.enabled = false,
		};
		return 0;
	}

	current_rate = sun8i_pwm_get_pair_rate(sun8i_chip, pwm->hwpwm);
	if (!current_rate || current_rate > U32_MAX)
		return -ERANGE;
	rounding.current_pair_rate = current_rate;

	if (sun8i_pwm_pair_rate_constrained(sun8i_chip, pwm->hwpwm)) {
		sun8i_pwm_consider_pair_rate(&rounding, current_rate);
	} else {
		for (unsigned int parent_idx = 0;
		     parent_idx < clk_hw_get_num_parents(pair->hw);
		     parent_idx++) {
			struct clk_hw *parent;
			unsigned long parent_rate;

			parent = clk_hw_get_parent_by_index(pair->hw, parent_idx);
			if (!parent)
				continue;
			parent_rate = clk_hw_get_rate(parent);
			if (!parent_rate)
				continue;

			for (unsigned int i = 0; sun8i_pwm_div_m_table[i].div;
			     i++) {
				u16 div_m = sun8i_pwm_div_m_table[i].div;
				u64 pair_rate;

				pair_rate = DIV_ROUND_UP_ULL(parent_rate, div_m);
				if (!pair_rate || pair_rate > U32_MAX)
					continue;
				sun8i_pwm_consider_pair_rate(&rounding, pair_rate);
			}
		}
	}

	if (!rounding.have_best)
		return -EINVAL;
	*wfhw = rounding.best;

	dev_dbg(pwmchip_parent(chip),
		"pwm#%u: %llu/%llu [+%llu] -> pair-rate=%u, pcr=%#x, ppr=%#x, bypass=%u\n",
		pwm->hwpwm, wf->duty_length_ns, wf->period_length_ns,
		wf->duty_offset_ns, wfhw->pair_rate, wfhw->pcr, wfhw->ppr,
		wfhw->bypass_en);

	return rounding.best_wf.period_length_ns > wf->period_length_ns;
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

/* Register the shared mux, gate and /div_m clock for each channel pair. */
static int sun8i_pwm_register_pair_clocks(struct device *dev,
					  struct sun8i_pwm_chip *sun8i_chip)
{
	static const struct clk_parent_data parent_data[] = {
		{ .index = 0 },
		{ .index = 1 },
	};

	for (unsigned int i = 0; i < SUN8I_PWM_NPAIRS; i++) {
		struct sun8i_pwm_pair *pair = &sun8i_chip->pairs[i];
		void __iomem *reg = sun8i_chip->base + SUN8I_PWM_PCCR(i);
		const char *name;
		int ret;

		name = devm_kasprintf(dev, GFP_KERNEL,
				      "%s#pwm-clk-src%u%u", dev_name(dev),
				       i * 2, i * 2 + 1);
		if (!name)
			return -ENOMEM;

		pair->mux.reg = reg;
		pair->mux.shift = SUN8I_PWM_PCCR_SRC_SHIFT;
		pair->mux.mask = SUN8I_PWM_PCCR_SRC_MASK;
		pair->mux.flags = CLK_MUX_ROUND_CLOSEST;
		pair->mux.lock = &sun8i_chip->lock;

		pair->chip = sun8i_chip;
		pair->index = i;

		pair->divider.reg = reg;
		pair->divider.shift = SUN8I_PWM_PCCR_DIV_M_SHIFT;
		pair->divider.width = SUN8I_PWM_PCCR_DIV_M_WIDTH;
		pair->divider.table = sun8i_pwm_div_m_table;
		pair->divider.lock = &sun8i_chip->lock;

		pair->hw = devm_clk_hw_register_composite_pdata(dev, name,
								parent_data,
								ARRAY_SIZE(parent_data),
			&pair->mux.hw, &clk_mux_ops,
			&pair->divider.hw, &clk_divider_ops,
			&pair->gate_hw, &sun8i_pwm_pair_gate_ops, 0);
		if (IS_ERR(pair->hw))
			return dev_err_probe(dev, PTR_ERR(pair->hw),
					     "Failed to register pair %u clock\n", i);

		/* Borrow CCF's handle so registration cannot pin this module. */
		pair->rate_nb.notifier_call = sun8i_pwm_pair_rate_notifier;
		ret = devm_clk_notifier_register(dev, pair->hw->clk, &pair->rate_nb);
		if (ret)
			return dev_err_probe(dev, ret,
					     "Failed to protect pair %u clock rate\n", i);
	}

	return 0;
}

/* Register the bypass clock for each channel. */
static int sun8i_pwm_register_bypass_clocks(struct device *dev,
					    struct sun8i_pwm_chip *sun8i_chip)
{
	for (unsigned int i = 0; i < SUN8I_PWM_NPWM; i++) {
		struct sun8i_pwm_channel *chan = &sun8i_chip->channels[i];
		struct clk_hw *parent = sun8i_chip->pairs[SUN8I_PWM_PAIR_IDX(i)].hw;
		const char *name;
		int ret;

		name = devm_kasprintf(dev, GFP_KERNEL, "%s#pwm-bypass%u",
				      dev_name(dev), i);
		if (!name)
			return -ENOMEM;

		chan->chip = sun8i_chip;
		chan->index = i;
		/*
		 * Protect the shared rate while prepared. Preserve unclaimed
		 * firmware outputs through CCF's unused-clock cleanup, as for
		 * PWM waveforms; only a consumer may turn them off.
		 */
		chan->bypass_hw.init =
			CLK_HW_INIT_HW(name, parent, &sun8i_pwm_bypass_ops,
				       CLK_SET_RATE_PARENT | CLK_SET_RATE_GATE |
				       CLK_IGNORE_UNUSED);

		ret = devm_clk_hw_register(dev, &chan->bypass_hw);
		if (ret)
			return dev_err_probe(dev, ret,
					     "Failed to register bypass clock %u\n", i);
	}

	return 0;
}

/*
 * A disabled pair gate makes any set PER bits ineffective. Clear those stale
 * enables before a clock consumer can turn the shared gate on and expose an
 * unrequested output.
 */
static void
sun8i_pwm_sanitize_disabled_pairs(struct sun8i_pwm_chip *sun8i_chip)
{
	u32 per;

	guard(spinlock_irqsave)(&sun8i_chip->lock);
	per = sun8i_pwm_readl(sun8i_chip, SUN8I_PWM_PER);
	for (unsigned int pair = 0; pair < SUN8I_PWM_NPAIRS; pair++) {
		unsigned int first = pair * 2;
		u32 pccr;

		pccr = sun8i_pwm_readl(sun8i_chip, SUN8I_PWM_PCCR(pair));
		if (pccr & SUN8I_PWM_PCCR_GATE)
			continue;

		per &= ~sun8i_pwm_pair_enable_mask(pair);
		pccr &= ~BIT(SUN8I_PWM_PCCR_BYPASS_BIT(first));
		pccr &= ~BIT(SUN8I_PWM_PCCR_BYPASS_BIT(first + 1));
		sun8i_pwm_writel(sun8i_chip, pccr, SUN8I_PWM_PCCR(pair));
	}
	sun8i_pwm_writel(sun8i_chip, per, SUN8I_PWM_PER);
}

static struct clk_hw *sun8i_pwm_get_clk_hw(struct of_phandle_args *clkspec,
					   void *data)
{
	struct sun8i_pwm_chip *sun8i_chip = data;

	if (clkspec->args[0] >= SUN8I_PWM_NPWM)
		return ERR_PTR(-EINVAL);

	return &sun8i_chip->channels[clkspec->args[0]].bypass_hw;
}

static int sun8i_pwm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sun8i_pwm_chip *sun8i_chip;
	struct reset_control *rst;
	struct pwm_chip *chip;
	int ret;

	chip = devm_pwmchip_alloc(dev, SUN8I_PWM_NPWM, sizeof(*sun8i_chip));
	if (IS_ERR(chip))
		return dev_err_probe(dev, PTR_ERR(chip),
				     "Failed to allocate pwmchip\n");

	sun8i_chip = sun8i_pwm_from_chip(chip);
	spin_lock_init(&sun8i_chip->lock);
	sun8i_chip->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(sun8i_chip->base))
		return dev_err_probe(dev, PTR_ERR(sun8i_chip->base),
				     "Failed to get PWM base address\n");

	sun8i_chip->bus_clk = devm_clk_get_enabled(dev, "bus");
	if (IS_ERR(sun8i_chip->bus_clk))
		return dev_err_probe(dev, PTR_ERR(sun8i_chip->bus_clk),
				     "Failed to get bus clock\n");

	rst = devm_reset_control_get_shared_deasserted(dev, NULL);
	if (IS_ERR(rst))
		return dev_err_probe(dev, PTR_ERR(rst),
				     "Failed to get reset control\n");

	chip->ops = &sun8i_pwm_ops;

	sun8i_pwm_sanitize_disabled_pairs(sun8i_chip);

	ret = sun8i_pwm_register_pair_clocks(dev, sun8i_chip);
	if (ret)
		return ret;

	ret = sun8i_pwm_register_bypass_clocks(dev, sun8i_chip);
	if (ret)
		return ret;

	ret = devm_of_clk_add_hw_provider(dev, sun8i_pwm_get_clk_hw, sun8i_chip);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to add HW clock provider\n");

	ret = devm_pwmchip_add(dev, chip);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to add PWM chip\n");

	return 0;
}

static const struct of_device_id sun8i_pwm_dt_ids[] = {
	{
		.compatible = "allwinner,sun50i-h616-pwm",
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
