// SPDX-License-Identifier: GPL-2.0-only
/*
 * DRM driver for Solomon SSD13xx OLED displays (I2C bus)
 *
 * Copyright 2022 Red Hat Inc.
 * Author: Javier Martinez Canillas <javierm@redhat.com>
 *
 * Based on drivers/video/fbdev/ssd1307fb.c
 * Copyright 2012 Free Electrons
 */
#include <linux/i2c.h>
#include <linux/module.h>

#include "ssd13xx.h"

#define DRIVER_NAME	"ssd13xx-i2c"
#define DRIVER_DESC	"DRM driver for Solomon SSD13xx OLED displays (I2C)"

static const struct regmap_config ssd13xx_i2c_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
};

static int ssd13xx_i2c_probe(struct i2c_client *client)
{
	struct ssd13xx_device *ssd13xx;
	struct regmap *regmap;

	regmap = devm_regmap_init_i2c(client, &ssd13xx_i2c_regmap_config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	ssd13xx = ssd13xx_probe(&client->dev, regmap);
	if (IS_ERR(ssd13xx))
		return PTR_ERR(ssd13xx);

	i2c_set_clientdata(client, ssd13xx);

	return 0;
}

static void ssd13xx_i2c_remove(struct i2c_client *client)
{
	struct ssd13xx_device *ssd13xx = i2c_get_clientdata(client);

	ssd13xx_remove(ssd13xx);
}

static void ssd13xx_i2c_shutdown(struct i2c_client *client)
{
	struct ssd13xx_device *ssd13xx = i2c_get_clientdata(client);

	ssd13xx_shutdown(ssd13xx);
}

static const struct of_device_id ssd13xx_of_match[] = {
	/* ssd130x family */
	{
		.compatible = "sinowealth,sh1106",
		.data = &ssd13xx_variants[SH1106_ID],
	},
	{
		.compatible = "solomon,ssd1305",
		.data = &ssd13xx_variants[SSD1305_ID],
	},
	{
		.compatible = "solomon,ssd1306",
		.data = &ssd13xx_variants[SSD1306_ID],
	},
	{
		.compatible = "solomon,ssd1307",
		.data = &ssd13xx_variants[SSD1307_ID],
	},
	{
		.compatible = "solomon,ssd1309",
		.data = &ssd13xx_variants[SSD1309_ID],
	},
	/* Deprecated but kept for backward compatibility */
	{
		.compatible = "solomon,ssd1305fb-i2c",
		.data = &ssd13xx_variants[SSD1305_ID],
	},
	{
		.compatible = "solomon,ssd1306fb-i2c",
		.data = &ssd13xx_variants[SSD1306_ID],
	},
	{
		.compatible = "solomon,ssd1307fb-i2c",
		.data = &ssd13xx_variants[SSD1307_ID],
	},
	{
		.compatible = "solomon,ssd1309fb-i2c",
		.data = &ssd13xx_variants[SSD1309_ID],
	},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ssd13xx_of_match);

static struct i2c_driver ssd13xx_i2c_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = ssd13xx_of_match,
	},
	.probe = ssd13xx_i2c_probe,
	.remove = ssd13xx_i2c_remove,
	.shutdown = ssd13xx_i2c_shutdown,
};
module_i2c_driver(ssd13xx_i2c_driver);

MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_AUTHOR("Javier Martinez Canillas <javierm@redhat.com>");
MODULE_LICENSE("GPL v2");
MODULE_IMPORT_NS(DRM_SSD13XX);
