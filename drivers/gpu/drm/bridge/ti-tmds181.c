// SPDX-License-Identifier: GPL-2.0
/*
 * TI tmds181 and sn65dp159 HDMI redriver and retimer chips
 *
 * Copyright (C) 2018 - 2025 Topic Embedded Products <www.topic.nl>
 *
 * based on code
 * Copyright (C) 2007 Hans Verkuil
 * Copyright (C) 2016, 2017 Leon Woestenberg <leon@sidebranch.com>
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/gpio/consumer.h>
#include <linux/delay.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_crtc.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>

MODULE_DESCRIPTION("I2C device driver for DP159 and TMDS181 redriver/retimer");
MODULE_AUTHOR("Mike Looijmans");
MODULE_LICENSE("GPL");

#define TMDS181_REG_ID		0
#define TMDS181_REG_REV		0x8
#define TMDS181_REG_CTRL9	0x9
/* Registers A and B have a volatile bit, but we don't use it, so cache is ok */
#define TMDS181_REG_CTRLA	0xA
#define TMDS181_REG_CTRLB	0xB
#define TMDS181_REG_CTRLC	0xC
#define TMDS181_REG_EQUALIZER	0xD
#define TMDS181_REG_EYESCAN	0xE

enum tmds181_chip {
	tmds181,
	dp159,
};

struct tmds181_data {
	struct i2c_client *client;
	struct regmap *regmap;
	struct drm_bridge *next_bridge;
	struct drm_bridge bridge;
	enum tmds181_chip chip;
};

static inline struct tmds181_data *
drm_bridge_to_tmds181_data(struct drm_bridge *bridge)
{
	return container_of(bridge, struct tmds181_data, bridge);
}

/* Set "apply" bit in control register after making changes */
static int tmds181_apply_changes(struct tmds181_data *data)
{
	return regmap_write_bits(data->regmap,
		TMDS181_REG_CTRLA, BIT(2), BIT(2));
}

static int tmds181_attach(struct drm_bridge *bridge, struct drm_encoder *encoder,
			  enum drm_bridge_attach_flags flags)
{
	struct tmds181_data *data = drm_bridge_to_tmds181_data(bridge);

	return drm_bridge_attach(encoder, data->next_bridge, bridge, flags);
}

static enum drm_mode_status
tmds181_mode_valid(struct drm_bridge *bridge, const struct drm_display_info *info,
		   const struct drm_display_mode *mode)
{
	/* Clock limits: clk between 25 and 350 MHz, clk is 1/10 of bit clock */
	if (mode->clock < 25000)
		return MODE_CLOCK_LOW;

	/* The actual HDMI clock (if provided) cannot exceed 350MHz */
	if (mode->crtc_clock > 350000)
		return MODE_CLOCK_HIGH;

	/*
	 * When in 4k mode, the clock is 1/40th of the bitrate. The limit is
	 * then the data rate of 6Gbps, which would use a 600MHz pixel clock.
	 */
	if (mode->clock > 600000)
		return MODE_CLOCK_HIGH;

	return MODE_OK;
}

static void tmds181_enable(struct drm_bridge *bridge)
{
	struct tmds181_data *data = drm_bridge_to_tmds181_data(bridge);

	/* Clear the PD_EN bit */
	regmap_update_bits(data->regmap, TMDS181_REG_CTRL9, BIT(3), 0);
}

static void tmds181_disable(struct drm_bridge *bridge)
{
	struct tmds181_data *data = drm_bridge_to_tmds181_data(bridge);

	/* Set the PD_EN bit */
	regmap_update_bits(data->regmap, TMDS181_REG_CTRL9, BIT(3), BIT(3));
}

static const struct drm_bridge_funcs tmds181_bridge_funcs = {
	.attach		= tmds181_attach,
	.mode_valid	= tmds181_mode_valid,
	.enable		= tmds181_enable,
	.disable	= tmds181_disable,
};

static const char * const tmds181_modes[] = {
	"redriver",
	"auto1",
	"auto2",
	"retimer",
};

static ssize_t mode_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct tmds181_data *data = dev_get_drvdata(dev);
	const char *equalizer;
	u32 val;
	int ret;

	ret = regmap_read(data->regmap, TMDS181_REG_CTRLA, &val);
	if (ret < 0)
		return ret;

	if (val & BIT(4)) {
		if (val & BIT(5))
			equalizer = "eq-adaptive";
		else
			equalizer = "eq-fixed";
	} else {
		equalizer = "eq-disabled";
	}

	return scnprintf(buf, PAGE_SIZE, "%6s %s %s\n",
			(val & BIT(7)) ? "sink" : "source",
			tmds181_modes[val & 0x03],
			equalizer);
}

static ssize_t mode_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	struct tmds181_data *data = dev_get_drvdata(dev);
	u32 val;
	int ret;
	int i;

	/* Strip trailing newline(s) for being user friendly */
	while (len && buf[len] == '\n')
		--len;

	/* Need at least 4 actual characters */
	if (len < 4)
		return -EINVAL;

	ret = regmap_read(data->regmap, TMDS181_REG_CTRLA, &val);
	if (ret < 0)
		return ret;

	for (i = 0; i < ARRAY_SIZE(tmds181_modes); ++i) {
		if (strncmp(tmds181_modes[i], buf, len) == 0) {
			val &= ~0x03;
			val |= i;
			break;
		}
	}

	if (strncmp("sink", buf, len) == 0)
		val |= BIT(7);

	if (strncmp("source", buf, len) == 0)
		val &= ~BIT(7);

	if (strncmp("eq-", buf, 3) == 0) {
		switch (buf[3]) {
		case 'a': /* adaptive */
			val |= BIT(4) | BIT(5);
			break;
		case 'f': /* fixed */
			val |= BIT(4);
			val &= ~BIT(5);
			break;
		case 'd': /* disable */
			val &= ~(BIT(4) | BIT(5));
			break;
		}
	}

	/* Always set the "apply changes" bit */
	val |= BIT(2);

	ret = regmap_write(data->regmap, TMDS181_REG_CTRLA, val);
	if (ret < 0)
		return ret;

	return len;
}

/* termination for HDMI TX: 0=off, 1=150..300, 3=75..150 ohms */
static ssize_t termination_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct tmds181_data *data = dev_get_drvdata(dev);
	u32 val;
	int ret;

	ret = regmap_read(data->regmap, TMDS181_REG_CTRLB, &val);
	if (ret < 0)
		return ret;

	val >>= 3;
	val &= 0x03;

	return scnprintf(buf, PAGE_SIZE, "%u\n", val);
}

static ssize_t termination_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	struct tmds181_data *data = dev_get_drvdata(dev);
	u32 val;
	unsigned long newval;
	int ret;

	ret = regmap_read(data->regmap, TMDS181_REG_CTRLB, &val);
	if (ret < 0)
		return ret;

	ret = kstrtoul((const char *) buf, 10, &newval);
	if (ret)
		return ret;

	if (newval > 3)
		return -EINVAL;

	val &= ~(0x03 << 3);
	val |= newval << 3;

	ret = regmap_write(data->regmap, TMDS181_REG_CTRLB, val);
	if (ret < 0)
		return ret;

	return len;
}

static DEVICE_ATTR_RW(mode);
static DEVICE_ATTR_RW(termination);

static struct attribute *tmds181_attrs[] = {
	&dev_attr_mode.attr,
	&dev_attr_termination.attr,
	NULL,
};

static const struct attribute_group tmds181_attr_group = {
	.attrs = tmds181_attrs,
};

static const u8 tmds181_id_tmds181[8] = "TMDS181 ";
static const u8 tmds181_id_dp159[8]   = "DP159   ";

static int tmds181_check_id(struct tmds181_data *data)
{
	int ret;
	int retry;
	u8 buffer[8];

	for (retry = 0; retry < 20; ++retry) {
		ret = regmap_bulk_read(data->regmap, TMDS181_REG_ID, buffer,
				       sizeof(buffer));
		if (!ret)
			break;

		/* Compensate for very long OE power-up delays due to the cap */
		usleep_range(5000, 10000);
	}

	if (ret) {
		dev_err(&data->client->dev, "I2C read ID failed\n");
		return ret;
	}

	if (memcmp(buffer, tmds181_id_tmds181, sizeof(buffer)) == 0) {
		dev_info(&data->client->dev, "Detected: TMDS181\n");
		data->chip = tmds181;
		return 0;
	}

	if (memcmp(buffer, tmds181_id_dp159, sizeof(buffer)) == 0) {
		dev_info(&data->client->dev, "Detected: DP159\n");
		data->chip = dp159;
		return 0;
	}

	dev_err(&data->client->dev, "Unknown or wrong ID: %*pE\n", (int)sizeof(buffer), buffer);

	return -ENODEV;
}

static bool tmds181_regmap_is_volatile(struct device *dev, unsigned int reg)
{
	switch (reg) {
	/* IBERT result and status registers, not used yet */
	case 0x15:
	case 0x17 ... 0x1F:
		return true;
	default:
		return false;
	}
}

static const struct regmap_config tmds181_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.cache_type = REGCACHE_RBTREE,
	.max_register = 0x20,
	.volatile_reg = tmds181_regmap_is_volatile,
};

static int tmds181_probe(struct i2c_client *client)
{
	struct tmds181_data *data;
	struct gpio_desc *oe_gpio;
	int ret;
	u32 param;
	u8 val;

	/* Check if the adapter supports the needed features */
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE_DATA))
		return -EIO;

	data = devm_kzalloc(&client->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->client = client;
	i2c_set_clientdata(client, data);
	data->regmap = devm_regmap_init_i2c(client, &tmds181_regmap_config);
	if (IS_ERR(data->regmap))
		return PTR_ERR(data->regmap);

	/* The "OE" pin acts as a reset */
	oe_gpio = devm_gpiod_get_optional(&client->dev, "oe", GPIOD_OUT_LOW);
	if (IS_ERR(oe_gpio)) {
		ret = PTR_ERR(oe_gpio);
		if (ret != -EPROBE_DEFER)
			dev_err(&client->dev, "failed to acquire 'oe' gpio\n");
		return ret;
	}
	if (oe_gpio) {
		/* Need at least 100us reset pulse */
		usleep_range(100, 200);
		gpiod_set_value_cansleep(oe_gpio, 1);
	}

	/* Reading the ID also provides enough time for the reset */
	ret = tmds181_check_id(data);
	if (ret)
		return ret;

	/* We take care of power control, so disable the chips PM functions */
	/* SIG_EN=0 PD_EN=1 HPD_AUTO_PWRDWN_DISABLE=1 I2C_DR_CTL=0b11*/
	regmap_update_bits(data->regmap, TMDS181_REG_CTRL9, 0x1f, 0x0f);

	/* Apply configuration changes */
	if (of_property_read_bool(client->dev.of_node, "source-mode"))
		regmap_update_bits(data->regmap,
				TMDS181_REG_CTRLA, BIT(7), 0);
	if (of_property_read_bool(client->dev.of_node, "sink-mode"))
		regmap_update_bits(data->regmap,
				TMDS181_REG_CTRLA, BIT(7), BIT(7));
	if (of_property_read_bool(client->dev.of_node, "redriver-mode"))
		regmap_update_bits(data->regmap,
				TMDS181_REG_CTRLA, 0x03, 0x00);
	if (of_property_read_bool(client->dev.of_node, "retimer-mode"))
		regmap_update_bits(data->regmap,
				TMDS181_REG_CTRLA, 0x03, 0x03);
	if (of_property_read_bool(client->dev.of_node, "adaptive-equalizer"))
		regmap_update_bits(data->regmap,
			TMDS181_REG_CTRLA, BIT(4) | BIT(5), BIT(4) | BIT(5));
	if (of_property_read_bool(client->dev.of_node, "disable-equalizer"))
		regmap_update_bits(data->regmap, TMDS181_REG_CTRLA, BIT(4), 0);

	switch (data->chip) {
	case dp159:
		/*  SLEW=0b00 Mode=HDMI DDC_TRAIN_SET=1 */
		val = BIT(0);
		/* Default slew rate is max */
		param = 3;
		if (!of_property_read_u32(client->dev.of_node,
					"slew-rate", &param)) {
			if (param > 3) {
				dev_err(&client->dev, "invalid slew-rate\n");
				return -EINVAL;
			}
			/* Implement 0 = slow, 3 = fast slew rate */
			val = (3 - param) << 6;
		}
		if (of_property_read_bool(client->dev.of_node, "dvi-mode"))
			val |= BIT(5);
		break;
	default:
		/* DDC_DR_SEL=1 DDC_TRAIN_SETDISABLE=1 */
		val = BIT(2) | BIT(0);
		break;
	}

	/* termination for HDMI TX: 0=off, 1=150..300, 3=75..150 ohms */
	if (!of_property_read_u32(client->dev.of_node, "termination", &param))
		val |= ((param & 0x3) << 3);

	ret = regmap_write(data->regmap, TMDS181_REG_CTRLB, val);
	if (ret < 0) {
		dev_err(&client->dev, "regmap_write(B) failed\n");
		return ret;
	}

	val = 0; /* Default for C register */
	if (!of_property_read_u32(client->dev.of_node, "vswing-data", &param))
		val |= (param << 5);
	if (!of_property_read_u32(client->dev.of_node, "vswing-clk", &param))
		val |= (param << 2);
	/* Datasheet recommends HDMI_TWPST=0b01 for HDMI compliance */
	if (of_property_read_bool(client->dev.of_node, "enable-de-emphasis"))
		val |= 0x01;
	ret = regmap_write(data->regmap, TMDS181_REG_CTRLC, val);
	if (ret < 0) {
		dev_err(&client->dev, "regmap_write(C) failed\n");
		return ret;
	}

	/* DIS_HDMI2_SWG: HDMI 1.x only, keep clock at full rate */
	val = BIT(0);
	if (!of_property_read_u32(client->dev.of_node, "eq-data", &param)) {
		val |= (param << 3);
		/* If defined, also force the "fixed" equalizer mode */
		regmap_update_bits(data->regmap, TMDS181_REG_CTRLA, BIT(5), 0);
	}
	if (!of_property_read_u32(client->dev.of_node, "eq-clk", &param)) {
		val |= (param << 1);
		regmap_update_bits(data->regmap, TMDS181_REG_CTRLA, BIT(5), 0);
	}
	ret = regmap_write(data->regmap, TMDS181_REG_EQUALIZER, val);
	if (ret < 0) {
		dev_err(&client->dev, "regmap_write(EQUALIZER) failed\n");
		return ret;
	}

	ret = tmds181_apply_changes(data);
	if (ret < 0) {
		dev_err(&client->dev, "tmds181_apply_changes failed\n");
		return ret;
	}

	ret = sysfs_create_group(&client->dev.kobj, &tmds181_attr_group);
	if (ret)
		dev_err(&client->dev, "sysfs_create_group error: %d\n", ret);

	/* Find next bridge in chain */
	data->next_bridge = devm_drm_of_get_bridge(&client->dev, client->dev.of_node, 1, 0);
	if (IS_ERR(data->next_bridge))
		return dev_err_probe(&client->dev, PTR_ERR(data->next_bridge),
				     "Failed to find next bridge\n");

	/* Register the bridge. */
	data->bridge.funcs = &tmds181_bridge_funcs;
	data->bridge.of_node = client->dev.of_node;

	return devm_drm_bridge_add(&client->dev, &data->bridge);
}

static const struct i2c_device_id tmds181_id[] = {
	{ "tmds181", tmds181 },
	{ "sn65dp159", dp159 },
	{}
};
MODULE_DEVICE_TABLE(i2c, tmds181_id);

#if IS_ENABLED(CONFIG_OF)
static const struct of_device_id tmds181_of_match[] = {
	{ .compatible = "ti,tmds181", },
	{ .compatible = "ti,sn65dp159", },
	{}
};
MODULE_DEVICE_TABLE(of, tmds181_of_match);
#endif

static struct i2c_driver tmds181_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.name	= "tmds181",
		.of_match_table = of_match_ptr(tmds181_of_match),
	},
	.probe		= tmds181_probe,
	.id_table	= tmds181_id,
};

module_i2c_driver(tmds181_driver);
