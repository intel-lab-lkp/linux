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

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/slab.h>

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
#define TMDS181_REG_CTRLA	0xa
#define TMDS181_REG_CTRLB	0xb
#define TMDS181_REG_CTRLC	0xc
#define TMDS181_REG_EQUALIZER	0xd
/* EYESCAN registers don't appear to deserve separate names */
#define TMDS181_REG_EYESCAN_E	0xe
#define TMDS181_REG_EYESCAN_F	0xf
#define TMDS181_REG_EYESCAN_15	0x15
#define TMDS181_REG_EYESCAN_17	0x17
#define TMDS181_REG_EYESCAN_1F	0x1f
#define TMDS181_REG_AUX		0x20


#define TMDS181_CTRL9_SIG_EN			BIT(4)
#define TMDS181_CTRL9_PD_EN			BIT(3)
#define TMDS181_CTRL9_HPD_AUTO_PWRDWN_DISABLE	BIT(2)
#define TMDS181_CTRL9_I2C_DR_CTL		GENMASK(1, 0)

#define TMDS181_CTRLA_MODE_SINK			BIT(7)
#define TMDS181_CTRLA_HPDSNK_GATE_EN		BIT(6)
#define TMDS181_CTRLA_EQ_ADA_EN			BIT(5)
#define TMDS181_CTRLA_EQ_EN			BIT(4)
#define TMDS181_CTRLA_AUX_BRG_EN		BIT(3)
#define TMDS181_CTRLA_APPLY			BIT(2)
#define TMDS181_CTRLA_DEV_FUNC_MODE		GENMASK(1, 0)

#define TMDS181_CTRLB_SLEW_CTL			GENMASK(7, 6)
#define TMDS181_CTRLB_HDMI_SEL_DVI		BIT(5)
#define TMDS181_CTRLB_TX_TERM_CTL		GENMASK(4, 3)
#define TMDS181_CTRLB_DDC_DR_SEL		BIT(2)
#define TMDS181_CTRLB_TMDS_CLOCK_RATIO_STATUS	BIT(1)
#define TMDS181_CTRLB_DDC_TRAIN_SET		BIT(0)

#define TMDS181_CTRLB_TX_TERM_150_300_OHMS	1
#define TMDS181_CTRLB_TX_TERM_75_150_OHMS	3

#define TMDS181_CTRLC_VSWING_DATA		GENMASK(7, 5)
#define TMDS181_CTRLC_VSWING_CLK		GENMASK(4, 2)
#define TMDS181_CTRLC_HDMI_TWPST1		GENMASK(1, 0)

#define TMDS181_EQ_DATA_LANE			GENMASK(5, 3)
#define TMDS181_EQ_CLOCK_LANE			GENMASK(2, 1)
#define TMDS181_EQ_DIS_HDMI2_SWG		BIT(0)

/* Above this data rate HDMI2 standards apply (TX termination) */
#define HDMI2_PIXEL_RATE_KHZ	340000

enum tmds181_chip {
	tmds181,
	dp159,
};

struct tmds181_data {
	struct i2c_client *client;
	struct regmap *regmap;
	struct drm_bridge *next_bridge;
	struct drm_bridge bridge;
	u32 retimer_threshold_khz;
};

static inline struct tmds181_data *
drm_bridge_to_tmds181_data(struct drm_bridge *bridge)
{
	return container_of(bridge, struct tmds181_data, bridge);
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
	 * When in HDMI 2 mode, the clock is 1/40th of the bitrate. The limit is
	 * then the data rate of 6Gbps, which would use a 600MHz pixel clock.
	 */
	if (mode->clock > 600000)
		return MODE_CLOCK_HIGH;

	return MODE_OK;
}

static void tmds181_enable(struct drm_bridge *bridge, struct drm_atomic_state *state)
{
	struct tmds181_data *data = drm_bridge_to_tmds181_data(bridge);
	const struct drm_crtc_state *crtc_state;
	const struct drm_display_mode *mode;
	struct drm_connector *connector;
	struct drm_crtc *crtc;
	unsigned int val;

	/*
	 * Retrieve the CRTC adjusted mode. This requires a little dance to go
	 * from the bridge to the encoder, to the connector and to the CRTC.
	 */
	connector = drm_atomic_get_new_connector_for_encoder(state,
							     bridge->encoder);
	crtc = drm_atomic_get_new_connector_state(state, connector)->crtc;
	crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	mode = &crtc_state->adjusted_mode;

	/* Set retimer/redriver mode based on pixel clock */
	val = mode->clock > data->retimer_threshold_khz ? TMDS181_CTRLA_DEV_FUNC_MODE : 0;
	regmap_update_bits(data->regmap, TMDS181_REG_CTRLA,
			   TMDS181_CTRLA_DEV_FUNC_MODE, val);

	/* Configure TX termination based on pixel clock */
	val = mode->clock > HDMI2_PIXEL_RATE_KHZ ?
				TMDS181_CTRLB_TX_TERM_75_150_OHMS :
				TMDS181_CTRLB_TX_TERM_150_300_OHMS;
	regmap_update_bits(data->regmap, TMDS181_REG_CTRLB,
			   TMDS181_CTRLB_TX_TERM_CTL,
			   FIELD_PREP(TMDS181_CTRLB_TX_TERM_CTL, val));

	regmap_update_bits(data->regmap, TMDS181_REG_CTRL9,
			   TMDS181_CTRL9_PD_EN, 0);
}

static void tmds181_disable(struct drm_bridge *bridge, struct drm_atomic_state *state)
{
	struct tmds181_data *data = drm_bridge_to_tmds181_data(bridge);

	/* Set the PD_EN bit */
	regmap_update_bits(data->regmap, TMDS181_REG_CTRL9,
			   TMDS181_CTRL9_PD_EN, TMDS181_CTRL9_PD_EN);
}

static const struct drm_bridge_funcs tmds181_bridge_funcs = {
	.attach		= tmds181_attach,
	.mode_valid	= tmds181_mode_valid,
	.atomic_enable	= tmds181_enable,
	.atomic_disable	= tmds181_disable,

	.atomic_reset = drm_atomic_helper_bridge_reset,
	.atomic_duplicate_state = drm_atomic_helper_bridge_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_bridge_destroy_state,
};

static const u8 tmds181_id_tmds181[8] __nonstring = "TMDS181 ";
static const u8 tmds181_id_dp159[8]   __nonstring = "DP159   ";

static int tmds181_check_id(struct tmds181_data *data, enum tmds181_chip *chip)
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
		if (*chip != tmds181) {
			dev_warn(&data->client->dev, "Detected: TMDS181\n");
			*chip = tmds181;
		}
		return 0;
	}

	if (memcmp(buffer, tmds181_id_dp159, sizeof(buffer)) == 0) {
		if (*chip != dp159) {
			dev_warn(&data->client->dev, "Detected: DP159\n");
			*chip = dp159;
		}
		return 0;
	}

	dev_err(&data->client->dev, "Unknown ID: %*pE\n", (int)sizeof(buffer), buffer);

	return -ENODEV;
}

static bool tmds181_regmap_is_volatile(struct device *dev, unsigned int reg)
{
	switch (reg) {
	/* IBERT result and status registers, not used yet */
	case TMDS181_REG_EYESCAN_15:
	case TMDS181_REG_EYESCAN_17 ... TMDS181_REG_EYESCAN_1F:
		return true;
	default:
		return false;
	}
}

static const struct regmap_config tmds181_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.cache_type = REGCACHE_RBTREE,
	.max_register = TMDS181_REG_AUX,
	.volatile_reg = tmds181_regmap_is_volatile,
};

static int tmds181_probe(struct i2c_client *client)
{
	struct tmds181_data *data;
	struct gpio_desc *oe_gpio;
	enum tmds181_chip chip;
	int ret;
	u32 param;
	u8 val;

	/* Check if the adapter supports the needed features */
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE_DATA))
		return -EIO;

	data = devm_drm_bridge_alloc(&client->dev, struct tmds181_data, bridge,
				     &tmds181_bridge_funcs);
	if (IS_ERR(data))
		return PTR_ERR(data);

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
	chip = (enum tmds181_chip)of_device_get_match_data(&client->dev);
	ret = tmds181_check_id(data, &chip);
	if (ret)
		return ret;

	/*
	 * We take care of power control, so disable the chips PM functions, and
	 * allow the DDC to run at 400kHz
	 */
	regmap_update_bits(data->regmap, TMDS181_REG_CTRL9,
			TMDS181_CTRL9_SIG_EN | TMDS181_CTRL9_PD_EN |
			TMDS181_CTRL9_HPD_AUTO_PWRDWN_DISABLE |
			TMDS181_CTRL9_I2C_DR_CTL,
			TMDS181_CTRL9_PD_EN |
			TMDS181_CTRL9_HPD_AUTO_PWRDWN_DISABLE |
			TMDS181_CTRL9_I2C_DR_CTL);

	/* Apply configuration changes */
	if (of_property_read_bool(client->dev.of_node, "ti,source-mode"))
		regmap_update_bits(data->regmap, TMDS181_REG_CTRLA,
				   TMDS181_CTRLA_MODE_SINK, 0);
	if (of_property_read_bool(client->dev.of_node, "ti,sink-mode"))
		regmap_update_bits(data->regmap, TMDS181_REG_CTRLA,
				   TMDS181_CTRLA_MODE_SINK, TMDS181_CTRLA_MODE_SINK);

	/*
	 * Using the automatic modes of the chip uses considerable power as it
	 * will keep the PLL running at all times. So instead, define our own
	 * threshold for the pixel rate. This also allows to use a sane default
	 * of 200MHz pixel rate for the redriver-retimer crossover point, as the
	 * modes below 3k don't show any benefit from the retimer.
	 */
	data->retimer_threshold_khz = 200000;
	if (!of_property_read_u32(client->dev.of_node,
				  "ti,retimer-threshold-hz", &param))
		data->retimer_threshold_khz = param / 1000;

	/* Default to low-power redriver mode */
	regmap_update_bits(data->regmap, TMDS181_REG_CTRLA,
			   TMDS181_CTRLA_DEV_FUNC_MODE, 0x00);

	if (of_property_read_bool(client->dev.of_node, "ti,adaptive-equalizer"))
		regmap_update_bits(data->regmap, TMDS181_REG_CTRLA,
				   TMDS181_CTRLA_EQ_EN | TMDS181_CTRLA_EQ_ADA_EN,
				   TMDS181_CTRLA_EQ_EN | TMDS181_CTRLA_EQ_ADA_EN);
	if (of_property_read_bool(client->dev.of_node, "ti,disable-equalizer"))
		regmap_update_bits(data->regmap, TMDS181_REG_CTRLA,
				   TMDS181_CTRLA_EQ_EN | TMDS181_CTRLA_EQ_ADA_EN,
				   0);

	switch (chip) {
	case dp159:
		val = 0;
		if (!of_property_read_u32(client->dev.of_node,
					  "ti,slew-rate", &param)) {
			if (param > 3) {
				dev_err(&client->dev, "invalid slew-rate\n");
				return -EINVAL;
			}
			/* Implement 0 = slow, 3 = fast slew rate */
			val = FIELD_PREP(TMDS181_CTRLB_SLEW_CTL, (3 - param));
		}
		if (of_property_read_bool(client->dev.of_node, "ti,dvi-mode"))
			val |= TMDS181_CTRLB_HDMI_SEL_DVI;
		break;
	default:
		val = TMDS181_CTRLB_DDC_DR_SEL;
		break;
	}

	/* Default to low-speed termination */
	val |= FIELD_PREP(TMDS181_CTRLB_TX_TERM_CTL, TMDS181_CTRLB_TX_TERM_150_300_OHMS);

	ret = regmap_write(data->regmap, TMDS181_REG_CTRLB, val);
	if (ret < 0) {
		dev_err(&client->dev, "regmap_write(B) failed\n");
		return ret;
	}

	/* Find next bridge in chain */
	data->next_bridge = devm_drm_of_get_bridge(&client->dev, client->dev.of_node, 1, 0);
	if (IS_ERR(data->next_bridge))
		return dev_err_probe(&client->dev, PTR_ERR(data->next_bridge),
				     "Failed to find next bridge\n");

	/* Register the bridge. */
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
	{ .compatible = "ti,tmds181", .data = (void *)tmds181 },
	{ .compatible = "ti,sn65dp159", .data = (void *)dp159 },
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
