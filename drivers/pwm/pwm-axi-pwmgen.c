// SPDX-License-Identifier: GPL-2.0
/*
 * Analog Devices AXI PWM generator
 *
 * Copyright 2024 Analog Devices Inc.
 * Copyright 2024 Baylibre SAS
 */
#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#define AXI_PWMGEN_NPWM			4
#define AXI_PWMGEN_REG_CORE_VERSION	0x00
#define AXI_PWMGEN_REG_ID		0x04
#define AXI_PWMGEN_REG_SCRATCHPAD	0x08
#define AXI_PWMGEN_REG_CORE_MAGIC	0x0C
#define AXI_PWMGEN_REG_CONFIG		0x10
#define AXI_PWMGEN_REG_NPWM		0x14
#define AXI_PWMGEN_CH_PERIOD_BASE	0x40
#define AXI_PWMGEN_CH_DUTY_BASE		0x44
#define AXI_PWMGEN_CH_OFFSET_BASE	0x48
#define AXI_PWMGEN_CHX_PERIOD(ch)	(AXI_PWMGEN_CH_PERIOD_BASE + (12 * (ch)))
#define AXI_PWMGEN_CHX_DUTY(ch)		(AXI_PWMGEN_CH_DUTY_BASE + (12 * (ch)))
#define AXI_PWMGEN_CHX_OFFSET(ch)	(AXI_PWMGEN_CH_OFFSET_BASE + (12 * (ch)))
#define AXI_PWMGEN_TEST_DATA		0x5A0F0081
#define AXI_PWMGEN_LOAD_CONFIG		BIT(1)
#define AXI_PWMGEN_RESET		BIT(0)
#define AXI_PWMGEN_MAX_REGISTER		0x6C

struct axi_pwmgen {
	struct pwm_chip		chip;
	struct clk		*clk;
	struct regmap		*regmap;

	/* Used to store the period when the channel is disabled */
	unsigned int		ch_period[AXI_PWMGEN_NPWM];
	bool			ch_enabled[AXI_PWMGEN_NPWM];
};

static const struct regmap_config axi_pwm_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.max_register = AXI_PWMGEN_MAX_REGISTER,
};

static struct axi_pwmgen *to_axi_pwmgen(struct pwm_chip *chip)
{
	return container_of(chip, struct axi_pwmgen, chip);
}

static int axi_pwmgen_apply(struct pwm_chip *chip, struct pwm_device *device,
			    const struct pwm_state *state)
{
	struct axi_pwmgen *pwm = to_axi_pwmgen(chip);
	unsigned long clk_rate_hz = clk_get_rate(pwm->clk);
	unsigned int ch = device->hwpwm;
	struct regmap *regmap = pwm->regmap;
	u64 period_cnt, duty_cnt;
	int ret;

	if (!clk_rate_hz)
		return -EINVAL;

	period_cnt = DIV_ROUND_UP_ULL(state->period * clk_rate_hz, NSEC_PER_SEC);
	if (period_cnt > UINT_MAX)
		return -EINVAL;

	pwm->ch_period[ch] = period_cnt;
	pwm->ch_enabled[ch] = state->enabled;
	ret = regmap_write(regmap, AXI_PWMGEN_CHX_PERIOD(ch), state->enabled ? period_cnt : 0);
	if (ret)
		return ret;

	duty_cnt = DIV_ROUND_UP_ULL(state->duty_cycle * clk_rate_hz, NSEC_PER_SEC);
	ret = regmap_write(regmap, AXI_PWMGEN_CHX_DUTY(ch), duty_cnt);
	if (ret)
		return ret;

	return regmap_write(regmap, AXI_PWMGEN_REG_CONFIG, AXI_PWMGEN_LOAD_CONFIG);
}

static int axi_pwmgen_get_state(struct pwm_chip *chip, struct pwm_device *device,
				struct pwm_state *state)
{
	struct axi_pwmgen *pwm = to_axi_pwmgen(chip);
	unsigned long clk_rate_hz = clk_get_rate(pwm->clk);
	struct regmap *regmap = pwm->regmap;
	unsigned int ch = device->hwpwm;
	u32 cnt;
	int ret;

	if (!clk_rate_hz) {
		dev_err(device->chip->dev, "axi pwm clock has no frequency\n");
		return -EINVAL;
	}

	state->enabled = pwm->ch_enabled[ch];

	if (state->enabled) {
		ret = regmap_read(regmap, AXI_PWMGEN_CHX_PERIOD(ch), &cnt);
		if (ret)
			return ret;
	} else {
		cnt = pwm->ch_period[ch];
	}

	state->period = DIV_ROUND_CLOSEST_ULL((u64)cnt * NSEC_PER_SEC, clk_rate_hz);

	ret = regmap_read(regmap, AXI_PWMGEN_CHX_DUTY(ch), &cnt);
	if (ret)
		return ret;

	state->duty_cycle = DIV_ROUND_CLOSEST_ULL((u64)cnt * NSEC_PER_SEC, clk_rate_hz);

	return 0;
}

static const struct pwm_ops axi_pwmgen_pwm_ops = {
	.apply = axi_pwmgen_apply,
	.get_state = axi_pwmgen_get_state,
};

static int axi_pwmgen_setup(struct axi_pwmgen *pwm, struct device *dev)
{
	struct regmap *regmap = pwm->regmap;
	int idx;
	int ret;
	u32 val;

	ret = regmap_write(regmap, AXI_PWMGEN_REG_SCRATCHPAD, AXI_PWMGEN_TEST_DATA);
	if (ret)
		return ret;

	ret = regmap_read(regmap, AXI_PWMGEN_REG_SCRATCHPAD, &val);
	if (ret)
		return ret;

	if (val != AXI_PWMGEN_TEST_DATA)
		return dev_err_probe(dev, -EIO, "failed to access the device registers\n");

	ret = regmap_read(regmap, AXI_PWMGEN_REG_NPWM, &pwm->chip.npwm);
	if (ret)
		return ret;

	if (pwm->chip.npwm > AXI_PWMGEN_NPWM) {
		dev_warn(dev, "driver is limited to %d channels but hardware reported %u\n",
				AXI_PWMGEN_NPWM, pwm->chip.npwm);
		pwm->chip.npwm = AXI_PWMGEN_NPWM;
	}

	/* Disable all the outputs */
	for (idx = 0; idx < pwm->chip.npwm; idx++) {
		ret = regmap_write(regmap, AXI_PWMGEN_CHX_PERIOD(idx), 0);
		if (ret)
			return ret;

		ret = regmap_write(regmap, AXI_PWMGEN_CHX_DUTY(idx), 0);
		if (ret)
			return ret;

		ret = regmap_write(regmap, AXI_PWMGEN_CHX_OFFSET(idx), 0);
		if (ret)
			return ret;
	}

	/* Enable the core */
	return regmap_update_bits(regmap, AXI_PWMGEN_REG_CONFIG, AXI_PWMGEN_RESET, 0);
}

static int axi_pwmgen_probe(struct platform_device *pdev)
{
	struct axi_pwmgen *pwm;
	void __iomem *io_base;
	int ret;

	pwm = devm_kzalloc(&pdev->dev, sizeof(*pwm), GFP_KERNEL);
	if (!pwm)
		return -ENOMEM;

	io_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(io_base))
		return PTR_ERR(io_base);

	pwm->regmap = devm_regmap_init_mmio(&pdev->dev, io_base, &axi_pwm_regmap_config);
	if (IS_ERR(pwm->regmap))
		return dev_err_probe(&pdev->dev, PTR_ERR(pwm->regmap),
				     "failed to init register map\n");

	pwm->clk = devm_clk_get_enabled(&pdev->dev, NULL);
	if (IS_ERR(pwm->clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(pwm->clk), "failed to get clock\n");

	pwm->chip.dev = &pdev->dev;
	pwm->chip.ops = &axi_pwmgen_pwm_ops;
	pwm->chip.base = -1;

	ret = axi_pwmgen_setup(pwm, &pdev->dev);
	if (ret < 0)
		return ret;

	return devm_pwmchip_add(&pdev->dev, &pwm->chip);
}

static const struct of_device_id axi_pwmgen_ids[] = {
	{ .compatible = "adi,axi-pwmgen-1.00.a" },
	{ }
};
MODULE_DEVICE_TABLE(of, axi_pwmgen_ids);

static struct platform_driver axi_pwmgen_driver = {
	.driver = {
		.name = "axi-pwmgen",
		.of_match_table = axi_pwmgen_ids,
	},
	.probe = axi_pwmgen_probe,
};

module_platform_driver(axi_pwmgen_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Sergiu Cuciurean <sergiu.cuciurean@analog.com>");
MODULE_DESCRIPTION("Driver for the Analog Devices AXI PWM generator");
