// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Andes ATCGPIO100 controller driver.
 *
 * Copyright (C) 2026 Andes Technology Corporation
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/dev_printk.h>
#include <linux/err.h>
#include <linux/find.h>
#include <linux/gpio/driver.h>
#include <linux/gpio/generic.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/math.h>
#include <linux/minmax.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/types.h>

#define REG_CONFIG			0x10
#define REG_DATA_IN			0x20
#define REG_DATA_OUT			0x24
#define REG_CHAN_DIR			0x28
#define REG_CHAN_PULL_EN		0x40
#define REG_CHAN_PULL_TYPE		0x44
#define REG_CHAN_INT_EN			0x50
#define REG_INT_MODE0			0x54
#define REG_INT_MODE1			0x58
#define REG_INT_MODE2			0x5c
#define REG_INT_MODE3			0x60
#define REG_CHAN_INT_STATE		0x64
#define REG_DEBOUNCE_EN			0x70
#define REG_DEBOUNCE_CTRL		0x74

#define CONFIG_PULL_OPTION		BIT(31)
#define CONFIG_INT_OPTION		BIT(30)
#define CONFIG_DEBOUNCE_OPTION		BIT(29)
#define CONFIG_DEBOUNCE_CTRL_CLK_SEL	BIT(31)
#define CONFIG_DEBOUNCE_CTRL_MASK	\
	(CONFIG_DEBOUNCE_CTRL_CLK_SEL | GENMASK(7, 0))
#define CONFIG_DEBOUNCE_MAX_CYCLES	256
#define CONFIG_PULL_TYPE_UP		0
#define CONFIG_PULL_TYPE_DOWN		1
#define CONFIG_CHAN_NUM_MSK		GENMASK(5, 0)
#define CONFIG_CHAN_NUM_GET(x)		FIELD_GET(CONFIG_CHAN_NUM_MSK, x)

#define INT_MODE_TYPE_REG_OFFSET(gpio)	(REG_INT_MODE0 + (((gpio) >> 3) << 2))
#define INT_MODE_TYPE_SHIFT(gpio)	(((gpio) % 8) << 2)
#define INT_MODE_TYPE_MASK(gpio)	\
	(GENMASK(2, 0) << INT_MODE_TYPE_SHIFT(gpio))
#define INT_MODE_LEVEL_HIGH		2
#define INT_MODE_LEVEL_LOW		3
#define INT_MODE_EDGE_NEGATIVE		5
#define INT_MODE_EDGE_POSITIVE		6
#define INT_MODE_EDGE_DUAL		7
#define INT_MODE_REG_NUM		4

#define ATCGPIO_CHAN_NUM_MAX		32

/**
 * struct atcgpio_context - Registers saved across system suspend
 * @data_out:     Channel data-out register
 * @dir:          Channel direction register
 * @pull_en:      Channel pull enable register
 * @pull_type:    Channel pull type register
 * @int_mode:     Channel interrupt mode registers
 * @debounce_en:  Channel de-bounce enable register
 * @debounce_ctrl: De-bounce control register
 */
struct atcgpio_context {
	u32 data_out;
	u32 dir;
	u32 pull_en;
	u32 pull_type;
	u32 int_mode[INT_MODE_REG_NUM];
	u32 debounce_en;
	u32 debounce_ctrl;
};

/**
 * struct atcgpio_priv - Private data structure for ATCGPIO100 controller
 * @chip:            Generic GPIO chip, also provides the register lock
 * @pdev:            Pointer to the platform device
 * @base:            Base address of the controller registers
 * @ctx:             Registers saved across system suspend
 * @int_en:          Shadow of the channel interrupt enable register
 * @extclk_rate:     Frequency of external clock for debounce calculations
 * @pclk_rate:       Frequency of APB bus clock for debounce calculations
 * @irq_parent:      Parent IRQ number connected to the interrupt controller
 * @chan_num:        Number of GPIO channels supported by the hardware
 * @pull_option:     Indicates hardware support for pull-up/down resistors
 * @int_option:      Indicates hardware support for interrupts
 * @debounce_option: Indicates hardware support for input debouncing
 */
struct atcgpio_priv {
	struct gpio_generic_chip	chip;
	struct platform_device	*pdev;
	void __iomem		*base;
	struct atcgpio_context	ctx;
	u32			int_en;
	unsigned int		extclk_rate;
	unsigned int		pclk_rate;
	int			irq_parent;
	unsigned char		chan_num;
	bool			pull_option;
	bool			int_option;
	bool			debounce_option;
};

static u32 atcgpio_read(struct atcgpio_priv *priv, unsigned int reg)
{
	return readl(priv->base + reg);
}

static void atcgpio_write(struct atcgpio_priv *priv, unsigned int reg, u32 val)
{
	writel(val, priv->base + reg);
}

static void atcgpio_update_bits(struct atcgpio_priv *priv, unsigned int reg,
				u32 mask, u32 val)
{
	u32 tmp;

	tmp = atcgpio_read(priv, reg);
	tmp = (tmp & ~mask) | (val & mask);
	atcgpio_write(priv, reg, tmp);
}

/*
 * Convert a de-bounce period to clock cycles, rounded up so that the filter
 * is at least as long as requested. Zero if the clock is not available.
 */
static u64 atcgpio_calc_cycles(u32 clk_rate, u32 debounce_us)
{
	if (!clk_rate)
		return 0;

	return DIV_ROUND_UP_ULL((u64)debounce_us * clk_rate, 1000000);
}

static int atcgpio_set_debounce(struct atcgpio_priv *priv,
				unsigned int gpio,
				unsigned int debounce_us)
{
	unsigned long flags;
	u64 ext_cycles, pclk_cycles, cycles;
	u32 ctrl_val;
	u32 active_users, current_ctrl;

	if (!debounce_us) {
		gpio_generic_chip_lock_irqsave(&priv->chip, flags);
		atcgpio_update_bits(priv, REG_DEBOUNCE_EN, BIT(gpio), 0);
		gpio_generic_chip_unlock_irqrestore(&priv->chip, flags);
		return 0;
	}

	pclk_cycles = atcgpio_calc_cycles(priv->pclk_rate, debounce_us);
	ext_cycles = atcgpio_calc_cycles(priv->extclk_rate, debounce_us);

	/*
	 * Prefer pclk, the faster clock, for a shorter de-bounce latency, as
	 * long as the period fits in its pre-scaler; else use extclk. Reject
	 * a period that neither clock can reach.
	 */
	if (pclk_cycles && pclk_cycles <= CONFIG_DEBOUNCE_MAX_CYCLES) {
		cycles = pclk_cycles;
		ctrl_val = CONFIG_DEBOUNCE_CTRL_CLK_SEL;
	} else if (ext_cycles && ext_cycles <= CONFIG_DEBOUNCE_MAX_CYCLES) {
		cycles = ext_cycles;
		ctrl_val = 0;
	} else {
		gpio_generic_chip_lock_irqsave(&priv->chip, flags);
		atcgpio_update_bits(priv, REG_DEBOUNCE_EN, BIT(gpio), 0);
		gpio_generic_chip_unlock_irqrestore(&priv->chip, flags);
		return -ENOTSUPP;
	}

	/* Filtering pulses shorter than N clocks takes a pre-scale of N - 1 */
	ctrl_val |= (u32)(cycles - 1) & GENMASK(7, 0);

	gpio_generic_chip_lock_irqsave(&priv->chip, flags);

	active_users = atcgpio_read(priv, REG_DEBOUNCE_EN);
	current_ctrl = atcgpio_read(priv, REG_DEBOUNCE_CTRL);

	active_users &= ~BIT(gpio);
	current_ctrl &= CONFIG_DEBOUNCE_CTRL_MASK;

	if (active_users && current_ctrl != ctrl_val) {
		atcgpio_update_bits(priv, REG_DEBOUNCE_EN, BIT(gpio), 0);
		gpio_generic_chip_unlock_irqrestore(&priv->chip, flags);
		return -ENOTSUPP;
	}

	atcgpio_update_bits(priv, REG_DEBOUNCE_CTRL,
			    CONFIG_DEBOUNCE_CTRL_MASK, ctrl_val);
	atcgpio_update_bits(priv, REG_DEBOUNCE_EN, BIT(gpio), BIT(gpio));

	gpio_generic_chip_unlock_irqrestore(&priv->chip, flags);

	return 0;
}

static int atcgpio_set_pull_type(struct atcgpio_priv *priv,
				 unsigned int gpio,
				 bool pull_type,
				 bool pull_en)
{
	unsigned long flags;

	gpio_generic_chip_lock_irqsave(&priv->chip, flags);
	if (pull_en) {
		atcgpio_update_bits(priv, REG_CHAN_PULL_TYPE, BIT(gpio),
				    pull_type ? BIT(gpio) : 0);
		atcgpio_update_bits(priv, REG_CHAN_PULL_EN, BIT(gpio),
				    BIT(gpio));
	} else {
		atcgpio_update_bits(priv, REG_CHAN_PULL_EN, BIT(gpio), 0);
	}
	gpio_generic_chip_unlock_irqrestore(&priv->chip, flags);

	return 0;
}

static int atcgpio_set_config(struct gpio_chip *gc,
			      unsigned int offset,
			      unsigned long config)
{
	struct atcgpio_priv *priv = gpiochip_get_data(gc);
	unsigned int debounce_us;
	int ret = -ENOTSUPP;

	switch (pinconf_to_config_param(config)) {
	case PIN_CONFIG_BIAS_DISABLE:
		if (priv->pull_option)
			ret = atcgpio_set_pull_type(priv, offset, 0, 0);
		break;
	case PIN_CONFIG_BIAS_PULL_UP:
		if (priv->pull_option)
			ret = atcgpio_set_pull_type(priv,
						    offset,
						    CONFIG_PULL_TYPE_UP, 1);
		break;
	case PIN_CONFIG_BIAS_PULL_DOWN:
		if (priv->pull_option)
			ret = atcgpio_set_pull_type(priv,
						    offset,
						    CONFIG_PULL_TYPE_DOWN, 1);
		break;
	case PIN_CONFIG_INPUT_DEBOUNCE:
		if (priv->debounce_option) {
			debounce_us = pinconf_to_config_argument(config);
			ret = atcgpio_set_debounce(priv, offset, debounce_us);
		}
		break;
	default:
		break;
	}

	return ret;
}

static void atcgpio_free(struct gpio_chip *gc, unsigned int offset)
{
	struct atcgpio_priv *priv = gpiochip_get_data(gc);
	unsigned long flags;

	if (!priv->debounce_option)
		return;

	gpio_generic_chip_lock_irqsave(&priv->chip, flags);
	atcgpio_update_bits(priv, REG_DEBOUNCE_EN, BIT(offset), 0);
	gpio_generic_chip_unlock_irqrestore(&priv->chip, flags);
}

static void atcgpio_ack_irq(struct irq_data *data)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(data);
	struct atcgpio_priv *priv = gpiochip_get_data(gc);
	unsigned int gpio = irqd_to_hwirq(data);

	atcgpio_write(priv, REG_CHAN_INT_STATE, BIT(gpio));
}

static void atcgpio_enable_irq(struct atcgpio_priv *priv,
			       unsigned int gpio,
			       bool enable)
{
	unsigned long flags;

	gpio_generic_chip_lock_irqsave(&priv->chip, flags);
	if (enable)
		priv->int_en |= BIT(gpio);
	else
		priv->int_en &= ~BIT(gpio);
	atcgpio_write(priv, REG_CHAN_INT_EN, priv->int_en);
	gpio_generic_chip_unlock_irqrestore(&priv->chip, flags);
}

static void atcgpio_set_irq_unmask(struct irq_data *data)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(data);
	struct atcgpio_priv *priv = gpiochip_get_data(gc);
	unsigned int gpio = irqd_to_hwirq(data);

	gpiochip_enable_irq(gc, gpio);
	atcgpio_enable_irq(priv, gpio, 1);
}

static void atcgpio_set_irq_mask(struct irq_data *data)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(data);
	struct atcgpio_priv *priv = gpiochip_get_data(gc);
	unsigned int gpio = irqd_to_hwirq(data);

	atcgpio_enable_irq(priv, gpio, 0);
	gpiochip_disable_irq(gc, gpio);
}

static int atcgpio_set_irq_type(struct irq_data *data, unsigned int type)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(data);
	struct atcgpio_priv *priv = gpiochip_get_data(gc);
	irq_flow_handler_t handler;
	unsigned long flags;
	unsigned int gpio = irqd_to_hwirq(data);
	unsigned char type_offset;
	unsigned char irq_type;

	type_offset = INT_MODE_TYPE_SHIFT(gpio);

	switch (type) {
	case IRQ_TYPE_EDGE_RISING:
		irq_type = INT_MODE_EDGE_POSITIVE;
		handler = handle_edge_irq;
		break;
	case IRQ_TYPE_EDGE_FALLING:
		irq_type = INT_MODE_EDGE_NEGATIVE;
		handler = handle_edge_irq;
		break;
	case IRQ_TYPE_EDGE_BOTH:
		irq_type = INT_MODE_EDGE_DUAL;
		handler = handle_edge_irq;
		break;
	case IRQ_TYPE_LEVEL_HIGH:
		irq_type = INT_MODE_LEVEL_HIGH;
		handler = handle_level_irq;
		break;
	case IRQ_TYPE_LEVEL_LOW:
		irq_type = INT_MODE_LEVEL_LOW;
		handler = handle_level_irq;
		break;
	default:
		return -EINVAL;
	}

	gpio_generic_chip_lock_irqsave(&priv->chip, flags);
	atcgpio_update_bits(priv, INT_MODE_TYPE_REG_OFFSET(gpio),
			    INT_MODE_TYPE_MASK(gpio), irq_type << type_offset);
	atcgpio_write(priv, REG_CHAN_INT_STATE, BIT(gpio));
	gpio_generic_chip_unlock_irqrestore(&priv->chip, flags);
	irq_set_handler_locked(data, handler);

	return 0;
}

static void atcgpio_irq_handler(struct irq_desc *desc)
{
	struct gpio_chip *gc = irq_desc_get_handler_data(desc);
	struct atcgpio_priv *priv = gpiochip_get_data(gc);
	unsigned long status;
	int offset;

	chained_irq_enter(irq_desc_get_chip(desc), desc);

	status = atcgpio_read(priv, REG_CHAN_INT_STATE);
	if (unlikely(!status)) {
		dev_warn_ratelimited(gc->parent, "Spurious IRQ: status is 0\n");
		goto irq_out;
	}

	for_each_set_bit(offset, &status, gc->ngpio)
		generic_handle_domain_irq(gc->irq.domain, offset);

irq_out:
	chained_irq_exit(irq_desc_get_chip(desc), desc);
}

static int atcgpio_irq_set_wake(struct irq_data *data, unsigned int on)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(data);
	struct atcgpio_priv *priv = gpiochip_get_data(gc);
	int ret;

	if (on)
		ret = enable_irq_wake(priv->irq_parent);
	else
		ret = disable_irq_wake(priv->irq_parent);

	return ret;
}

static const struct irq_chip atcgpio_irqchip = {
	.name = "gpio-atcgpio100",
	.irq_ack = atcgpio_ack_irq,
	.irq_mask = atcgpio_set_irq_mask,
	.irq_unmask = atcgpio_set_irq_unmask,
	.irq_set_type = atcgpio_set_irq_type,
	.irq_set_wake = atcgpio_irq_set_wake,
	.flags = IRQCHIP_IMMUTABLE | IRQCHIP_SET_TYPE_MASKED |
		 IRQCHIP_MASK_ON_SUSPEND,
	GPIOCHIP_IRQ_RESOURCE_HELPERS,
};

static int atcgpio_init_resources(struct atcgpio_priv *priv)
{
	struct gpio_generic_chip_config config;
	struct clk *clk_debounce;
	int ret;

	priv->base = devm_platform_get_and_ioremap_resource(priv->pdev, 0, NULL);
	if (IS_ERR(priv->base))
		return dev_err_probe(&priv->pdev->dev, PTR_ERR(priv->base),
				     "Failed to get ioremap resource\n");

	clk_debounce = devm_clk_get_enabled(&priv->pdev->dev, "pclk");
	if (IS_ERR(clk_debounce))
		return dev_err_probe(&priv->pdev->dev, PTR_ERR(clk_debounce),
				     "Failed to get pclk\n");
	priv->pclk_rate = clk_get_rate(clk_debounce);

	clk_debounce = devm_clk_get_optional_enabled(&priv->pdev->dev,
						     "extclk");
	if (clk_debounce == ERR_PTR(-EPROBE_DEFER))
		return dev_err_probe(&priv->pdev->dev, -EPROBE_DEFER,
				     "Failed to get extclk\n");
	if (IS_ERR(clk_debounce)) {
		dev_warn(&priv->pdev->dev, "Failed to get extclk: %pe\n",
			 clk_debounce);
		priv->extclk_rate = 0;
	} else {
		priv->extclk_rate = clk_get_rate(clk_debounce);
	}

	config = (struct gpio_generic_chip_config) {
		.dev = &priv->pdev->dev,
		.sz = 4,
		.dat = priv->base + REG_DATA_IN,
		.set = priv->base + REG_DATA_OUT,
		.dirout = priv->base + REG_CHAN_DIR,
		.flags = GPIO_GENERIC_READ_OUTPUT_REG_SET,
	};

	ret = gpio_generic_chip_init(&priv->chip, &config);
	if (ret)
		return dev_err_probe(&priv->pdev->dev, ret,
				     "Failed to init generic GPIO chip\n");

	return 0;
}

static void atcgpio_init_controller(struct atcgpio_priv *priv)
{
	u32 cfg;

	cfg = atcgpio_read(priv, REG_CONFIG);
	priv->pull_option = !!(cfg & CONFIG_PULL_OPTION);
	priv->int_option = !!(cfg & CONFIG_INT_OPTION);
	priv->debounce_option = !!(cfg & CONFIG_DEBOUNCE_OPTION);
	priv->chan_num = CONFIG_CHAN_NUM_GET(cfg);
	if (priv->chan_num > ATCGPIO_CHAN_NUM_MAX || !priv->chan_num) {
		dev_warn(&priv->pdev->dev,
			 "Invalid hardware GPIO number %d, forcing to %d\n",
			 priv->chan_num,
			 ATCGPIO_CHAN_NUM_MAX);
		priv->chan_num = ATCGPIO_CHAN_NUM_MAX;
	}

	if (!priv->extclk_rate && !priv->pclk_rate) {
		dev_warn(&priv->pdev->dev,
			 "Disabling debounce: missing reference clock\n");
		priv->debounce_option = 0;
	}

	if (priv->debounce_option)
		atcgpio_write(priv, REG_DEBOUNCE_EN, 0);

	if (priv->int_option) {
		atcgpio_write(priv, REG_CHAN_INT_EN, 0);
		atcgpio_write(priv, REG_INT_MODE0, 0);
		atcgpio_write(priv, REG_INT_MODE1, 0);
		atcgpio_write(priv, REG_INT_MODE2, 0);
		atcgpio_write(priv, REG_INT_MODE3, 0);
		atcgpio_write(priv, REG_CHAN_INT_STATE, GENMASK(31, 0));
	}
}

static int atcgpio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct gpio_irq_chip *gc_irq;
	struct atcgpio_priv *priv;
	struct gpio_chip *gc;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->pdev = pdev;
	ret = atcgpio_init_resources(priv);
	if (ret)
		return ret;

	atcgpio_init_controller(priv);

	gc = &priv->chip.gc;
	gc->owner = THIS_MODULE;
	gc->set_config = atcgpio_set_config;
	gc->free = atcgpio_free;
	gc->ngpio = min_t(unsigned int, gc->ngpio, priv->chan_num);

	if (priv->int_option) {
		priv->irq_parent = platform_get_irq_optional(pdev, 0);
		if (priv->irq_parent == -EPROBE_DEFER)
			return dev_err_probe(dev, priv->irq_parent,
					     "Failed to get IRQ\n");
		if (priv->irq_parent < 0) {
			dev_info(dev, "No IRQ, interrupt support disabled\n");
			priv->int_option = false;
		}
	}

	if (priv->int_option) {
		gc_irq = &gc->irq;
		gpio_irq_chip_set_chip(gc_irq, &atcgpio_irqchip);
		gc_irq->parent_handler = atcgpio_irq_handler;
		gc_irq->num_parents = 1;
		gc_irq->parents = devm_kcalloc(dev, gc_irq->num_parents,
					       sizeof(*gc_irq->parents),
					       GFP_KERNEL);
		if (!gc_irq->parents)
			return -ENOMEM;
		gc_irq->parents[0] = priv->irq_parent;
		gc_irq->default_type = IRQ_TYPE_NONE;
		gc_irq->handler = handle_bad_irq;
	}

	platform_set_drvdata(pdev, priv);
	return devm_gpiochip_add_data(dev, gc, priv);
}

static int atcgpio_suspend(struct device *dev)
{
	struct atcgpio_priv *priv = dev_get_drvdata(dev);
	struct atcgpio_context *ctx = &priv->ctx;
	unsigned long flags;

	gpio_generic_chip_lock_irqsave(&priv->chip, flags);
	ctx->data_out = atcgpio_read(priv, REG_DATA_OUT);
	ctx->dir = atcgpio_read(priv, REG_CHAN_DIR);
	if (priv->pull_option) {
		ctx->pull_en = atcgpio_read(priv, REG_CHAN_PULL_EN);
		ctx->pull_type = atcgpio_read(priv, REG_CHAN_PULL_TYPE);
	}
	if (priv->int_option) {
		ctx->int_mode[0] = atcgpio_read(priv, REG_INT_MODE0);
		ctx->int_mode[1] = atcgpio_read(priv, REG_INT_MODE1);
		ctx->int_mode[2] = atcgpio_read(priv, REG_INT_MODE2);
		ctx->int_mode[3] = atcgpio_read(priv, REG_INT_MODE3);
	}
	if (priv->debounce_option) {
		ctx->debounce_en = atcgpio_read(priv, REG_DEBOUNCE_EN);
		ctx->debounce_ctrl = atcgpio_read(priv, REG_DEBOUNCE_CTRL);
	}
	gpio_generic_chip_unlock_irqrestore(&priv->chip, flags);

	return 0;
}

static int atcgpio_resume(struct device *dev)
{
	struct atcgpio_priv *priv = dev_get_drvdata(dev);
	struct atcgpio_context *ctx = &priv->ctx;
	unsigned long flags;

	gpio_generic_chip_lock_irqsave(&priv->chip, flags);
	atcgpio_write(priv, REG_DATA_OUT, ctx->data_out);
	if (priv->pull_option) {
		atcgpio_write(priv, REG_CHAN_PULL_TYPE, ctx->pull_type);
		atcgpio_write(priv, REG_CHAN_PULL_EN, ctx->pull_en);
	}
	atcgpio_write(priv, REG_CHAN_DIR, ctx->dir);
	if (priv->debounce_option) {
		atcgpio_write(priv, REG_DEBOUNCE_CTRL, ctx->debounce_ctrl);
		atcgpio_write(priv, REG_DEBOUNCE_EN, ctx->debounce_en);
	}
	if (priv->int_option) {
		atcgpio_write(priv, REG_INT_MODE0, ctx->int_mode[0]);
		atcgpio_write(priv, REG_INT_MODE1, ctx->int_mode[1]);
		atcgpio_write(priv, REG_INT_MODE2, ctx->int_mode[2]);
		atcgpio_write(priv, REG_INT_MODE3, ctx->int_mode[3]);
		atcgpio_write(priv, REG_CHAN_INT_EN, priv->int_en);
	}
	gpio_generic_chip_unlock_irqrestore(&priv->chip, flags);

	return 0;
}

static DEFINE_NOIRQ_DEV_PM_OPS(atcgpio_pm_ops,
			       atcgpio_suspend, atcgpio_resume);

static const struct of_device_id atcgpio100_gpio_match[] = {
	{ .compatible = "andestech,ae350-gpio" },
	{ }
};
MODULE_DEVICE_TABLE(of, atcgpio100_gpio_match);

static struct platform_driver atcgpio100_gpio_driver = {
	.probe = atcgpio_probe,
	.driver = {
		.name = "atcgpio100",
		.of_match_table = atcgpio100_gpio_match,
		.pm = pm_sleep_ptr(&atcgpio_pm_ops),
	}
};

module_platform_driver(atcgpio100_gpio_driver);

MODULE_DESCRIPTION("Andes ATCGPIO100 GPIO controller driver");
MODULE_AUTHOR("CL Wang <cl634@andestech.com>");
MODULE_LICENSE("GPL");
