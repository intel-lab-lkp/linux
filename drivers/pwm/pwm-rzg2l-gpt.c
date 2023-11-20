// SPDX-License-Identifier: GPL-2.0
/*
 * Renesas RZ/G2L General PWM Timer (GPT) driver
 *
 * Copyright (C) 2023 Renesas Electronics Corporation
 *
 * Hardware manual for this IP can be found here
 * https://www.renesas.com/eu/en/document/mah/rzg2l-group-rzg2lc-group-users-manual-hardware-0?language=en
 *
 * Limitations:
 * - Counter must be stopped before modifying Mode and Prescaler.
 * - When PWM is disabled, the output is driven to inactive.
 * - While the hardware supports both polarities, the driver (for now)
 *   only handles normal polarity.
 * - When both channels are used, disabling the channel on one stops the
 *   other.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/pwm.h>
#include <linux/reset.h>
#include <linux/time.h>

#define RZG2L_GTCR		0x2c
#define RZG2L_GTUDDTYC		0x30
#define RZG2L_GTIOR		0x34
#define RZG2L_GTINTAD		0x38
#define RZG2L_GTBER		0x40
#define RZG2L_GTCNT		0x48
#define RZG2L_GTCCRA		0x4c
#define RZG2L_GTCCRB		0x50
#define RZG2L_GTPR		0x64

#define RZG2L_GTCR_CST		BIT(0)
#define RZG2L_GTCR_MD		GENMASK(18, 16)
#define RZG2L_GTCR_TPCS		GENMASK(26, 24)

#define RZG2L_GTCR_MD_SAW_WAVE_PWM_MODE	FIELD_PREP(RZG2L_GTCR_MD, 0)

#define RZG2L_GTUDDTYC_UP	BIT(0)
#define RZG2L_GTUDDTYC_UDF	BIT(1)
#define RZG2L_UP_COUNTING	(RZG2L_GTUDDTYC_UP | RZG2L_GTUDDTYC_UDF)

#define RZG2L_GTIOR_GTIOA	GENMASK(4, 0)
#define RZG2L_GTIOR_OADF	GENMASK(10, 9)
#define RZG2L_GTIOR_GTIOB	GENMASK(20, 16)
#define RZG2L_GTIOR_OBDF	GENMASK(26, 25)
#define RZG2L_GTIOR_OAE		BIT(8)
#define RZG2L_GTIOR_OBE		BIT(24)
#define RZG2L_GTIOR_OADF_HIGH_IMP_ON_OUT_DISABLE	BIT(9)
#define RZG2L_GTIOR_OBDF_HIGH_IMP_ON_OUT_DISABLE	BIT(25)
#define RZG2L_GTIOR_PIN_DISABLE_SETTING \
	(RZG2L_GTIOR_OADF_HIGH_IMP_ON_OUT_DISABLE | RZG2L_GTIOR_OBDF_HIGH_IMP_ON_OUT_DISABLE)

#define RZG2L_INIT_OUT_LO_OUT_LO_END_TOGGLE	0x07
#define RZG2L_INIT_OUT_HI_OUT_HI_END_TOGGLE	0x1b

#define RZG2L_GTIOR_GTIOA_OUT_HI_END_TOGGLE_CMP_MATCH \
	(RZG2L_INIT_OUT_HI_OUT_HI_END_TOGGLE | RZG2L_GTIOR_OAE)
#define RZG2L_GTIOR_GTIOA_OUT_LO_END_TOGGLE_CMP_MATCH \
	(RZG2L_INIT_OUT_LO_OUT_LO_END_TOGGLE | RZG2L_GTIOR_OAE)
#define RZG2L_GTIOR_GTIOB_OUT_HI_END_TOGGLE_CMP_MATCH \
	(FIELD_PREP(RZG2L_GTIOR_GTIOB, RZG2L_INIT_OUT_HI_OUT_HI_END_TOGGLE) | RZG2L_GTIOR_OBE)
#define RZG2L_GTIOR_GTIOB_OUT_LO_END_TOGGLE_CMP_MATCH \
	(FIELD_PREP(RZG2L_GTIOR_GTIOB, RZG2L_INIT_OUT_LO_OUT_LO_END_TOGGLE) | RZG2L_GTIOR_OBE)

#define RZG2L_GTINTAD_GRP_MASK			GENMASK(25, 24)

#define RZG2L_GTCCR(i) (0x4c + 4 * (i))

#define RZG2L_MAX_HW_CHANNELS	8
#define RZG2L_CHANNELS_PER_IO	2
#define RZG2L_MAX_PWM_CHANNELS	(RZG2L_MAX_HW_CHANNELS * RZG2L_CHANNELS_PER_IO)
#define RZG2L_MAX_SCALE_FACTOR	1024

#define RZG2L_IS_IOB(a)	((a) & 0x1)
#define RZG2L_GET_CH(a)	((a) / 2)

#define RZG2L_GET_CH_OFFS(i) (0x100 * (i))

#define RZG2L_MAX_POEG_GROUPS	4
#define RZG2L_LAST_POEG_GROUP	3

struct rzg2l_gpt_chip {
	struct pwm_chip chip;
	void __iomem *mmio;
	struct reset_control *rstc;
	struct clk *clk;
	struct mutex lock; /* lock to protect shared channel resources */
	unsigned long rate;
	u64 max_val;
	u32 state_period[RZG2L_MAX_HW_CHANNELS];
	u32 user_count[RZG2L_MAX_HW_CHANNELS];
	u32 enable_count[RZG2L_MAX_HW_CHANNELS];
	DECLARE_BITMAP(ch_en_bits, RZG2L_MAX_PWM_CHANNELS);
	DECLARE_BITMAP(poeg_gpt_link, RZG2L_MAX_POEG_GROUPS * RZG2L_MAX_HW_CHANNELS);
};

static inline struct rzg2l_gpt_chip *to_rzg2l_gpt_chip(struct pwm_chip *chip)
{
	return container_of(chip, struct rzg2l_gpt_chip, chip);
}

static void rzg2l_gpt_write(struct rzg2l_gpt_chip *rzg2l_gpt, u32 reg, u32 data)
{
	writel(data, rzg2l_gpt->mmio + reg);
}

static u32 rzg2l_gpt_read(struct rzg2l_gpt_chip *rzg2l_gpt, u32 reg)
{
	return readl(rzg2l_gpt->mmio + reg);
}

static void rzg2l_gpt_modify(struct rzg2l_gpt_chip *rzg2l_gpt, u32 reg, u32 clr,
			     u32 set)
{
	rzg2l_gpt_write(rzg2l_gpt, reg,
			(rzg2l_gpt_read(rzg2l_gpt, reg) & ~clr) | set);
}

static u8 rzg2l_gpt_calculate_prescale(struct rzg2l_gpt_chip *rzg2l_gpt,
				       u64 period_cycles)
{
	u32 prescaled_period_cycles;
	u8 prescale;

	prescaled_period_cycles = period_cycles >> 32;
	if (prescaled_period_cycles >= 256)
		prescale = 5;
	else
		prescale = (fls(prescaled_period_cycles) + 1) / 2;

	return prescale;
}

static int rzg2l_gpt_request(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct rzg2l_gpt_chip *rzg2l_gpt = to_rzg2l_gpt_chip(chip);
	u32 ch = RZG2L_GET_CH(pwm->hwpwm);

	mutex_lock(&rzg2l_gpt->lock);
	rzg2l_gpt->user_count[ch]++;
	mutex_unlock(&rzg2l_gpt->lock);

	return 0;
}

static void rzg2l_gpt_free(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct rzg2l_gpt_chip *rzg2l_gpt = to_rzg2l_gpt_chip(chip);
	u32 ch = RZG2L_GET_CH(pwm->hwpwm);

	mutex_lock(&rzg2l_gpt->lock);
	rzg2l_gpt->user_count[ch]--;
	mutex_unlock(&rzg2l_gpt->lock);
}

static bool rzg2l_gpt_is_ch_enabled(struct rzg2l_gpt_chip *rzg2l_gpt, u8 hwpwm)
{
	u8 ch = RZG2L_GET_CH(hwpwm);
	u32 offs = RZG2L_GET_CH_OFFS(ch);
	bool is_counter_running, is_output_en;
	u32 val;

	val = rzg2l_gpt_read(rzg2l_gpt, offs + RZG2L_GTCR);
	is_counter_running = val & RZG2L_GTCR_CST;

	val = rzg2l_gpt_read(rzg2l_gpt, offs + RZG2L_GTIOR);
	if (RZG2L_IS_IOB(hwpwm))
		is_output_en = val & RZG2L_GTIOR_OBE;
	else
		is_output_en = val & RZG2L_GTIOR_OAE;

	return (is_counter_running && is_output_en);
}

static int rzg2l_gpt_enable(struct rzg2l_gpt_chip *rzg2l_gpt,
			    struct pwm_device *pwm)
{
	u8 ch = RZG2L_GET_CH(pwm->hwpwm);
	u32 offs = RZG2L_GET_CH_OFFS(ch);

	/* Enable pin output */
	if (RZG2L_IS_IOB(pwm->hwpwm))
		rzg2l_gpt_modify(rzg2l_gpt, offs + RZG2L_GTIOR,
				 RZG2L_GTIOR_GTIOB | RZG2L_GTIOR_OBE,
				 RZG2L_GTIOR_GTIOB_OUT_HI_END_TOGGLE_CMP_MATCH);
	else
		rzg2l_gpt_modify(rzg2l_gpt, offs + RZG2L_GTIOR,
				 RZG2L_GTIOR_GTIOA | RZG2L_GTIOR_OAE,
				 RZG2L_GTIOR_GTIOA_OUT_HI_END_TOGGLE_CMP_MATCH);

	mutex_lock(&rzg2l_gpt->lock);
	if (!rzg2l_gpt->enable_count[ch])
		rzg2l_gpt_modify(rzg2l_gpt, offs + RZG2L_GTCR, 0, RZG2L_GTCR_CST);

	rzg2l_gpt->enable_count[ch]++;
	mutex_unlock(&rzg2l_gpt->lock);

	return 0;
}

static void rzg2l_gpt_disable(struct rzg2l_gpt_chip *rzg2l_gpt,
			      struct pwm_device *pwm)
{
	u8 ch = RZG2L_GET_CH(pwm->hwpwm);
	u32 offs = RZG2L_GET_CH_OFFS(ch);

	/* Disable pin output */
	if (RZG2L_IS_IOB(pwm->hwpwm))
		rzg2l_gpt_modify(rzg2l_gpt, offs + RZG2L_GTIOR, RZG2L_GTIOR_OBE, 0);
	else
		rzg2l_gpt_modify(rzg2l_gpt, offs + RZG2L_GTIOR, RZG2L_GTIOR_OAE, 0);

	/* Stop count, Output low on GTIOCx pin when counting stops */
	mutex_lock(&rzg2l_gpt->lock);
	rzg2l_gpt->enable_count[ch]--;
	if (!rzg2l_gpt->enable_count[ch])
		rzg2l_gpt_modify(rzg2l_gpt, offs + RZG2L_GTCR, RZG2L_GTCR_CST, 0);

	mutex_unlock(&rzg2l_gpt->lock);

	/*
	 * Probe() set these bits, if pwm is enabled by bootloader. In such
	 * case, clearing the bits will avoid errors during unbind.
	 */
	if (test_bit(pwm->hwpwm, rzg2l_gpt->ch_en_bits))
		clear_bit(pwm->hwpwm, rzg2l_gpt->ch_en_bits);
}

static u64 calculate_period_or_duty(struct rzg2l_gpt_chip *rzg2l_gpt, u32 val, u8 prescale)
{
	u64 retval;
	u64 tmp;

	tmp = NSEC_PER_SEC * (u64)val;
	/*
	 * To avoid losing precision for smaller period/duty cycle values
	 * ((2^32 * 10^9 << 2) < 2^64), do not process the rounded values.
	 */
	if (prescale < 2)
		retval = DIV64_U64_ROUND_UP(tmp << (2 * prescale), rzg2l_gpt->rate);
	else
		retval = DIV64_U64_ROUND_UP(tmp, rzg2l_gpt->rate) << (2 * prescale);

	return retval;
}

static int rzg2l_gpt_get_state(struct pwm_chip *chip, struct pwm_device *pwm,
			       struct pwm_state *state)
{
	struct rzg2l_gpt_chip *rzg2l_gpt = to_rzg2l_gpt_chip(chip);
	int rc;

	rc = pm_runtime_resume_and_get(chip->dev);
	if (rc)
		return rc;

	state->enabled = rzg2l_gpt_is_ch_enabled(rzg2l_gpt, pwm->hwpwm);
	if (state->enabled) {
		u32 ch = RZG2L_GET_CH(pwm->hwpwm);
		u32 offs = RZG2L_GET_CH_OFFS(ch);
		u8 prescale;
		u32 val;

		val = rzg2l_gpt_read(rzg2l_gpt, offs + RZG2L_GTCR);
		prescale = FIELD_GET(RZG2L_GTCR_TPCS, val);

		val = rzg2l_gpt_read(rzg2l_gpt, offs + RZG2L_GTPR);
		state->period = calculate_period_or_duty(rzg2l_gpt, val, prescale);

		val = rzg2l_gpt_read(rzg2l_gpt,
				     offs + RZG2L_GTCCR(RZG2L_IS_IOB(pwm->hwpwm)));
		state->duty_cycle = calculate_period_or_duty(rzg2l_gpt, val, prescale);
		if (state->duty_cycle > state->period)
			state->duty_cycle = state->period;
	}

	state->polarity = PWM_POLARITY_NORMAL;
	pm_runtime_put(chip->dev);

	return 0;
}

static u32 rzg2l_gpt_calculate_pv_or_dc(u64 period_or_duty_cycle, u8 prescale)
{
	return min_t(u64, DIV64_U64_ROUND_UP(period_or_duty_cycle, 1 << (2 * prescale)),
		     (u64)U32_MAX);
}

/* Caller holds the lock while calling rzg2l_gpt_config() */
static int rzg2l_gpt_config(struct pwm_chip *chip, struct pwm_device *pwm,
			    const struct pwm_state *state)
{
	struct rzg2l_gpt_chip *rzg2l_gpt = to_rzg2l_gpt_chip(chip);
	u8 ch = RZG2L_GET_CH(pwm->hwpwm);
	u32 offs = RZG2L_GET_CH_OFFS(ch);
	unsigned long pv, dc;
	u64 period_cycles;
	u64 duty_cycles;
	u8 prescale;

	/*
	 * GPT counter is shared by multiple channels, so prescale and period
	 * can NOT be modified when there are multiple channels in use with
	 * different settings.
	 */
	if (state->period != rzg2l_gpt->state_period[ch] && rzg2l_gpt->user_count[ch] > 1)
		return -EBUSY;

	/* Limit period/duty cycle to max value supported by the HW */
	if (state->period > rzg2l_gpt->max_val)
		period_cycles = rzg2l_gpt->max_val;
	else
		period_cycles = state->period;

	period_cycles = mul_u64_u32_div(period_cycles, rzg2l_gpt->rate, NSEC_PER_SEC);
	prescale = rzg2l_gpt_calculate_prescale(rzg2l_gpt, period_cycles);

	pv = rzg2l_gpt_calculate_pv_or_dc(period_cycles, prescale);

	if (state->duty_cycle > rzg2l_gpt->max_val)
		duty_cycles = rzg2l_gpt->max_val;
	else
		duty_cycles = state->duty_cycle;

	duty_cycles = mul_u64_u32_div(duty_cycles, rzg2l_gpt->rate, NSEC_PER_SEC);
	dc = rzg2l_gpt_calculate_pv_or_dc(duty_cycles, prescale);

	/*
	 * GPT counter is shared by multiple channels, we cache the period value
	 * from the first enabled channel and use the same value for both
	 * channels.
	 */
	rzg2l_gpt->state_period[ch] = state->period;

	/*
	 * Counter must be stopped before modifying mode, prescaler, timer
	 * counter and buffer enable registers. These registers are shared
	 * between both channels. So allow updating these registers only for the
	 * first enabled channel.
	 */
	if (rzg2l_gpt->enable_count[ch] <= 1)
		rzg2l_gpt_modify(rzg2l_gpt, offs + RZG2L_GTCR, RZG2L_GTCR_CST, 0);

	/* GPT set operating mode (saw-wave up-counting) */
	rzg2l_gpt_modify(rzg2l_gpt, offs + RZG2L_GTCR, RZG2L_GTCR_MD,
			 RZG2L_GTCR_MD_SAW_WAVE_PWM_MODE);

	/* Set count direction */
	rzg2l_gpt_write(rzg2l_gpt, offs + RZG2L_GTUDDTYC, RZG2L_UP_COUNTING);
	/* Select count clock */
	rzg2l_gpt_modify(rzg2l_gpt, offs + RZG2L_GTCR, RZG2L_GTCR_TPCS,
			 FIELD_PREP(RZG2L_GTCR_TPCS, prescale));

	/* Set period */
	rzg2l_gpt_write(rzg2l_gpt, offs + RZG2L_GTPR, pv);

	/* Set duty cycle */
	rzg2l_gpt_write(rzg2l_gpt, offs + RZG2L_GTCCR(RZG2L_IS_IOB(pwm->hwpwm)),
			dc);

	/* Set initial value for counter */
	rzg2l_gpt_write(rzg2l_gpt, offs + RZG2L_GTCNT, 0);

	/* Set no buffer operation */
	rzg2l_gpt_write(rzg2l_gpt, offs + RZG2L_GTBER, 0);

	/* Restart the counter after updating the registers */
	if (rzg2l_gpt->enable_count[ch] <= 1)
		rzg2l_gpt_modify(rzg2l_gpt, offs + RZG2L_GTCR,
				 RZG2L_GTCR_CST, RZG2L_GTCR_CST);

	return 0;
}

static int rzg2l_gpt_apply(struct pwm_chip *chip, struct pwm_device *pwm,
			   const struct pwm_state *state)
{
	struct rzg2l_gpt_chip *rzg2l_gpt = to_rzg2l_gpt_chip(chip);
	bool enabled = pwm->state.enabled;
	int ret;

	if (state->polarity != PWM_POLARITY_NORMAL)
		return -EINVAL;

	if (!state->enabled) {
		if (enabled) {
			rzg2l_gpt_disable(rzg2l_gpt, pwm);
			pm_runtime_put_sync(rzg2l_gpt->chip.dev);
		}

		return 0;
	}

	if (!enabled) {
		ret = pm_runtime_resume_and_get(rzg2l_gpt->chip.dev);
		if (ret)
			return ret;
	}

	mutex_lock(&rzg2l_gpt->lock);
	ret = rzg2l_gpt_config(chip, pwm, state);
	mutex_unlock(&rzg2l_gpt->lock);
	if (ret)
		return ret;

	if (!enabled)
		ret = rzg2l_gpt_enable(rzg2l_gpt, pwm);

	return ret;
}

static const struct pwm_ops rzg2l_gpt_ops = {
	.request = rzg2l_gpt_request,
	.free = rzg2l_gpt_free,
	.get_state = rzg2l_gpt_get_state,
	.apply = rzg2l_gpt_apply,
};

static int rzg2l_gpt_pm_runtime_suspend(struct device *dev)
{
	struct rzg2l_gpt_chip *rzg2l_gpt = dev_get_drvdata(dev);

	clk_disable_unprepare(rzg2l_gpt->clk);

	return 0;
}

static int rzg2l_gpt_pm_runtime_resume(struct device *dev)
{
	struct rzg2l_gpt_chip *rzg2l_gpt = dev_get_drvdata(dev);

	return clk_prepare_enable(rzg2l_gpt->clk);
}

static DEFINE_RUNTIME_DEV_PM_OPS(rzg2l_gpt_pm_ops,
				 rzg2l_gpt_pm_runtime_suspend,
				 rzg2l_gpt_pm_runtime_resume, NULL);

static void rzg2l_gpt_reset_assert_pm_disable(void *data)
{
	struct rzg2l_gpt_chip *rzg2l_gpt = data;
	u32 i;

	clk_rate_exclusive_put(rzg2l_gpt->clk);
	/*
	 * The below check is for making balanced PM usage count
	 * eg: boot loader is turning on PWM and probe increments the PM usage
	 * count. Before apply, if there is unbind/remove callback we need to
	 * decrement the PM usage count.
	 */
	for (i = 0; i < RZG2L_MAX_PWM_CHANNELS; i++) {
		if (test_bit(i, rzg2l_gpt->ch_en_bits))
			pm_runtime_put(rzg2l_gpt->chip.dev);
	}

	pm_runtime_disable(rzg2l_gpt->chip.dev);
	pm_runtime_set_suspended(rzg2l_gpt->chip.dev);
	reset_control_assert(rzg2l_gpt->rstc);
}

/*
 * This function links a poeg group{A,B,C,D} with a gpt channel{0..7} and
 * configure the pin for output disable.
 */
static void rzg2l_gpt_poeg_init(struct platform_device *pdev,
				struct rzg2l_gpt_chip *rzg2l_gpt)
{
	struct of_phandle_args of_args;
	unsigned int i;
	u32 poeg_grp;
	u32 bitpos;
	int cells;
	u32 offs;
	int ret;

	cells = of_property_count_u32_elems(pdev->dev.of_node, "renesas,poegs");
	if (cells == -EINVAL)
		return;

	cells >>= 1;
	for (i = 0; i < cells; i++) {
		ret = of_parse_phandle_with_fixed_args(pdev->dev.of_node,
						       "renesas,poegs", 1, i,
						       &of_args);
		if (ret) {
			dev_err(&pdev->dev,
				"Failed to parse 'renesas,poegs' property\n");
			return;
		}

		if (of_args.args[0] >= RZG2L_MAX_HW_CHANNELS) {
			dev_err(&pdev->dev, "Invalid channel %d >= %d\n",
				of_args.args[0], RZG2L_MAX_HW_CHANNELS);
			of_node_put(of_args.np);
			return;
		}

		bitpos = of_args.args[0];
		if (!of_device_is_available(of_args.np)) {
			/* It's fine to have a phandle to a non-enabled poeg. */
			of_node_put(of_args.np);
			continue;
		}

		if (!of_property_read_u32(of_args.np, "renesas,poeg-id", &poeg_grp)) {
			offs = RZG2L_GET_CH_OFFS(of_args.args[0]);
			if (poeg_grp > RZG2L_LAST_POEG_GROUP) {
				dev_err(&pdev->dev, "Invalid poeg group %d > %d\n",
					poeg_grp, RZG2L_LAST_POEG_GROUP);
				of_node_put(of_args.np);
				return;
			}

			bitpos += poeg_grp * RZG2L_MAX_HW_CHANNELS;
			set_bit(bitpos, rzg2l_gpt->poeg_gpt_link);

			rzg2l_gpt_modify(rzg2l_gpt, offs + RZG2L_GTINTAD,
					 RZG2L_GTINTAD_GRP_MASK,
					 poeg_grp << 24);

			rzg2l_gpt_modify(rzg2l_gpt, offs + RZG2L_GTIOR,
					 RZG2L_GTIOR_OBDF | RZG2L_GTIOR_OADF,
					 RZG2L_GTIOR_PIN_DISABLE_SETTING);
		}

		of_node_put(of_args.np);
	}
}

static int rzg2l_gpt_probe(struct platform_device *pdev)
{
	struct rzg2l_gpt_chip *rzg2l_gpt;
	int ret;
	u32 i;

	rzg2l_gpt = devm_kzalloc(&pdev->dev, sizeof(*rzg2l_gpt), GFP_KERNEL);
	if (!rzg2l_gpt)
		return -ENOMEM;

	rzg2l_gpt->mmio = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(rzg2l_gpt->mmio))
		return PTR_ERR(rzg2l_gpt->mmio);

	rzg2l_gpt->rstc = devm_reset_control_get_exclusive(&pdev->dev, NULL);
	if (IS_ERR(rzg2l_gpt->rstc))
		return dev_err_probe(&pdev->dev, PTR_ERR(rzg2l_gpt->rstc),
				     "get reset failed\n");

	rzg2l_gpt->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(rzg2l_gpt->clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(rzg2l_gpt->clk),
				     "cannot get clock\n");

	ret = reset_control_deassert(rzg2l_gpt->rstc);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "cannot deassert reset control\n");

	ret = clk_prepare_enable(rzg2l_gpt->clk);
	if (ret)
		goto err_reset;

	ret = clk_rate_exclusive_get(rzg2l_gpt->clk);
	if (ret)
		goto err_clk_disable;

	rzg2l_gpt->rate = clk_get_rate(rzg2l_gpt->clk);
	if (!rzg2l_gpt->rate) {
		ret = dev_err_probe(&pdev->dev, -EINVAL, "gpt clk rate is 0");
		goto err_clk_rate_put;
	}

	/*
	 * Refuse clk rates > 1 GHz to prevent overflow later for computing
	 * period and duty cycle.
	 */
	if (rzg2l_gpt->rate > NSEC_PER_SEC) {
		ret = -EINVAL;
		goto err_clk_rate_put;
	}

	rzg2l_gpt->max_val = mul_u64_u64_div_u64(U32_MAX, NSEC_PER_SEC,
						 rzg2l_gpt->rate) * RZG2L_MAX_SCALE_FACTOR;

	/*
	 *  We need to keep the clock on, in case the bootloader has enabled the
	 *  PWM and is running during probe().
	 */
	for (i = 0; i < RZG2L_MAX_PWM_CHANNELS; i++) {
		if (rzg2l_gpt_is_ch_enabled(rzg2l_gpt, i)) {
			set_bit(i, rzg2l_gpt->ch_en_bits);
			pm_runtime_get_sync(&pdev->dev);
		}
	}

	rzg2l_gpt_poeg_init(pdev, rzg2l_gpt);
	mutex_init(&rzg2l_gpt->lock);
	platform_set_drvdata(pdev, rzg2l_gpt);
	rzg2l_gpt->chip.dev = &pdev->dev;

	pm_runtime_set_active(&pdev->dev);
	pm_runtime_enable(&pdev->dev);
	ret = devm_add_action_or_reset(&pdev->dev,
				       rzg2l_gpt_reset_assert_pm_disable,
				       rzg2l_gpt);
	if (ret < 0)
		return ret;

	rzg2l_gpt->chip.ops = &rzg2l_gpt_ops;
	rzg2l_gpt->chip.npwm = RZG2L_MAX_PWM_CHANNELS;
	ret = devm_pwmchip_add(&pdev->dev, &rzg2l_gpt->chip);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "failed to add PWM chip\n");

	pm_runtime_idle(&pdev->dev);

	return 0;

err_clk_rate_put:
	clk_rate_exclusive_put(rzg2l_gpt->clk);
err_clk_disable:
	clk_disable_unprepare(rzg2l_gpt->clk);
err_reset:
	reset_control_assert(rzg2l_gpt->rstc);
	return ret;
}

static const struct of_device_id rzg2l_gpt_of_table[] = {
	{ .compatible = "renesas,rzg2l-gpt", },
	{ /* Sentinel */ }
};
MODULE_DEVICE_TABLE(of, rzg2l_gpt_of_table);

static struct platform_driver rzg2l_gpt_driver = {
	.driver = {
		.name = "pwm-rzg2l-gpt",
		.pm = pm_ptr(&rzg2l_gpt_pm_ops),
		.of_match_table = rzg2l_gpt_of_table,
	},
	.probe = rzg2l_gpt_probe,
};
module_platform_driver(rzg2l_gpt_driver);

MODULE_AUTHOR("Biju Das <biju.das.jz@bp.renesas.com>");
MODULE_DESCRIPTION("Renesas RZ/G2L General PWM Timer (GPT) Driver");
MODULE_LICENSE("GPL");
