// SPDX-License-Identifier: GPL-2.0-only
/*
 * Orient Chip OCP8178 Backlight Driver
 *
 * Copyright (C) 2026 Wim de With
 *
 * Author: Wim de With <wf@dewith.io>
 */

#include <linux/backlight.h>
#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/irqflags.h>
#include <linux/platform_device.h>
#include <linux/property.h>

#define OCP8178_MAX_BRIGHTNESS 0x1F /* 5 bits */

#define OCP8178_DEVICE_ADDRESS 0x72

#define OCP8178_DATA_RFA BIT(7)
#define OCP8178_DATA_ADDR GENMASK(6, 5)
#define OCP8178_DATA_VALUE GENMASK(4, 0)

#define OCP8178_1W_INIT_MAX_RETRIES 5
#define OCP8178_1W_INIT_SLEEP_US (50 * 1000)

#define OCP8178_T_OFF_US (3 * 1000) /* datasheet specifies at least 2.5 ms */
#define OCP8178_1W_T_DELAY_US (100 + 10) /* 10 us as safety factor */
#define OCP8178_1W_T_DETECT_US (260 + 10) /* 10 us as safety factor */
#define OCP8178_1W_T_START_US 2
#define OCP8178_1W_T_EOS_US 2
#define OCP8178_1W_T_WIN_NS (1000 * 1000)

/*
 * The datasheet specifies 1.7 Kbps to 160 Kbps.
 * 1 / (160 Kbps) is about 6.67 us, so using 7 us per bit should be fine.
 * T_HIGH + T_LOW = 7 us
 * T_HIGH > 2 * T_LOW for high bits
 * T_LOW > 2 * T_HIGH for low bits
 */
#define OCP8178_1W_HIGH_BIT_T_LOW_US 2
#define OCP8178_1W_HIGH_BIT_T_HIGH_US 5
#define OCP8178_1W_LOW_BIT_T_LOW_US 5
#define OCP8178_1W_LOW_BIT_T_HIGH_US 2

struct ocp8178_bl {
	struct device *dev;
	struct gpio_desc *gpiod;
};

static int ocp8178_bl_enable_onewire(struct ocp8178_bl *ocp8178)
{
	u64 start, duration;

	dev_dbg(ocp8178->dev, "enabling onewire protocol\n");

	gpiod_set_value(ocp8178->gpiod, 0);
	fsleep(OCP8178_T_OFF_US);

	start = ktime_get_ns();

	gpiod_set_value(ocp8178->gpiod, 1);
	udelay(OCP8178_1W_T_DELAY_US);
	gpiod_set_value(ocp8178->gpiod, 0);
	udelay(OCP8178_1W_T_DETECT_US);
	gpiod_set_value(ocp8178->gpiod, 1);

	duration = ktime_get_ns() - start;

	if (duration >= OCP8178_1W_T_WIN_NS) {
		dev_err(ocp8178->dev,
			"onewire detection window exceeded (%llu ns)\n",
			duration);
		return -EAGAIN;
	}

	return 0;
}

static void ocp8178_bl_write_u8(struct ocp8178_bl *ocp8178, u8 value)
{
	unsigned long flags;

	gpiod_set_value(ocp8178->gpiod, 1);
	udelay(OCP8178_1W_T_START_US);

	local_irq_save(flags);

	for (int i = 7; i >= 0; i--) {
		if ((value >> i) & 1) {
			gpiod_set_value(ocp8178->gpiod, 0);
			udelay(OCP8178_1W_HIGH_BIT_T_LOW_US);
			gpiod_set_value(ocp8178->gpiod, 1);
			udelay(OCP8178_1W_HIGH_BIT_T_HIGH_US);
		} else {
			gpiod_set_value(ocp8178->gpiod, 0);
			udelay(OCP8178_1W_LOW_BIT_T_LOW_US);
			gpiod_set_value(ocp8178->gpiod, 1);
			udelay(OCP8178_1W_LOW_BIT_T_HIGH_US);
		}
	}

	gpiod_set_value(ocp8178->gpiod, 0);
	udelay(OCP8178_1W_T_EOS_US);
	gpiod_set_value(ocp8178->gpiod, 1);

	local_irq_restore(flags);
}

static void ocp8178_bl_set_brightness(struct ocp8178_bl *ocp8178, u8 brightness)
{
	u8 data = 0;

	/*
	 * We cannot set RFA (request for acknowledge) unless the GPIO pin is
	 * configured as open drain.
	 */
	data |= FIELD_PREP(OCP8178_DATA_RFA, 0);
	data |= FIELD_PREP(OCP8178_DATA_ADDR, 0);
	data |= FIELD_PREP(OCP8178_DATA_VALUE, brightness);

	ocp8178_bl_write_u8(ocp8178, OCP8178_DEVICE_ADDRESS);
	ocp8178_bl_write_u8(ocp8178, data);
}

static int ocp8178_bl_update_status(struct backlight_device *bl)
{
	struct ocp8178_bl *ocp8178 = bl_get_data(bl);
	u8 brightness = backlight_get_brightness(bl);

	ocp8178_bl_set_brightness(ocp8178, brightness);
	return 0;
}

static const struct backlight_ops ocp8178_bl_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.update_status = ocp8178_bl_update_status,
};

static int ocp8178_bl_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct backlight_device *bl;
	struct backlight_properties props;
	struct ocp8178_bl *ocp8178;
	u32 max_brightness, brightness;
	int ret, retries;

	ocp8178 = devm_kzalloc(dev, sizeof(*ocp8178), GFP_KERNEL);
	if (!ocp8178)
		return -ENOMEM;

	ocp8178->dev = dev;

	ret = device_property_read_u32(dev, "max-brightness", &max_brightness);
	if (ret)
		max_brightness = OCP8178_MAX_BRIGHTNESS;
	if (max_brightness > OCP8178_MAX_BRIGHTNESS) {
		dev_warn(dev, "max brightness exceeds hardware limit\n");
		max_brightness = OCP8178_MAX_BRIGHTNESS;
	}

	ret = device_property_read_u32(dev, "default-brightness", &brightness);
	if (ret)
		brightness = max_brightness;
	if (brightness > max_brightness) {
		dev_warn(dev, "default brightness exceeds max brightness\n");
		brightness = max_brightness;
	}

	ocp8178->gpiod = devm_gpiod_get(dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(ocp8178->gpiod))
		return dev_err_probe(dev, PTR_ERR(ocp8178->gpiod),
				     "GPIO missing or invalid\n");
	if (gpiod_cansleep(ocp8178->gpiod))
		return dev_err_probe(dev, -EINVAL,
				     "sleeping GPIO not supported\n");
	gpiod_set_consumer_name(ocp8178->gpiod, dev_name(dev));

	for (retries = 0; retries < OCP8178_1W_INIT_MAX_RETRIES; retries++) {
		ret = ocp8178_bl_enable_onewire(ocp8178);
		if (!ret)
			break;
		if (ret != -EAGAIN)
			return ret;
		fsleep(OCP8178_1W_INIT_SLEEP_US);
	}
	if (retries >= OCP8178_1W_INIT_MAX_RETRIES)
		return dev_err_probe(dev, -ETIMEDOUT,
				     "failed to initialize onewire protocol\n");

	props = (typeof(props)){
		.type = BACKLIGHT_RAW,
		.brightness = brightness,
		.max_brightness = max_brightness,
		.power = BACKLIGHT_POWER_ON,
		.scale = BACKLIGHT_SCALE_NON_LINEAR,
	};

	bl = devm_backlight_device_register(dev, dev_name(dev), dev, ocp8178,
					    &ocp8178_bl_ops, &props);
	if (IS_ERR(bl))
		return dev_err_probe(dev, PTR_ERR(bl),
				     "failed to register backlight\n");

	platform_set_drvdata(pdev, bl);
	backlight_update_status(bl);

	dev_dbg(dev, "probed, brightness=%u/%u\n", brightness, max_brightness);

	return 0;
}

static const struct of_device_id ocp8178_bl_of_match[] = {
	{ .compatible = "ocs,ocp8178" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ocp8178_bl_of_match);

static struct platform_driver ocp8178_bl_driver = {
	.driver = {
		.name = "ocp8178-bl",
		.of_match_table = ocp8178_bl_of_match,
	},
	.probe = ocp8178_bl_probe,
};
module_platform_driver(ocp8178_bl_driver);

MODULE_AUTHOR("Wim de With <wf@dewith.io>");
MODULE_DESCRIPTION("Orient Chip OCP8178 Backlight Driver");
MODULE_LICENSE("GPL");
