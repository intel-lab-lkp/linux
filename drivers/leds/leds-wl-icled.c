// SPDX-License-Identifier: GPL-2.0-only
// LED driver for Wurth Elektronik WL-ICLED
// Copyright (C) Systeme Helmholz - https://www.helmholz.de

#include <linux/leds.h>
#include <linux/spi/spi.h>
#include <linux/led-class-multicolor.h>

#define WL_ICLED_LEDS_PER_IC 3
#define WL_ICLED_MAX_BRIGHTNESS 255

#define WL_ICLED_32E_CMD_WRITE 0x7
#define WL_ICLED_32E_CMD_SLEEP 0x5
#define WL_ICLED_32E_CMD_SHIFT 5
#define WL_ICLED_32E_MAX_GAIN 31
#define WL_ICLED_32E_GAIN_MASK 0x1f
#define WL_ICLED_32E_START_FRAME_SIZE 4

#define WL_ICLED_48E_MAX_GAIN 15
#define WL_ICLED_48E_MAX_PWM 4095
#define WL_ICLED_48E_GAIN_SHIFT 12
#define WL_ICLED_48E_PWM_MASK 0xfff

enum wl_icled_type {
	WL_ICLED_SINGLE_WIRE,
	WL_ICLED_TWO_WIRE,
};

struct wl_icled_led {
	struct led_classdev_mc	mc_cdev;
	struct wl_icled	*priv;
	int brightness;
};

struct wl_icled_info {
	int model_id;
	int color_sequence[WL_ICLED_LEDS_PER_IC];
	void (*encode)(u8 *buf, struct wl_icled_led *led);
	enum wl_icled_type type;
	size_t bytes_per_led;
	unsigned int speed_hz;
};

enum wl_icled_model {
	WL1315X246,     /*1315050930246 */
	WL1315X002,     /*1315050930002*/
	WL131X000,      /*1313210530000*/
			/*1312020030000*/
	WL131161X,      /*1311610030140 */
	WL131212X,      /*1312121320437 */
};

struct wl_icled {
	const struct wl_icled_info *icled_info;
	struct device *dev;
	struct mutex lock;
	struct spi_device *spi;
	size_t count;
	u8 *buf;
	struct wl_icled_led leds[];
};

static void wl_icled_encode_24bit(u8 *buf, struct wl_icled_led *led)
{
 /* WE 1315050930002, 1313210530000 and 1312020030000 control sequence:
  * +---------------------------+---+----------------------------+
  * |            LED 1          |...|         LED n              |
  * +---------------------------+--------------------------------+
  * |  GREEN  |  RED   |  BLUE  |...|  GREEN   |  RED   |  BLUE  |
  * +---------+--------+--------+--------------+--------+--------+
  * |    8    |   8    |    8   |...|     8    |   8    |    8   |
  * +---------------------------+---+----------------------------+
  */
	u8 pattern_true, pattern_false;
	int i, j;

	pattern_false = 0xc0;
	pattern_true = (led->priv->icled_info->model_id == WL1315X002) ?
			0xf0 : 0xfc;

	for (i = 0; i < WL_ICLED_LEDS_PER_IC; i++) {
		for (j = 7; j >= 0; j--, buf++)
			*buf = led->mc_cdev.subled_info[i].brightness & BIT(j) ?
			       pattern_true : pattern_false;
	}
}

static void wl_icled_encode_48bit(u8 *buf, struct wl_icled_led *led)
{
 /* WE-1312121320437 control sequence:
  * +--------------------------+---+--------------------------+
  * |            LED 1         |...|            LED n         |
  * +--------------------------+---+--------------------------+
  * |  RED   |  GREEN |  BLUE  |...|  RED   |  GREEN |  BLUE  |
  * +--------+--------+--------+---+--------+--------+--------+
  * |GAIN|PWM|GAIN|PWM|GAIN|PWM|...|GAIN|PWM|GAIN|PWM|GAIN|PWM|
  * +----+---+----+---+----+---+---+----+---+----+---+----+---|
  * | 4  |12 | 4  |12 | 4  |12 |...| 4  |12 | 4  |12 | 4  |12 |
  * +---------------------------------------------------------+
  */
	u8 pattern_true, pattern_false;
	int i, j;

	pattern_false = 0xc0;
	pattern_true = 0xfc;

	for (i = 0; i < WL_ICLED_LEDS_PER_IC; i++) {
		unsigned int brightness;
		u16 led_sequence;

		brightness = led->mc_cdev.subled_info[i].brightness;

		led_sequence = ((brightness * WL_ICLED_48E_MAX_PWM)/
				WL_ICLED_MAX_BRIGHTNESS) &
				WL_ICLED_48E_PWM_MASK;
		led_sequence |= ((brightness * WL_ICLED_48E_MAX_GAIN)/
				WL_ICLED_MAX_BRIGHTNESS) <<
				WL_ICLED_48E_GAIN_SHIFT;

		for (j = 15; j >= 0; j--, buf++)
			*buf = led_sequence & BIT(j) ?
			       pattern_true : pattern_false;
	}

}

static void wl_icled_encode_32bit(u8 *buf, struct wl_icled_led *led)
{
 /* WE 1315050930246 and 1311610030140 control sequence:
  * |--------------+------+------+-----------------------+-------------+
  * |  Start Frame | Flag | Gain |      PWM  level       |  End Frame  |
  * +--------------+------+------+-----------------------+-------------+
  * |              |             |LED1 | LED2| LED3| LEDn|             |
  * |              +-------------+-----+-----+-----+-----+             |
  * |              | 3bit | 5bit |B|G|R|B|G|R|B|G|R|B|G|R|             |
  * +--------------+-------------+-----------------------+-------------+
  * |   3 bytes    |    1 byte   |        n * 3 bytes    |(n+7)/8bytes |
  * +--------------+-------------+-----------------------+-------------+
  */
	int i;

	*buf = (WL_ICLED_32E_CMD_WRITE << WL_ICLED_32E_CMD_SHIFT) |
	       (((led->brightness * WL_ICLED_32E_MAX_GAIN)/
		WL_ICLED_MAX_BRIGHTNESS) & WL_ICLED_32E_GAIN_MASK);
	buf++;

	for (i = 0; i < WL_ICLED_LEDS_PER_IC; i++, buf++)
		*buf = led->mc_cdev.subled_info[i].brightness;
}

static const struct wl_icled_info wl_icled_info_tbl[] = {

	[WL1315X246] = {/* 1315050930246 */
		.model_id = WL1315X246,
		.color_sequence = { LED_COLOR_ID_BLUE,
				    LED_COLOR_ID_GREEN,
				    LED_COLOR_ID_RED },
		.bytes_per_led = 4,
		.encode = wl_icled_encode_32bit,
		.type = WL_ICLED_TWO_WIRE,
	},
	[WL1315X002] = { /*1315050930002*/
		.model_id = WL1315X002,
		.color_sequence = { LED_COLOR_ID_GREEN,
				    LED_COLOR_ID_RED,
				    LED_COLOR_ID_BLUE },
		.bytes_per_led = 24,
		.encode = wl_icled_encode_24bit,
		.speed_hz = 5333333,
		.type = WL_ICLED_SINGLE_WIRE,
	},
	[WL131X000] = { /*1313210530000*/ /*1312020030000*/
		.model_id = WL131X000,
		.color_sequence = { LED_COLOR_ID_GREEN,
				    LED_COLOR_ID_RED,
				    LED_COLOR_ID_BLUE },
		.bytes_per_led = 24,
		.encode = wl_icled_encode_24bit,
		.speed_hz = 6666666,
		.type = WL_ICLED_SINGLE_WIRE,
	},
	[WL131161X] = { /*1311610030140 */
		.model_id = WL131161X,
		.color_sequence = { LED_COLOR_ID_GREEN,
				    LED_COLOR_ID_BLUE,
				    LED_COLOR_ID_RED },
		.bytes_per_led = 4,
		.encode = wl_icled_encode_32bit,
		.type = WL_ICLED_TWO_WIRE,
	},
	[WL131212X] = { /*1312121320437 */
		.model_id = WL131212X,
		.color_sequence = { LED_COLOR_ID_RED,
				    LED_COLOR_ID_GREEN,
				    LED_COLOR_ID_BLUE },
		.bytes_per_led = 48,
		.encode = wl_icled_encode_48bit,
		.speed_hz = 6666666,
		.type = WL_ICLED_SINGLE_WIRE,
	},
};

static int wl_icled_sync(struct wl_icled *priv)
{
	size_t i, len;
	int ret;
	u8 *buf;

	buf = priv->buf;
	len = priv->icled_info->bytes_per_led * priv->count;

	if (priv->icled_info->type == WL_ICLED_TWO_WIRE) {
		/* Prepare start frame: 4 bytes of 0 */
		for (i = 0; i < WL_ICLED_32E_START_FRAME_SIZE; i++, buf++)
			*buf = 0;

		/* Prepare end frame: N/2 bits (rounded to first byte) of 1s */
		buf += priv->icled_info->bytes_per_led * priv->count;
		for (i = 0; i < (priv->count + 15)/16; i++, buf++)
			*buf = 0xff;

		len = buf - priv->buf;
		buf = priv->buf + WL_ICLED_32E_START_FRAME_SIZE;
	}

	for (i = 0; i < priv->count; i++)
		priv->icled_info->encode(buf +
					(priv->icled_info->bytes_per_led * i),
					 &priv->leds[i]);

	if (priv->icled_info->type == WL_ICLED_SINGLE_WIRE) {
		struct spi_transfer t = {
			.tx_buf	= priv->buf,
			.len = len,
			.speed_hz = priv->icled_info->speed_hz,
			.bits_per_word = SPI_NBITS_OCTAL,
		};
		struct spi_message m;

		spi_message_init(&m);
		spi_message_add_tail(&t, &m);

		ret = spi_sync(priv->spi, &m);
	} else {
		ret = spi_write(priv->spi, priv->buf, len);
	}

	return ret;
}

static int wl_icled_set_brightness(struct led_classdev *cdev,
				   enum led_brightness brightness)
{
	struct led_classdev_mc *mc_dev = lcdev_to_mccdev(cdev);
	struct wl_icled_led *led = container_of(mc_dev,
						struct wl_icled_led,
						mc_cdev);
	int ret;

	mutex_lock(&led->priv->lock);
	led->brightness = brightness;
	led_mc_calc_color_components(&led->mc_cdev, brightness);
	ret = wl_icled_sync(led->priv);
	mutex_unlock(&led->priv->lock);

	return ret;
}

static int wl_icled_probe_dt(struct wl_icled *priv, size_t count)
{
	struct led_init_data init_data = {};
	struct mc_subled *mc_led_info;
	struct fwnode_handle *child;
	struct wl_icled_led *led;
	int ret, i;
	u32 reg;

	device_for_each_child_node(priv->dev, child) {

		ret = fwnode_property_read_u32(child, "reg", &reg);
		if (ret < 0 || reg >= count) {
			dev_err(priv->dev, "reg property is invalid\n");
			fwnode_handle_put(child);

			return ret ? -EINVAL : ret;
		}

		led = &priv->leds[reg];
		led->priv = priv;
		led->mc_cdev.num_colors = WL_ICLED_LEDS_PER_IC;
		led->mc_cdev.led_cdev.brightness_set_blocking =
							wl_icled_set_brightness;

		mc_led_info = devm_kcalloc(priv->dev,
					   led->mc_cdev.num_colors,
					   sizeof(*mc_led_info), GFP_KERNEL);
		if (!mc_led_info) {
			fwnode_handle_put(child);
			return -ENOMEM;
		}

		for (i = 0; i < WL_ICLED_LEDS_PER_IC; i++)
			mc_led_info[i].color_index =
				priv->icled_info->color_sequence[i];

		led->mc_cdev.subled_info = mc_led_info;
		init_data.fwnode = child;
		ret = devm_led_classdev_multicolor_register_ext(priv->dev,
								&led->mc_cdev,
								&init_data);
		if (ret) {
			dev_err(priv->dev,
				"failed to register LED device, err %d", ret);
			fwnode_handle_put(child);
			return ret;
		}
	}

	for (i = 0; i < count; i++) {
		/* make sure all leds got assigned */
		if (priv->leds[i].priv != priv) {
			dev_err(priv->dev, "reg property is invalid\n");
			return -EINVAL;
		}
	}

	return 0;
}

static int wl_icled_probe(struct spi_device *spi)
{
	struct wl_icled	*dev;
	size_t count, buf_len;
	int ret;

	count = device_get_child_node_count(&spi->dev);
	if (!count) {
		dev_err(&spi->dev, "LEDs are not defined in device tree!");
		return -ENODEV;
	}

	dev = devm_kzalloc(&spi->dev, struct_size(dev, leds, count),
			    GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	mutex_init(&dev->lock);
	dev->count	= count;
	dev->dev	= &spi->dev;
	dev->spi	= spi;

	dev->icled_info = device_get_match_data(&spi->dev);

	buf_len = count * dev->icled_info->bytes_per_led;

	if (dev->icled_info->model_id == WL131161X ||
	    dev->icled_info->model_id == WL1315X246) {
		/* account for START and END frames */
		buf_len += WL_ICLED_32E_START_FRAME_SIZE;
		buf_len += (count + 15)/16;
	}

	dev->buf = devm_kzalloc(&spi->dev, buf_len, GFP_KERNEL);
	if (!dev->buf)
		return -ENOMEM;

	ret = wl_icled_probe_dt(dev, count);
	if (ret)
		return ret;

	spi_set_drvdata(spi, dev);

	return 0;
}

static void wl_icled_remove(struct spi_device *spi)
{
	struct wl_icled *priv = spi_get_drvdata(spi);

	mutex_destroy(&priv->lock);
}

static const struct of_device_id wl_icled_dt_ids[] = {
	{ .compatible = "we,1315x246", .data = &wl_icled_info_tbl[WL1315X246]},
	{ .compatible = "we,1315x002", .data = &wl_icled_info_tbl[WL1315X002]},
	{ .compatible = "we,131x000", .data = &wl_icled_info_tbl[WL131X000]},
	{ .compatible = "we,131161x", .data = &wl_icled_info_tbl[WL131161X]},
	{ .compatible = "we,131212x", .data = &wl_icled_info_tbl[WL131212X]},
	{},
};
MODULE_DEVICE_TABLE(of, wl_icled_dt_ids);

static struct spi_driver wl_icled_driver = {
	.probe		= wl_icled_probe,
	.remove		= wl_icled_remove,
	.driver = {
		.name		= KBUILD_MODNAME,
		.of_match_table	= wl_icled_dt_ids,
	},
};

module_spi_driver(wl_icled_driver);

MODULE_DESCRIPTION("LED driver for Wurth Elektronik WL-ICLEDs");
MODULE_AUTHOR("Ante Knezic <knezic@helmholz.com>");
MODULE_LICENSE("GPL");
