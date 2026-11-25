// SPDX-License-Identifier: GPL-2.0-only

/*
 * Copyright (C) 2026 by RISCstar Solutions Corporation.  All rights reserved.
 */

#include <linux/bits.h>
#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/reset-controller.h>
#include <linux/slab.h>

#include <dt-bindings/clock/toshiba,tc9564.h>

/*
 * Define the offsets for the registers that implement clock enable/disable
 * bits for gated clocks, and those that implement reset assertion bits for
 * resets.  These offsets are relative to the address space defined in
 * devicetree.
 */
#define CLOCK0_OFFSET	0x0
#define CLOCK1_OFFSET	0x8

#define RESET0_OFFSET	0x4
#define RESET1_OFFSET	0xc

/*
 * A PLL driven by a 25 MHz input oscillator provides a clock source to
 * the chip.  A set of fixed rate clocks is derived from the PLL, and a
 * set of gate clocks are children of the fixed rate clocks.
 *
 * The clk_hw[] array within a clk_hw_onecell_data structure tracks all
 * of them.  The following definitions describe how these are laid out
 * based on their indexes into the clk_hw[] array.  Gate clocks are first,
 * so their "raw" public IDs can be used as indexes into the array.
 */
#define GATE_BASE	0
#define FIXED_BASE	(GATE_BASE + GATE_COUNT)
#define CLOCK_PLL	(FIXED_BASE + FIXED_COUNT)

/* The total number of clocks in the clk_hw[] array */
#define CLOCK_COUNT	(GATE_COUNT + FIXED_COUNT + 1)

struct tc9564_clock {
	void __iomem *ioaddr;	/* Mapped clock/reset registers */
	spinlock_t lock;	/* Protects reset register updates */
	struct clk_hw_onecell_data *clk_data;
	struct reset_controller_dev rcdev;
};

/* Indexes into the fixed_init[] array */
enum fixed_clock {
	CLOCK_62_5M,
	CLOCK_125M,
	CLOCK_250M,
	CLOCK_312_5M,
};

struct fixed_init {
	const char *name;
	unsigned int div;	/* Rate is PLL rate divided by this */
};

struct gate_init {
	const char *name;
	enum fixed_clock parent;
	u8 offset;		/* Control register offset (0x0 or 0x8) */
	u8 bit;			/* Bit in the control register (0-31) */
	bool critical;
};

struct reset_init {
	u8 offset;		/* Control register offset (0x4 or 0xc) */
	u8 bit;			/* Bit in the control register (0-31) */
};

/* Add FIXED_BASE to index values to get their index into the clk_hw[] array */
static const struct fixed_init fixed_init[] = {
	[CLOCK_312_5M] = {
		.name = "312.5MHz",
		.div = 4,
	},
	[CLOCK_250M] = {
		.name = "250MHz",
		.div = 5,
	},
	[CLOCK_125M] = {
		.name = "125MHz",
		.div = 10,
	},
	[CLOCK_62_5M] = {
		.name = "62.5MHz",
		.div = 20,
	},
};
#define FIXED_COUNT	ARRAY_SIZE(fixed_init)

/* Add GATE_BASE to index values to get their index into the clk_hw[] array */
static const struct gate_init gate_init[] = {
	[CLOCK_MAC0_TX] = {
		.name = "MAC0-TX",
		.parent = CLOCK_312_5M,
		.offset = CLOCK0_OFFSET,
		.bit = 7,
	},
	[CLOCK_PCIE] = {
		.name = "PCIe",
		.parent = CLOCK_250M,
		.offset = CLOCK0_OFFSET,
		.bit = 9,
		.critical = true,
	},
	[CLOCK_I2C] = {
		.name = "i2c",
		.parent = CLOCK_62_5M,
		.offset = CLOCK0_OFFSET,
		.bit = 12,
		.critical = true,
	},
	[CLOCK_MAC0_RX] = {
		.name = "MAC0-RX",
		.parent = CLOCK_312_5M,
		.offset = CLOCK0_OFFSET,
		.bit = 14,
	},
	[CLOCK_MSIGEN] = {
		.name = "msigen",
		.parent = CLOCK_125M,
		.offset = CLOCK0_OFFSET,
		.bit = 18,
	},
	[CLOCK_MAC0_125M] = {
		.name = "MAC0-125MHz",
		.parent = CLOCK_125M,
		.offset = CLOCK0_OFFSET,
		.bit = 29,
	},
	[CLOCK_MAC0_ALL] = {
		.name = "MAC0-ALL",
		.parent = CLOCK_250M,
		.offset = CLOCK0_OFFSET,
		.bit = 31,
	},
	[CLOCK_MAC1_TX] = {
		.name = "MAC1-TX",
		.parent = CLOCK_312_5M,
		.offset = CLOCK1_OFFSET,
		.bit = 7,
	},
	[CLOCK_MAC1_RX] = {
		.name = "MAC1-RX",
		.parent = CLOCK_312_5M,
		.offset = CLOCK1_OFFSET,
		.bit = 14,
	},
	[CLOCK_MAC1_125M] = {
		.name = "MAC1-125MHz",
		.parent = CLOCK_125M,
		.offset = CLOCK1_OFFSET,
		.bit = 29,
	},
	[CLOCK_MAC1_ALL] = {
		.name = "MAC1-ALL",
		.parent = CLOCK_250M,
		.offset = CLOCK1_OFFSET,
		.bit = 31,
	},
};
#define GATE_COUNT	ARRAY_SIZE(gate_init)

static const struct reset_init reset_init[] = {
	[RESET_MAC0_MAC] = {
		.offset = RESET0_OFFSET,
		.bit = 7,
	},
	[RESET_PCIE] = {
		.offset = RESET0_OFFSET,
		.bit = 9,
	},
	[RESET_I2C] = {
		.offset = RESET0_OFFSET,
		.bit = 12,
	},
	[RESET_MSIGEN] = {
		.offset = RESET0_OFFSET,
		.bit = 18,
	},
	[RESET_MAC0_PMA] = {
		.offset = RESET0_OFFSET,
		.bit = 30,
	},
	[RESET_MAC0_XPCS] = {
		.offset = RESET0_OFFSET,
		.bit = 31,
	},
	[RESET_MAC1_MAC] = {
		.offset = RESET1_OFFSET,
		.bit = 7,
	},
	[RESET_MAC1_PMA] = {
		.offset = RESET1_OFFSET,
		.bit = 30,
	},
	[RESET_MAC1_XPCS] = {
		.offset = RESET1_OFFSET,
		.bit = 31,
	},
};
#define RESET_COUNT	ARRAY_SIZE(reset_init)

/* Caller must free the returned pointer if non-null */
static const char *clock_name(struct device *dev, const char *name)
{
	return kasprintf(GFP_KERNEL, "%s-%s", dev_name(dev), name);
}

/* Initialize the PLL (parent is the input oscillator); called from probe */
static int pll_init(struct device *dev, struct tc9564_clock *clock)
{
	struct clk_hw *hw;
	const char *name;

	/* Initialize the PLL clock first (input oscillator is parent) */
	name = clock_name(dev, "pll");
	if (!name)
		return -ENOMEM;

	hw = devm_clk_hw_register_fixed_factor_index(dev, name, 0, 0, 50, 1);
	kfree(name);
	if (IS_ERR(hw))
		return dev_err_probe(dev, PTR_ERR(hw),
				     "failed to register pll\n");

	clock->clk_data->hws[CLOCK_PLL] = hw;

	return 0;
}

/* Initialize the fixed rate clocks (PLL is parent); called from probe */
static int fixed_clock_init(struct device *dev, struct tc9564_clock *clock)
{
	struct clk_hw_onecell_data *clk_data = clock->clk_data;
	struct clk_hw *pll_hw = clk_data->hws[CLOCK_PLL];
	const char *name;

	for (u32 i = 0; i < FIXED_COUNT; i++) {
		const struct fixed_init *init = &fixed_init[i];
		struct clk_hw *hw;

		name = clock_name(dev, init->name);
		if (!name)
			return -ENOMEM;

		hw = devm_clk_hw_register_fixed_factor_parent_hw(dev, name,
								 pll_hw, 0,
								 1, init->div);
		kfree(name);
		if (IS_ERR(hw))
			return dev_err_probe(dev, PTR_ERR(hw),
					     "failed to register %s\n",
					     init->name);

		clk_data->hws[FIXED_BASE + i] = hw;
	}

	return 0;
}

/* Initialize the gate clocks; called from probe */
static int gate_clock_init(struct device *dev, struct tc9564_clock *clock)
{
	struct clk_hw_onecell_data *clk_data = clock->clk_data;
	struct clk_hw *parent_hw;
	const char *name;

	for (u32 i = 0; i < GATE_COUNT; i++) {
		const struct gate_init *init = &gate_init[i];
		unsigned long flags;
		struct clk_hw *hw;
		void __iomem *reg;
		u32 parent_index;

		name = clock_name(dev, init->name);
		if (!name)
			return -ENOMEM;

		/* Each gate clock's parent is a fixed rate clock */
		parent_index = FIXED_BASE + init->parent;
		parent_hw = clk_data->hws[parent_index];
		flags = init->critical ? CLK_IS_CRITICAL : 0;
		reg = clock->ioaddr + init->offset;

		hw = devm_clk_hw_register_gate_parent_hw(dev, name, parent_hw,
							 flags, reg, init->bit,
							 0, NULL);
		kfree(name);
		if (IS_ERR(hw))
			return dev_err_probe(dev, PTR_ERR(hw),
					     "failed to register %s\n",
					     init->name);

		clk_data->hws[GATE_BASE + i] = hw;
	}

	return 0;
}

static int reset_update(struct reset_controller_dev *rcdev,
			unsigned long id, bool assert)
{
	const struct reset_init *reset;
	struct tc9564_clock *clock;
	unsigned long flags;
	void __iomem *reg;
	u32 mask;
	u32 val;

	/* Warn on any attempt to change state of a critical reset */
	if (WARN_ON(id == RESET_I2C) || WARN_ON(id == RESET_PCIE))
		return 0;

	clock = dev_get_drvdata(rcdev->dev);
	reset = &reset_init[id];
	reg = clock->ioaddr + reset->offset;
	mask = BIT(reset->bit);

	spin_lock_irqsave(&clock->lock, flags);

	val = readl(reg);
	if (assert)
		val |= mask;
	else
		val &= ~mask;
	writel(val, reg);

	spin_unlock_irqrestore(&clock->lock, flags);

	return 0;
}

static int reset_assert(struct reset_controller_dev *rcdev, unsigned long id)
{
	return reset_update(rcdev, id, true);
}

static int reset_deassert(struct reset_controller_dev *rcdev, unsigned long id)
{
	return reset_update(rcdev, id, false);
}

static int reset_status(struct reset_controller_dev *rcdev, unsigned long id)
{
	struct tc9564_clock *clock = dev_get_drvdata(rcdev->dev);
	const struct reset_init *reset = &reset_init[id];
	void __iomem *reg;

	reg = clock->ioaddr + reset->offset;

	return readl(reg) & BIT(reset->bit) ? 1 : 0;
}

static const struct reset_control_ops reset_control_ops = {
	.assert = reset_assert,
	.deassert = reset_deassert,
	.status = reset_status,
};

static int tc9564_clk_probe(struct platform_device *pdev)
{
	struct clk_hw_onecell_data *clk_data;
	struct reset_controller_dev *rcdev;
	struct tc9564_clock *clock;
	struct device *dev = &pdev->dev;
	struct device_node *np;
	int ret;

	np = dev_of_node(dev);
	if (!np)
		return dev_err_probe(dev, -EINVAL, "no devicetree node\n");

	clock = devm_kzalloc_obj(dev, *clock);
	if (!clock)
		return -ENOMEM;

	clock->ioaddr = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(clock->ioaddr))
		return dev_err_probe(dev, PTR_ERR(clock->ioaddr),
				     "failed to map registers\n");

	clk_data = devm_kzalloc_flex(dev, *clock->clk_data, hws,
				     CLOCK_COUNT);
	if (!clk_data)
		return -ENOMEM;
	clk_data->num = CLOCK_COUNT;

	clock->clk_data = clk_data;

	/* We need the driver data pointer to initialize clocks and resets */
	dev_set_drvdata(dev, clock);

	/* Set up the clocks and register the provider */
	ret = pll_init(dev, clock);
	if (ret)
		return ret;

	ret = fixed_clock_init(dev, clock);
	if (ret)
		return ret;

	ret = gate_clock_init(dev, clock);
	if (ret)
		return ret;

	ret = devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, clk_data);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to add clock provider\n");

	/* Set up and register the reset controller */
	rcdev = &clock->rcdev;
	rcdev->ops = &reset_control_ops;
	rcdev->owner = THIS_MODULE;
	rcdev->dev = dev;
	rcdev->of_node = np;
	rcdev->nr_resets = RESET_COUNT;

	spin_lock_init(&clock->lock);

	ret = devm_reset_controller_register(dev, rcdev);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register reset controller\n");

	return 0;
}

static const struct of_device_id tc9564_clock_match[] = {
	{ .compatible = "toshiba,tc9564-clock", },
	{ }
};
MODULE_DEVICE_TABLE(of, tc9564_clock_match);

static struct platform_driver tc9564_clk_driver = {
	.probe = tc9564_clk_probe,
	.driver = {
		.name = KBUILD_MODNAME,
		.of_match_table = tc9564_clock_match,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
};
module_platform_driver(tc9564_clk_driver);

MODULE_DESCRIPTION("Toshiba TC9564 Clock Driver");
MODULE_LICENSE("GPL");
