// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2025 Marek Vasut
 */

#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pwm.h>

static int argon_fan_hat_pwm_apply(struct pwm_chip *chip, struct pwm_device *pwm,
				   const struct pwm_state *state)
{
	struct i2c_client *i2c = pwmchip_get_drvdata(chip);
	u8 tx[2] = { 0x80, state->enabled ? pwm_get_relative_duty_cycle(state, 100) : 0 };
	struct i2c_msg msg = {
		.addr = i2c->addr,
		.len = 2,
		.buf = tx,
	};

	if (state->polarity != PWM_POLARITY_NORMAL)
		return -EINVAL;

	return (i2c_transfer(i2c->adapter, &msg, 1) == 1) ? 0 : -EINVAL;
}

static const struct pwm_ops argon_fan_hat_pwm_ops = {
	.apply = argon_fan_hat_pwm_apply,
};

static int argon_fan_hat_i2c_probe(struct i2c_client *i2c)
{
	struct pwm_chip *pc = devm_pwmchip_alloc(&i2c->dev, 1, 0);

	if (IS_ERR(pc))
		return PTR_ERR(pc);

	pc->ops = &argon_fan_hat_pwm_ops;
	pwmchip_set_drvdata(pc, i2c);

	return devm_pwmchip_add(&i2c->dev, pc);
}

static const struct of_device_id argon_fan_hat_dt_ids[] = {
	{ .compatible = "argon40,fan-hat" },
	{ },
};
MODULE_DEVICE_TABLE(of, argon_fan_hat_dt_ids);

static struct i2c_driver argon_fan_hat_driver = {
	.driver = {
		.name = "argon-fan-hat",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
		.of_match_table = argon_fan_hat_dt_ids,
	},
	.probe = argon_fan_hat_i2c_probe,
};

module_i2c_driver(argon_fan_hat_driver);

MODULE_AUTHOR("Marek Vasut <marek.vasut+renesas@mailbox.org>");
MODULE_DESCRIPTION("Argon40 Fan HAT");
MODULE_LICENSE("GPL");
