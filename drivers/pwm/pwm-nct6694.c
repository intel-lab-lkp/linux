// SPDX-License-Identifier: GPL-2.0
/*
 * Nuvoton NCT6694 PWM driver based on USB interface.
 *
 * Copyright (C) 2024 Nuvoton Technology Corp.
 */

#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pwm.h>
#include <linux/platform_device.h>
#include <linux/mfd/nct6694.h>

#define DRVNAME "nct6694-pwm"

#define NR_PWM	10
#define MAX_PERIOD_NS	40000		/* PWM Maximum Frequency = 25kHz */
#define PERIOD_NS_CONST	10200000	/* Period_ns to Freq_reg */

/* Host interface */
#define REQUEST_RPT_MOD			0xFF
#define REQUEST_HWMON_MOD		0x00
#define REQUEST_PWM_MOD			0x01

/* Report Channel */
#define HWMON_PWM_IDX(x)		(0x70 + (x))

/* Message Channel -HWMON */
/* Command 00h */
#define REQUEST_HWMON_CMD0_LEN		0x40
#define REQUEST_HWMON_CMD0_OFFSET	0x0000	/* OFFSET = SEL|CMD */
#define HWMON_PWM_EN(x)			(0x06 + (x))
#define HWMON_PWM_PP(x)			(0x08 + (x))
#define HWMON_PWM_FREQ_IDX(x)		(0x30 + (x))

/* Message Channel -FAN */
/* Command 00h */
#define REQUEST_PWM_CMD0_LEN		0x08
#define REQUEST_PWM_CMD0_OFFSET		0x0000	/* OFFSET = SEL|CMD */
#define PWM_CH_EN(x)			(x ? 0x00 : 0x01)
/* Command 01h */
#define REQUEST_PWM_CMD1_LEN		0x20
#define REQUEST_PWM_CMD1_OFFSET		0x0001	/* OFFSET = SEL|CMD */
#define PWM_MAL_EN(x)			(x ? 0x00 : 0x01)
#define PWM_MAL_VAL(x)			(0x02 + (x))

/*
 *		Frequency <-> Period
 * (10^9 * 255) / (25000 * Freq_reg) = Period_ns
 *		10200000 / Freq_reg  = Period_ns
 *
 * | Freq_reg | Freq_Hz | Period_ns |
 * |  1 (01h  |  98.039 |  10200000 |
 * |  2 (02h) | 196.078 |   5100000 |
 * |  3 (03h) | 294.117 |   3400000 |
 * |		  ...		    |
 * |		  ...		    |
 * |		  ...		    |
 * | 253 (FDh)| 24803.9 |  40316.20 |
 * | 254 (FEh)| 24901.9 |  40157.48 |
 * | 255 (FFh)|  25000  |    40000  |
 *
 */

struct nct6694_pwm_data {
	struct nct6694 *nct6694;
	unsigned char hwmon_cmd0_buf[REQUEST_HWMON_CMD0_LEN];
	unsigned char pwm_cmd0_buf[REQUEST_PWM_CMD0_LEN];
	unsigned char pwm_cmd1_buf[REQUEST_PWM_CMD1_LEN];
};

static inline struct nct6694_pwm_data *to_nct6694_pwm_data(struct pwm_chip *chip)
{
	return pwmchip_get_drvdata(chip);
}

static int nct6694_pwm_request(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct nct6694_pwm_data *data = to_nct6694_pwm_data(chip);
	unsigned char ch_enable = data->pwm_cmd0_buf[PWM_CH_EN(pwm->hwpwm / 8)];
	unsigned char mal_enable = data->pwm_cmd1_buf[PWM_MAL_EN(pwm->hwpwm / 8)];
	bool ch_en = ch_enable & BIT(pwm->hwpwm % 8);
	bool mal_en = mal_enable & BIT(pwm->hwpwm % 8);

	if (!(ch_en && mal_en)) {
		pr_err("%s: PWM(%d) is running in other mode!\n",
		       __func__, pwm->hwpwm);
		return -EINVAL;
	}

	return 0;
}

static int nct6694_pwm_get_state(struct pwm_chip *chip,
				 struct pwm_device *pwm,
				 struct pwm_state *state)
{
	struct nct6694_pwm_data *data = to_nct6694_pwm_data(chip);
	unsigned char freq_reg, duty;

	/* Get pwm device initial state */
	state->enabled = true;

	freq_reg = data->hwmon_cmd0_buf[HWMON_PWM_FREQ_IDX(pwm->hwpwm)];
	state->period = PERIOD_NS_CONST / freq_reg;

	duty = data->pwm_cmd1_buf[PWM_MAL_VAL(pwm->hwpwm)];
	state->duty_cycle = duty * state->period / 0xFF;

	return 0;
}

static int nct6694_pwm_apply(struct pwm_chip *chip,
			     struct pwm_device *pwm,
			     const struct pwm_state *state)
{
	struct nct6694_pwm_data *data = to_nct6694_pwm_data(chip);
	unsigned char freq_reg, duty;
	int ret;

	if (state->period < MAX_PERIOD_NS)
		return -EINVAL;

	/* (10^9 * 255) / (25000 * Freq_reg) = Period_ns */
	freq_reg = (unsigned char)(PERIOD_NS_CONST / state->period);
	data->hwmon_cmd0_buf[HWMON_PWM_FREQ_IDX(pwm->hwpwm)] = freq_reg;
	ret = nct6694_write_msg(data->nct6694, REQUEST_HWMON_MOD,
				REQUEST_HWMON_CMD0_OFFSET,
				REQUEST_HWMON_CMD0_LEN,
				data->hwmon_cmd0_buf);
	if (ret)
		return -EIO;

	/* Duty = duty * 0xFF */
	duty = (unsigned char)(state->duty_cycle * 0xFF / state->period);
	data->pwm_cmd1_buf[PWM_MAL_VAL(pwm->hwpwm)] = duty;
	if (state->enabled)
		data->pwm_cmd1_buf[PWM_MAL_EN(pwm->hwpwm / 8)] |= BIT(pwm->hwpwm  % 8);
	else
		data->pwm_cmd1_buf[PWM_MAL_EN(pwm->hwpwm / 8)] &= ~BIT(pwm->hwpwm  % 8);
	ret = nct6694_write_msg(data->nct6694, REQUEST_PWM_MOD,
				REQUEST_PWM_CMD1_OFFSET, REQUEST_PWM_CMD1_LEN,
				data->pwm_cmd1_buf);
	if (ret)
		return -EIO;

	return 0;
}

static const struct pwm_ops nct6694_pwm_ops = {
	.request = nct6694_pwm_request,
	.apply = nct6694_pwm_apply,
	.get_state = nct6694_pwm_get_state,
};

static int nct6694_pwm_init(struct nct6694_pwm_data *data)
{
	struct nct6694 *nct6694 = data->nct6694;
	int ret;

	ret = nct6694_read_msg(nct6694, REQUEST_HWMON_MOD,
			       REQUEST_HWMON_CMD0_OFFSET,
			       REQUEST_HWMON_CMD0_LEN, 0,
			       REQUEST_HWMON_CMD0_LEN,
			       data->hwmon_cmd0_buf);
	if (ret)
		return ret;

	ret = nct6694_read_msg(nct6694, REQUEST_PWM_MOD,
			       REQUEST_PWM_CMD0_OFFSET,
			       REQUEST_PWM_CMD0_LEN, 0,
			       REQUEST_PWM_CMD0_LEN,
			       data->pwm_cmd0_buf);
	if (ret)
		return ret;

	ret = nct6694_read_msg(nct6694, REQUEST_PWM_MOD,
			       REQUEST_PWM_CMD1_OFFSET,
			       REQUEST_PWM_CMD1_LEN, 0,
			       REQUEST_PWM_CMD1_LEN,
			       data->pwm_cmd1_buf);
	return ret;
}

static int nct6694_pwm_probe(struct platform_device *pdev)
{
	struct pwm_chip *chip;
	struct nct6694_pwm_data *data;
	struct nct6694 *nct6694 = dev_get_drvdata(pdev->dev.parent);
	int ret;

	chip = devm_pwmchip_alloc(&pdev->dev, NR_PWM, sizeof(*data));
	if (IS_ERR(chip))
		return PTR_ERR(chip);

	data = to_nct6694_pwm_data(chip);

	data->nct6694 = nct6694;
	chip->ops = &nct6694_pwm_ops;

	ret = nct6694_pwm_init(data);
	if (ret)
		return -EIO;

	/* Register pwm device to PWM framework */
	ret = devm_pwmchip_add(&pdev->dev, chip);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register pwm device!\n");
		return ret;
	}

	return 0;
}

static struct platform_driver nct6694_pwm_driver = {
	.driver = {
		.name	= DRVNAME,
	},
	.probe		= nct6694_pwm_probe,
};

static int __init nct6694_init(void)
{
	int err;

	err = platform_driver_register(&nct6694_pwm_driver);
	if (!err) {
		if (err)
			platform_driver_unregister(&nct6694_pwm_driver);
	}

	return err;
}
subsys_initcall(nct6694_init);

static void __exit nct6694_exit(void)
{
	platform_driver_unregister(&nct6694_pwm_driver);
}
module_exit(nct6694_exit);

MODULE_DESCRIPTION("USB-PWM driver for NCT6694");
MODULE_AUTHOR("Ming Yu <tmyu0@nuvoton.com>");
MODULE_LICENSE("GPL");
