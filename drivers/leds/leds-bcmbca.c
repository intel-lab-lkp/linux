// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Driver for BCMBCA memory-mapped LEDs
 *
 * Copyright 2024 Linus Walleij <linus.walleij@linaro.org>
 */
#include <linux/bits.h>
#include <linux/cleanup.h>
#include <linux/io.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>

#define BCMBCA_CTRL			0x00
#define BCMBCA_CTRL_SERIAL_POL_HIGH	BIT(1)
#define BCMBCA_CTRL_ENABLE		BIT(3) /* Uncertain about this bit */
#define BCMBCA_CTRL_SERIAL_MSB_FIRST	BIT(4)

#define BCMBCA_MASK			0x04
#define BCMBCA_HW_EN			0x08
/*
 * In the serial shift selector, set bits to 1 from BIT(0) and upward all
 * set to one until the last LED including any unused slots in the shift
 * register.
 */
#define BCMBCA_SERIAL_SHIFT_SEL		0x0c
#define BCMBCA_FLASH_RATE		0x10 /* 4 bits per LED so -> 1c */
#define BCMBCA_BRIGHTNESS		0x20 /* 4 bits per LED so -> 2c */
#define BCMBCA_POWER_LED_CFG		0x30
#define BCMBCA_POWER_LUT		0x34 /* -> b0 */
#define BCMBCA_HW_POLARITY		0xb4
#define BCMBCA_SW_DATA			0xb8 /* 1 bit on/off for each LED */
#define BCMBCA_SW_POLARITY		0xbc
#define BCMBCA_PARALLEL_POLARITY	0xc0
#define BCMBCA_SERIAL_POLARITY		0xc4

#define BCMBCA_LED_MAX_COUNT		32
#define BCMBCA_LED_MAX_BRIGHTNESS	8

enum bcmbca_led_type {
	BCMBCA_LED_SERIAL,
	BCMBCA_LED_PARALLEL,
};

/**
 * struct bcmbca_led - state container for bcmbca based LEDs
 * @cdev: LED class device for this LED
 * @base: memory base address
 * @lock: memory lock
 * @idx: LED index number
 * @active_low: LED is active low
 * @num_serial_shifters: number of serial shift registers
 * @led_type: whether this is a serial or parallel LED
 */
struct bcmbca_led {
	struct led_classdev cdev;
	void __iomem *base;
	spinlock_t *lock;
	unsigned int idx;
	bool active_low;
	u32 num_serial_shifters;
	enum bcmbca_led_type led_type;
};

static void bcmbca_led_blink_disable(struct led_classdev *ldev)
{
	struct bcmbca_led *led =
		container_of(ldev, struct bcmbca_led, cdev);
	/* 8 LEDs per register so integer-divide by 8 */
	u8 led_offset = (led->idx >> 3);
	/* Find the 4 bits for each LED */
	u32 led_mask = 0xf << ((led->idx & 0x07) << 2);
	u32 val;

	/* Write registers */
	guard(spinlock_irqsave)(led->lock);
	val = readl(led->base + BCMBCA_FLASH_RATE + led_offset);
	val &= led_mask;
	writel(val, led->base + BCMBCA_FLASH_RATE + led_offset);
}

static int bcmbca_led_blink_set(struct led_classdev *ldev,
				unsigned long *delay_on,
				unsigned long *delay_off)
{
	struct bcmbca_led *led =
		container_of(ldev, struct bcmbca_led, cdev);
	u8 led_offset = (led->idx >> 3);
	u32 led_mask = 0xf << ((led->idx & 0x07) << 2);
	unsigned long period;
	u32 led_val;
	u32 val;

	/* Friendly defaults as specified in the documentation */
	if (*delay_on == 0 && *delay_off == 0) {
		*delay_on = 240;
		*delay_off = 240;
	}

	if (*delay_on != *delay_off) {
		dev_dbg(ldev->dev, "only square blink supported\n");
		return -EINVAL;
	}

	period = *delay_on + *delay_off;
	if (period > 2000) {
		dev_dbg(ldev->dev, "period too long, %lu max 2000 ms\n",
			period);
		return -EINVAL;
	}

	if (period <= 60)
		led_val = 1;
	else if (period <= 120)
		led_val = 2;
	else if (period <= 240)
		led_val = 3;
	else if (period <= 480)
		led_val = 4;
	else if (period <= 960)
		led_val = 5;
	else if (period <= 1920)
		led_val = 6;
	else
		led_val = 7;

	led_val = led_val << ((led->idx & 0x07) << 2);

	/* Write registers */
	guard(spinlock_irqsave)(led->lock);
	val = readl(led->base + BCMBCA_FLASH_RATE + led_offset);
	val &= led_mask;
	val |= led_val;
	writel(val, led->base + BCMBCA_FLASH_RATE + led_offset);

	return 0;
}

static void bcmbca_led_set(struct led_classdev *ldev,
			    enum led_brightness value)
{
	struct bcmbca_led *led =
		container_of(ldev, struct bcmbca_led, cdev);
	u8 led_offset = (led->idx >> 3);
	u32 led_mask = 0xf << ((led->idx & 0x07) << 2);
	u32 led_val = value << ((led->idx & 0x07) << 2);
	u32 val;

	dev_dbg(ldev->dev, "LED%u, register %08x, mask %08x, value %08x\n",
		led->idx, BCMBCA_BRIGHTNESS + led_offset, led_mask, led_val);

	/* Write registers */
	guard(spinlock_irqsave)(led->lock);

	/* Parallel LEDs support brightness control */
	if (led->led_type == BCMBCA_LED_PARALLEL) {
		val = readl(led->base + BCMBCA_BRIGHTNESS + led_offset);
		val &= led_mask;
		val |= led_val;
		writel(val, led->base + BCMBCA_BRIGHTNESS + led_offset);
	}

	/* Software control on/off */
	if (value == LED_OFF) {
		val = readl(led->base + BCMBCA_SW_DATA);
		val &= ~BIT(led->idx);
		writel(val, led->base + BCMBCA_SW_DATA);
		bcmbca_led_blink_disable(ldev);
	} else {
		val = readl(led->base + BCMBCA_SW_DATA);
		val |= BIT(led->idx);
		writel(val, led->base + BCMBCA_SW_DATA);
	}
}

static u8 bcmbca_led_get(void __iomem *base, int idx)
{
	u8 led_offset = (idx >> 3);
	u32 led_mask = 0xf << ((idx & 0x07) << 2);
	u32 val;

	/* Called marshalled so no lock needed */
	val = readl(base + BCMBCA_BRIGHTNESS + led_offset);
	return ((val & led_mask) >> ((idx & 0x07) << 2));
}

static int bcmbca_led_probe(struct device *dev, struct device_node *np, u32 reg,
			    void __iomem *base, spinlock_t *lock,
			    u32 num_shifters, enum bcmbca_led_type led_type)
{
	struct led_init_data init_data = {};
	struct bcmbca_led *led;
	enum led_default_state state;
	u32 val;
	int rc;

	led = devm_kzalloc(dev, sizeof(*led), GFP_KERNEL);
	if (!led)
		return -ENOMEM;

	led->idx = reg;
	led->base = base;
	led->lock = lock;

	if (of_property_read_bool(np, "active-low"))
		led->active_low = true;

	init_data.fwnode = of_fwnode_handle(np);

	if (led->led_type == BCMBCA_LED_PARALLEL)
		led->cdev.max_brightness = BCMBCA_LED_MAX_BRIGHTNESS;
	else
		led->cdev.max_brightness = LED_ON;

	state = led_init_default_state_get(init_data.fwnode);

	switch (state) {
	case LEDS_DEFSTATE_ON:
		led->cdev.brightness = led->cdev.max_brightness;
		break;
	case LEDS_DEFSTATE_KEEP:
		val = bcmbca_led_get(base, led->idx);
		if (val)
			led->cdev.brightness = val;
		else
			led->cdev.brightness = LED_OFF;
		break;
	default:
		led->cdev.brightness = LED_OFF;
		break;
	}

	/*
	 * Polarity inversion setting per-LED
	 * The default is actually active low, we set a bit to 1
	 * in the register to make it active high.
	 */
	if (!of_property_read_bool(np, "active-low")) {
		switch (led_type) {
		case BCMBCA_LED_SERIAL:
			val = readl(base + BCMBCA_SERIAL_POLARITY);
			val |= BIT(led->idx);
			writel(val, base + BCMBCA_SERIAL_POLARITY);
			break;
		case BCMBCA_LED_PARALLEL:
			val = readl(base + BCMBCA_PARALLEL_POLARITY);
			val |= BIT(led->idx);
			writel(val, base + BCMBCA_PARALLEL_POLARITY);
			break;
		default:
			break;
		}
	}

	/* Initial brightness setup */
	bcmbca_led_set(&led->cdev, led->cdev.brightness);

	led->cdev.brightness_set = bcmbca_led_set;
	led->cdev.blink_set = bcmbca_led_blink_set;
	/* TODO: implement HW control */

	rc = devm_led_classdev_register_ext(dev, &led->cdev, &init_data);
	if (rc < 0)
		return rc;

	dev_info(dev, "registered LED %s\n", led->cdev.name);

	return 0;
}

static int bcmbca_leds_probe(struct platform_device *pdev)
{
	unsigned int max_leds = BCMBCA_LED_MAX_COUNT;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev_of_node(&pdev->dev);
	enum bcmbca_led_type led_type;
	void __iomem *base;
	spinlock_t *lock; /* memory lock */
	u32 num_shifters;
	u32 val;
	int ret;
	int i;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	lock = devm_kzalloc(dev, sizeof(*lock), GFP_KERNEL);
	if (!lock)
		return -ENOMEM;

	ret = of_property_read_u32(np, "brcm,serial-shifters", &num_shifters);
	if (!ret) {
		/* Serial LEDs */
		dev_dbg(dev, "serial LEDs, %d shift registers used\n", num_shifters);
		led_type = BCMBCA_LED_SERIAL;
		/*
		 * Each shifter can handle maximum 8 LEDs so we cap the
		 * maximum LEDs we can handle at that.
		 */
		max_leds = num_shifters * 8;
	} else {
		/* Parallel LEDs */
		led_type = BCMBCA_LED_PARALLEL;
		dev_info(dev, "parallel LEDs requested, this is untested\n");
	}

	spin_lock_init(lock);

	/* Turn off all LEDs and let the driver deal with them */
	writel(0, base + BCMBCA_HW_EN);
	writel(0, base + BCMBCA_SERIAL_POLARITY);
	writel(0, base + BCMBCA_PARALLEL_POLARITY);

	/* Set up serial shift register */
	switch (num_shifters) {
	case 0:
		val = 0;
		break;
	case 1:
		val = 0x000000ff;
		break;
	case 2:
		val = 0x0000ffff;
		break;
	case 3:
		val = 0x00ffffff;
		break;
	case 4:
		val = 0xffffffff;
		break;
	}
	writel(val, base + BCMBCA_SERIAL_SHIFT_SEL);

	/* ??? */
	writel(0xc0000000, base + BCMBCA_HW_POLARITY);
	/* Initialize to max brightness */
	for (i = 0; i < BCMBCA_LED_MAX_COUNT/8; i++) {
		writel(0x88888888, base + BCMBCA_BRIGHTNESS + 4*i);
		writel(0, base + BCMBCA_BRIGHTNESS + BCMBCA_FLASH_RATE + 4*i);
	}

	for_each_available_child_of_node_scoped(np, child) {
		u32 reg;

		if (of_property_read_u32(child, "reg", &reg))
			continue;

		if (reg >= max_leds) {
			dev_err(dev, "invalid LED (%u >= %d)\n", reg,
				max_leds);
			continue;
		}

		ret = bcmbca_led_probe(dev, child, reg, base, lock,
				       num_shifters, led_type);
		if (ret < 0)
			return ret;
	}

	/* Enable the LEDs */
	val = BCMBCA_CTRL_ENABLE | BCMBCA_CTRL_SERIAL_POL_HIGH;
	if (of_property_read_bool(np, "brcm,serial-active-low"))
		val &= ~BCMBCA_CTRL_SERIAL_POL_HIGH;
	writel(val, base + BCMBCA_CTRL);

	return 0;
}

static const struct of_device_id bcmbca_leds_of_match[] = {
	{ .compatible = "brcm,bcmbca-leds", },
	{ },
};
MODULE_DEVICE_TABLE(of, bcmbca_leds_of_match);

static struct platform_driver bcmbca_leds_driver = {
	.probe = bcmbca_leds_probe,
	.driver = {
		.name = "leds-bcmbca",
		.of_match_table = bcmbca_leds_of_match,
	},
};

module_platform_driver(bcmbca_leds_driver);

MODULE_AUTHOR("Linus Walleij <linus.walleij@linaro.org>");
MODULE_DESCRIPTION("LED driver for BCMBCA LED controllers");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:leds-bcmbca");
