// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Aaeon MCU GPIO driver
 *
 * Copyright (C) 2025 Bootlin
 * Author: Jérémie Dautheribes <jeremie.dautheribes@bootlin.com>
 * Author: Thomas Perrot <thomas.perrot@bootlin.com>
 */

#include <linux/bitmap.h>
#include <linux/gpio/driver.h>
#include <linux/mfd/aaeon-mcu.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#define AAEON_MCU_CONFIG_GPIO_INPUT 0x69
#define AAEON_MCU_CONFIG_GPIO_OUTPUT 0x6F
#define AAEON_MCU_READ_GPIO 0x72
#define AAEON_MCU_WRITE_GPIO 0x77

#define AAEON_MCU_CONTROL_GPO 0x6C

#define MAX_GPIOS 12
#define MAX_GPOS 7

struct aaeon_mcu_gpio {
	struct gpio_chip gc;
	struct aaeon_mcu_dev *mfd;
	DECLARE_BITMAP(dir_in, MAX_GPOS + MAX_GPIOS);
	DECLARE_BITMAP(gpo_state, MAX_GPOS);
};

static int aaeon_mcu_gpio_config_input_cmd(struct aaeon_mcu_gpio *data,
					    unsigned int offset)
{
	u8 cmd[3], rsp;

	cmd[0] = AAEON_MCU_CONFIG_GPIO_INPUT;
	cmd[1] = offset - 7;
	cmd[2] = 0x00;

	return aaeon_mcu_i2c_xfer(data->mfd->i2c_client, cmd, 3, &rsp, 1);
}

static int aaeon_mcu_gpio_direction_input(struct gpio_chip *gc, unsigned int offset)
{
	struct aaeon_mcu_gpio *data = gpiochip_get_data(gc);
	int ret;

	if (offset < MAX_GPOS) {
		dev_err(gc->parent, "GPIO offset (%d) must be an output GPO\n", offset);
		return -EOPNOTSUPP;
	}

	ret = aaeon_mcu_gpio_config_input_cmd(data, offset);
	if (ret < 0)
		return ret;

	set_bit(offset, data->dir_in);

	return 0;
}

static int aaeon_mcu_gpio_config_output_cmd(struct aaeon_mcu_gpio *data,
					     unsigned int offset,
					     int value)
{
	u8 cmd[3], rsp;
	int ret;

	cmd[0] = AAEON_MCU_CONFIG_GPIO_OUTPUT;
	cmd[1] = offset - 7;
	cmd[2] = 0x00;

	ret = aaeon_mcu_i2c_xfer(data->mfd->i2c_client, cmd, 3, &rsp, 1);
	if (ret < 0)
		return ret;

	cmd[0] = AAEON_MCU_WRITE_GPIO;
	/* cmd[1] = offset - 7; */
	cmd[2] = !!value;

	return aaeon_mcu_i2c_xfer(data->mfd->i2c_client, cmd, 3, &rsp, 1);
}

static int aaeon_mcu_gpio_direction_output(struct gpio_chip *gc, unsigned int offset, int value)
{
	struct aaeon_mcu_gpio *data = gpiochip_get_data(gc);
	int ret;

	if (offset < MAX_GPOS)
		return 0;

	ret = aaeon_mcu_gpio_config_output_cmd(data, offset, value);
	if (ret < 0)
		return ret;

	clear_bit(offset, data->dir_in);

	return 0;
}

static int aaeon_mcu_gpio_get_direction(struct gpio_chip *gc, unsigned int offset)
{
	struct aaeon_mcu_gpio *data = gpiochip_get_data(gc);

	return test_bit(offset, data->dir_in) ?
		GPIO_LINE_DIRECTION_IN : GPIO_LINE_DIRECTION_OUT;
}

static int aaeon_mcu_gpio_get(struct gpio_chip *gc, unsigned int offset)
{
	struct aaeon_mcu_gpio *data = gpiochip_get_data(gc);
	u8 cmd[3], rsp;
	int ret;

	if (offset < MAX_GPOS)
		return test_bit(offset, data->gpo_state);

	cmd[0] = AAEON_MCU_READ_GPIO;
	cmd[1] = offset - 7;
	cmd[2] = 0x00;

	ret = aaeon_mcu_i2c_xfer(data->mfd->i2c_client, cmd, 3, &rsp, 1);
	if (ret < 0)
		return ret;

	return rsp;
}

static int aaeon_mcu_gpo_set_cmd(struct aaeon_mcu_gpio *data, unsigned int offset, int value)
{
	u8 cmd[3], rsp;

	cmd[0] = AAEON_MCU_CONTROL_GPO;
	cmd[1] = offset + 1;
	cmd[2] = !!value;

	return aaeon_mcu_i2c_xfer(data->mfd->i2c_client, cmd, 3, &rsp, 1);
}

static int aaeon_mcu_gpio_set_cmd(struct aaeon_mcu_gpio *data, unsigned int offset, int value)
{
	u8 cmd[3], rsp;

	cmd[0] = AAEON_MCU_WRITE_GPIO;
	cmd[1] = offset - 7;
	cmd[2] = !!value;

	return aaeon_mcu_i2c_xfer(data->mfd->i2c_client, cmd, 3, &rsp, 1);
}

static int aaeon_mcu_gpio_set(struct gpio_chip *gc, unsigned int offset,
			      int value)
{
	struct aaeon_mcu_gpio *data = gpiochip_get_data(gc);

	if (offset < MAX_GPOS) {
		if (aaeon_mcu_gpo_set_cmd(data, offset, value) == 0)
			assign_bit(offset, data->gpo_state, value);
	} else {
		return aaeon_mcu_gpio_set_cmd(data, offset, value);
	}
	return 0;
}

static const struct gpio_chip aaeon_mcu_chip = {
	.label			= "gpio-aaeon-mcu",
	.owner			= THIS_MODULE,
	.get_direction		= aaeon_mcu_gpio_get_direction,
	.direction_input	= aaeon_mcu_gpio_direction_input,
	.direction_output	= aaeon_mcu_gpio_direction_output,
	.get			= aaeon_mcu_gpio_get,
	.set			= aaeon_mcu_gpio_set,
	.base			= -1,
	.ngpio			= MAX_GPOS + MAX_GPIOS,
	.can_sleep		= true,
};

static void aaeon_mcu_gpio_reset(struct aaeon_mcu_gpio *data, struct device *dev)
{
	unsigned int i;
	int ret;

	/* Reset all GPOs */
	for (i = 0; i < MAX_GPOS; i++) {
		ret = aaeon_mcu_gpo_set_cmd(data, i, 0);
		if (ret < 0)
			dev_warn(dev, "Failed to reset GPO %u state: %d\n", i, ret);
		clear_bit(i, data->dir_in);
	}

	/* Reset all GPIOs */
	for (i = MAX_GPOS; i < MAX_GPOS + MAX_GPIOS; i++) {
		ret = aaeon_mcu_gpio_config_input_cmd(data, i);
		if (ret < 0)
			dev_warn(dev, "Failed to reset GPIO %u state: %d\n", i, ret);
		set_bit(i, data->dir_in);
	}
}

static int aaeon_mcu_gpio_probe(struct platform_device *pdev)
{
	struct aaeon_mcu_dev *mfd = dev_get_drvdata(pdev->dev.parent);
	struct aaeon_mcu_gpio *data;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->mfd = mfd;
	data->gc = aaeon_mcu_chip;
	data->gc.parent = &pdev->dev;

	/*
	 * Reset all GPIO states to a known configuration. The MCU does not
	 * reset GPIO state on soft reboot, only on power cycle (hard reboot).
	 * Without this reset, GPIOs would retain their previous state across
	 * reboots, which could lead to unexpected behavior.
	 */
	aaeon_mcu_gpio_reset(data, &pdev->dev);

	platform_set_drvdata(pdev, data);

	return devm_gpiochip_add_data(&pdev->dev, &data->gc,
				      data);
}

static const struct of_device_id aaeon_mcu_gpio_of_match[] = {
	{ .compatible = "aaeon,srg-imx8pl-gpio" },
	{},
};

MODULE_DEVICE_TABLE(of, aaeon_mcu_gpio_of_match);

static struct platform_driver aaeon_mcu_gpio_driver = {
	.driver = {
		.name = "aaeon-mcu-gpio",
		.of_match_table = aaeon_mcu_gpio_of_match,
	},
	.probe = aaeon_mcu_gpio_probe,
};

module_platform_driver(aaeon_mcu_gpio_driver);

MODULE_DESCRIPTION("GPIO interface for Aaeon MCU");
MODULE_AUTHOR("Jérémie Dautherbes <jeremie.dautheribes@bootlin.com>");
MODULE_LICENSE("GPL");
