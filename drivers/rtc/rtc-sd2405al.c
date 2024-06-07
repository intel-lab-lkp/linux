// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * RTC driver for the SD2405AL Real-Time Clock
 *
 * Datasheet:
 * https://image.dfrobot.com/image/data/TOY0021/SD2405AL%20datasheet%20(Angelo%20v0.1).pdf
 *
 * Copyright (C) 2024 Tóth János <gomba007@gmail.com>
 * Copyright (C) 2018 Zoro Li <long17.cool@163.com>
 */

#include <linux/bcd.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/rtc.h>

#define SD2405AL_REG_T_SEC	0x00
#define SD2405AL_REG_T_MIN	0x01
#define SD2405AL_REG_T_HOUR	0x02
#define SD2405AL_REG_T_WEEK	0x03
#define SD2405AL_REG_T_DAY	0x04
#define SD2405AL_REG_T_MON	0x05
#define SD2405AL_REG_T_YEAR	0x06

#define SD2405AL_REG_A_SEC	0x07
#define SD2405AL_REG_A_MIN	0x08
#define SD2405AL_REG_A_HOUR	0x09
#define SD2405AL_REG_A_WEEK	0x0A
#define SD2405AL_REG_A_DAY	0x0B
#define SD2405AL_REG_A_MON	0x0C
#define SD2405AL_REG_A_YEAR	0x0D
#define SD2405AL_REG_A_EN	0x0E

#define SD2405AL_REG_CTR1	0x0F
#define SD2405AL_REG_CTR2	0x10
#define SD2405AL_REG_CTR3	0x11
#define SD2405AL_REG_TTF	0x12
#define SD2405AL_REG_CTR	0x13	/* count down register */

#define SD2405AL_REG_M_START	0x14	/* general RAM start */
#define SD2405AL_REG_M_END	0x1F	/* general RAM end */

#define SD2405AL_NUM_T_REGS	(SD2405AL_REG_T_YEAR - SD2405AL_REG_T_SEC + 1)

#define SD2405AL_BIT_24H	0x80
#define SD2405AL_BIT_12H_PM	0x20

struct sd2405al {
	struct rtc_device	*rtc;
	struct regmap		*regmap;
};

static int sd2405al_read_time(struct device *dev, struct rtc_time *time)
{
	u8 hour;
	u8 data[SD2405AL_NUM_T_REGS] = { 0 };
	struct i2c_client *client = to_i2c_client(dev);
	struct sd2405al *sd2405al = i2c_get_clientdata(client);
	int ret;

	ret = regmap_bulk_read(sd2405al->regmap, SD2405AL_REG_T_SEC, data,
			       SD2405AL_NUM_T_REGS);
	if (ret < 0) {
		printk(KBUILD_MODNAME ": reading failed: %d\n", ret);
		return ret;
	}

	time->tm_sec = bcd2bin(data[SD2405AL_REG_T_SEC] & 0x7F);
	time->tm_min = bcd2bin(data[SD2405AL_REG_T_MIN] & 0x7F);

	hour = data[SD2405AL_REG_T_HOUR];
	if (hour & SD2405AL_BIT_24H)
		time->tm_hour = bcd2bin(hour & 0x3F);
	else
		if (hour & SD2405AL_BIT_12H_PM)
			time->tm_hour = bcd2bin(hour & 0x1F) + 12;
		else /* 12 hour mode, AM */
			time->tm_hour = bcd2bin(hour & 0x1F);

	time->tm_mday = bcd2bin(data[SD2405AL_REG_T_DAY] & 0x3F);
	time->tm_wday = data[SD2405AL_REG_T_WEEK] & 0x07;
	time->tm_mon = bcd2bin(data[SD2405AL_REG_T_MON] & 0x1F) - 1;
	time->tm_year = bcd2bin(data[SD2405AL_REG_T_YEAR]) + 100;

	return 0;
}

static int sd2405al_set_time(struct device *dev, struct rtc_time *time)
{
	u8 data[SD2405AL_NUM_T_REGS];
	struct i2c_client *client = to_i2c_client(dev);
	struct sd2405al *sd2405al = i2c_get_clientdata(client);
	int ret;

	data[SD2405AL_REG_T_SEC] = bin2bcd(time->tm_sec);
	data[SD2405AL_REG_T_MIN] = bin2bcd(time->tm_min);
	data[SD2405AL_REG_T_HOUR] = bin2bcd(time->tm_hour) | SD2405AL_BIT_24H;
	data[SD2405AL_REG_T_DAY] = bin2bcd(time->tm_mday);
	data[SD2405AL_REG_T_WEEK] = time->tm_wday & 0x07;
	data[SD2405AL_REG_T_MON] = bin2bcd(time->tm_mon) + 1;
	data[SD2405AL_REG_T_YEAR] = bin2bcd(time->tm_year - 100);

	ret = regmap_bulk_write(sd2405al->regmap, SD2405AL_REG_T_SEC, data,
				SD2405AL_NUM_T_REGS);
	if (ret < 0) {
		printk(KBUILD_MODNAME ": writing failed: %d\n", ret);
		return ret;
	}

	return 0;
}

static const struct rtc_class_ops sd2405al_rtc_ops = {
	.read_time = sd2405al_read_time,
	.set_time = sd2405al_set_time,
};

static const struct regmap_config sd2405al_regmap_conf = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = SD2405AL_REG_M_END,
};

static int sd2405al_probe(struct i2c_client *client)
{
	struct sd2405al *sd2405al;
	int func;
	int ret;

	func = i2c_check_functionality(client->adapter, I2C_FUNC_I2C);
	if (!func) {
		printk(KBUILD_MODNAME ": invalid adapter\n");
		return -ENODEV;
	}

	sd2405al = devm_kzalloc(&client->dev, sizeof(*sd2405al), GFP_KERNEL);
	if (!sd2405al) {
		printk(KBUILD_MODNAME ": unable to allocate memory\n");
		return -ENOMEM;
	}

	sd2405al->regmap = devm_regmap_init_i2c(client, &sd2405al_regmap_conf);
	if (IS_ERR(sd2405al->regmap)) {
		printk(KBUILD_MODNAME ": unable to allocate regmap\n");
		return PTR_ERR(sd2405al->regmap);
	}

	i2c_set_clientdata(client, sd2405al);

	sd2405al->rtc = devm_rtc_allocate_device(&client->dev);
	if (IS_ERR(sd2405al->rtc)) {
		printk(KBUILD_MODNAME ": unable to allocate device\n");
		return PTR_ERR(sd2405al->rtc);
	}

	sd2405al->rtc->ops = &sd2405al_rtc_ops;
	sd2405al->rtc->range_min = RTC_TIMESTAMP_BEGIN_2000;
	sd2405al->rtc->range_max = RTC_TIMESTAMP_END_2099;

	ret = devm_rtc_register_device(sd2405al->rtc);
	if (ret < 0) {
		printk(KBUILD_MODNAME ": unable to register device: %d\n", ret);
		return ret;
	}

	return 0;
}

static const struct i2c_device_id sd2405al_id[] = {
	{ "sd2405al", 0 },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(i2c, sd2405al_id);

static const __maybe_unused struct of_device_id sd2405al_of_match[] = {
	{ .compatible = "dfrobot,sd2405al" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, sd2405al_of_match);

static struct i2c_driver sd2405al_driver = {
	.driver = {
		.name = "sd2405al",
		.of_match_table = of_match_ptr(sd2405al_of_match),
	},
	.probe = sd2405al_probe,
	.id_table = sd2405al_id,
};

module_i2c_driver(sd2405al_driver);

MODULE_AUTHOR("Tóth János");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("SD2405AL RTC driver");
