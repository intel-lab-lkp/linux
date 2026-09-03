// SPDX-License-Identifier: GPL-2.0-only
/*
 * MFD core driver for the X-Powers AC200
 *
 * Copyright (C) 2019 Jernej Skrabec <jernej.skrabec@gmail.com>
 * Copyright (C) 2026 James Hilliard <james.hilliard1@gmail.com>
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/mfd/core.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>

#include <dt-bindings/mfd/x-powers,ac200.h>

#define AC200_SYS_CONTROL_REG			0x0002
#define AC200_SYS_CONTROL_CHIP_RESET_DEASSERT	BIT(0)
#define AC200_SYS_IRQ_ENABLE_REG		0x0004
#define AC200_SYS_IRQ_INTB_ENABLE		BIT(15)
#define AC200_SYS_IRQ_INTB_ACTIVE_HIGH		BIT(14)
#define AC200_SYS_IRQ_RTC			BIT(12)
#define AC200_SYS_IRQ_EPHY			BIT(8)
#define AC200_SYS_IRQ_TVE			BIT(4)
#define AC200_SYS_IRQ_STATUS_REG		0x0006

/* Interface register accessible from every register page. */
#define AC200_TWI_REG_ADDR_H	0x00fe
#define AC200_MAX_REG		0xa1f2

static const struct regmap_range_cfg ac200_range_cfg[] = {
	{
		.range_max = AC200_MAX_REG,
		.selector_reg = AC200_TWI_REG_ADDR_H,
		.selector_mask = 0xff,
		.window_len = 256,
	},
};

/*
 * Each AC200 sub-block can reset independently, invalidating its register
 * contents without regmap's knowledge. Cache only the common page selector;
 * this avoids a selector read-modify-write for every access on the same page
 * without ever returning stale functional-register values.
 */
static bool ac200_volatile_reg(struct device *dev, unsigned int reg)
{
	return reg != AC200_TWI_REG_ADDR_H;
}

static const struct regmap_config ac200_regmap_config = {
	.name = "ac200",
	.reg_bits = 8,
	.reg_stride = 2,
	.val_bits = 16,
	.ranges = ac200_range_cfg,
	.num_ranges = ARRAY_SIZE(ac200_range_cfg),
	.max_register = AC200_MAX_REG,
	.volatile_reg = ac200_volatile_reg,
	.cache_type = REGCACHE_MAPLE,
};

static const struct regmap_irq ac200_irqs[] = {
	REGMAP_IRQ_REG(AC200_IRQ_TVE, 0, AC200_SYS_IRQ_TVE),
	REGMAP_IRQ_REG(AC200_IRQ_EPHY, 0, AC200_SYS_IRQ_EPHY),
	REGMAP_IRQ_REG(AC200_IRQ_RTC, 0, AC200_SYS_IRQ_RTC),
};

/*
 * SYS_IRQ_ENABLE is an enable register rather than a mask register, hence
 * unmask_base. SYS_IRQ_STATUS reflects the source levels, so the function
 * which raised an interrupt is responsible for clearing it.
 */
static const struct regmap_irq_chip ac200_irq_chip = {
	.name = "ac200",
	.status_base = AC200_SYS_IRQ_STATUS_REG,
	.unmask_base = AC200_SYS_IRQ_ENABLE_REG,
	.num_regs = 1,
	.irqs = ac200_irqs,
	.num_irqs = ARRAY_SIZE(ac200_irqs),
};

static const struct mfd_cell ac200_cells[] = {
	{
		.name = "ac200-codec",
		.of_compatible = "x-powers,ac200-codec",
	}, {
		.name = "ac200-tve",
		.of_compatible = "x-powers,ac200-tve",
	},
};

static int ac200_init_irq(struct device *dev, struct regmap *regmap, int irq)
{
	struct regmap_irq_chip_data *irq_data;
	unsigned int trigger;
	u16 value = AC200_SYS_IRQ_INTB_ENABLE;
	int ret;

	trigger = irq_get_trigger_type(irq);
	switch (trigger) {
	case IRQ_TYPE_LEVEL_HIGH:
		value |= AC200_SYS_IRQ_INTB_ACTIVE_HIGH;
		break;
	case IRQ_TYPE_NONE:
	case IRQ_TYPE_LEVEL_LOW:
		break;
	default:
		return dev_err_probe(dev, -EINVAL,
				     "INTB is level triggered, not type %u\n",
				     trigger);
	}

	ret = regmap_update_bits(regmap, AC200_SYS_IRQ_ENABLE_REG,
				 AC200_SYS_IRQ_INTB_ENABLE |
				 AC200_SYS_IRQ_INTB_ACTIVE_HIGH, value);
	if (ret)
		return ret;

	ret = devm_regmap_add_irq_chip(dev, regmap, irq, IRQF_ONESHOT, 0,
				       &ac200_irq_chip, &irq_data);
	if (ret)
		return dev_err_probe(dev, ret, "failed to add IRQ chip\n");

	return 0;
}

static int ac200_add_devices(struct device *dev)
{
	struct mfd_cell cells[ARRAY_SIZE(ac200_cells)];
	unsigned int num_cells = 0;
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(ac200_cells); i++) {
		const struct mfd_cell *cell = &ac200_cells[i];
		struct device_node *child;

		child = of_get_compatible_child(dev->of_node,
						cell->of_compatible);
		if (!child)
			continue;
		if (of_device_is_available(child))
			cells[num_cells++] = *cell;
		of_node_put(child);
	}

	if (!num_cells)
		return 0;

	ret = devm_mfd_add_devices(dev, PLATFORM_DEVID_NONE, cells, num_cells,
				   NULL, 0, NULL);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to add function devices\n");

	return 0;
}

static int ac200_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct regmap *regmap;
	struct clk *clk;
	int ret;

	clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk),
				     "failed to enable input clock\n");

	ret = devm_clk_rate_exclusive_get(dev, clk);
	if (ret)
		return dev_err_probe(dev, ret, "failed to lock clock rate\n");

	regmap = devm_regmap_init_i2c(client, &ac200_regmap_config);
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap),
				     "failed to initialize regmap\n");

	/*
	 * No minimum delay is documented. Match the vendor driver's 40 ms delay
	 * before its first AC200 register access after enabling the input clock.
	 */
	msleep(40);

	ret = regmap_set_bits(regmap, AC200_SYS_CONTROL_REG,
			      AC200_SYS_CONTROL_CHIP_RESET_DEASSERT);
	if (ret)
		return ret;

	if (client->irq > 0) {
		ret = ac200_init_irq(dev, regmap, client->irq);
		if (ret)
			return ret;
	}

	return ac200_add_devices(dev);
}

static const struct of_device_id ac200_of_match[] = {
	{ .compatible = "x-powers,ac200" },
	{ }
};
MODULE_DEVICE_TABLE(of, ac200_of_match);

static const struct i2c_device_id ac200_i2c_ids[] = {
	{ .name = "ac200" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ac200_i2c_ids);

static struct i2c_driver ac200_driver = {
	.driver = {
		.name = "ac200",
		.of_match_table = ac200_of_match,
	},
	.probe = ac200_probe,
	.id_table = ac200_i2c_ids,
};
module_i2c_driver(ac200_driver);

MODULE_AUTHOR("Jernej Skrabec <jernej.skrabec@gmail.com>");
MODULE_AUTHOR("James Hilliard <james.hilliard1@gmail.com>");
MODULE_DESCRIPTION("X-Powers AC200 MFD core driver");
MODULE_LICENSE("GPL");
