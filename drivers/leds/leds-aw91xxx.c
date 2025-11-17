// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * leds-aw91xxx.c   aw91xxx led module
 *
 * Copyright (c) 2021 AWINIC Technology CO., LTD
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/workqueue.h>
#include <linux/errno.h>
#include <linux/pm.h>
#include <linux/platform_device.h>
#include <linux/input.h>
#include <linux/i2c.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/of_gpio.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/time.h>
#include <linux/delay.h>
#include <linux/of_gpio.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/leds.h>
#include <linux/pinctrl/consumer.h>
#include <linux/regulator/consumer.h>
#include <linux/dma-mapping.h>
#include <linux/hrtimer.h>
#include <linux/kernel.h>
#include <linux/firmware.h>
#include <linux/version.h>
#include <linux/debugfs.h>
#include <linux/leds.h>
#include <linux/fb.h>
//#include <stddef.h>
#include "leds-aw91xxx.h"

/******************************************************
 *
 * Marco
 *
 ******************************************************/
#define AW91XXX_I2C_NAME "aw91xxx_led"
#define HRTIMER_FRAME	20

#define AW91XXX_DRIVER_VERSION "v1.0.0"

#define AW_I2C_RETRIES 5
#define AW_I2C_RETRY_DELAY 5
#define AW_READ_CHIPID_RETRIES 5
#define AW_READ_CHIPID_RETRY_DELAY 5

#define REG_INPUT_P0        0x00
#define REG_INPUT_P1        0x01
#define REG_OUTPUT_P0       0x02
#define REG_OUTPUT_P1       0x03
#define REG_CONFIG_P0       0x04
#define REG_CONFIG_P1       0x05
#define REG_INT_P0          0x06
#define REG_INT_P1          0x07
#define REG_ID              0x10
#define REG_CTRL            0x11
#define REG_WORK_MODE_P0    0x12
#define REG_WORK_MODE_P1    0x13
#define REG_EN_BREATH       0x14
#define REG_FADE_TIME       0x15
#define REG_FULL_TIME       0x16
#define REG_DLY0_BREATH     0x17
#define REG_DLY1_BREATH     0x18
#define REG_DLY2_BREATH     0x19
#define REG_DLY3_BREATH     0x1a
#define REG_DLY4_BREATH     0x1b
#define REG_DLY5_BREATH     0x1c
#define REG_DIM00           0x20
#define REG_DIM01           0x21
#define REG_DIM02           0x22
#define REG_DIM03           0x23
#define REG_DIM04           0x24
#define REG_DIM05           0x25
#define REG_DIM06           0x26
#define REG_DIM07           0x27
#define REG_DIM08           0x28
#define REG_DIM09           0x29
#define REG_SWRST           0x7F
#define REG_81H             0x81

/* aw91xxx register read/write access */
#define REG_NONE_ACCESS                 0
#define REG_RD_ACCESS                   (1 << 0)
#define REG_WR_ACCESS                   (1 << 1)
#define AW91XXX_REG_MAX                  0xFF
#define AW91XXX_DIM_MAX                  0x0A
#define AW91XXX_PORT_NUM (0x10) /* 16 */
#define AW91XXX_WORK_LED_NUM 10

const unsigned char aw91xxx_reg_access[AW91XXX_REG_MAX] = {
	[REG_INPUT_P0]		= REG_RD_ACCESS,
	[REG_INPUT_P1]		= REG_RD_ACCESS,
	[REG_OUTPUT_P0]		= REG_RD_ACCESS | REG_WR_ACCESS,
	[REG_OUTPUT_P1]		= REG_RD_ACCESS | REG_WR_ACCESS,
	[REG_CONFIG_P0]		= REG_RD_ACCESS | REG_WR_ACCESS,
	[REG_CONFIG_P1]		= REG_RD_ACCESS | REG_WR_ACCESS,
	[REG_INT_P0]		= REG_RD_ACCESS | REG_WR_ACCESS,
	[REG_INT_P1]		= REG_RD_ACCESS | REG_WR_ACCESS,
	[REG_ID]		= REG_RD_ACCESS,
	[REG_CTRL]		= REG_RD_ACCESS | REG_WR_ACCESS,
	[REG_WORK_MODE_P0]	= REG_RD_ACCESS | REG_WR_ACCESS,
	[REG_WORK_MODE_P1]	= REG_RD_ACCESS | REG_WR_ACCESS,
	[REG_EN_BREATH]		= REG_RD_ACCESS | REG_WR_ACCESS,
	[REG_FADE_TIME]		= REG_RD_ACCESS | REG_WR_ACCESS,
	[REG_FULL_TIME]		= REG_RD_ACCESS | REG_WR_ACCESS,
	[REG_DLY0_BREATH]	= REG_RD_ACCESS | REG_WR_ACCESS,
	[REG_DLY1_BREATH]	= REG_RD_ACCESS | REG_WR_ACCESS,
	[REG_DLY2_BREATH]	= REG_RD_ACCESS | REG_WR_ACCESS,
	[REG_DLY3_BREATH]	= REG_RD_ACCESS | REG_WR_ACCESS,
	[REG_DLY4_BREATH]	= REG_RD_ACCESS | REG_WR_ACCESS,
	[REG_DLY5_BREATH]	= REG_RD_ACCESS | REG_WR_ACCESS,
	[REG_DIM00]		= REG_WR_ACCESS,
	[REG_DIM01]		= REG_WR_ACCESS,
	[REG_DIM02]		= REG_WR_ACCESS,
	[REG_DIM03]		= REG_WR_ACCESS,
	[REG_DIM04]		= REG_WR_ACCESS,
	[REG_DIM05]		= REG_WR_ACCESS,
	[REG_DIM06]		= REG_WR_ACCESS,
	[REG_DIM07]		= REG_WR_ACCESS,
	[REG_DIM08]		= REG_WR_ACCESS,
	[REG_DIM09]		= REG_WR_ACCESS,
	[REG_SWRST]		= REG_WR_ACCESS,
};

static KEY_STATE key_map[] = {
/*	name		code				val */
	{"DEL",		KEY_BACK,			0},
	{"BACK",	KEY_BACK,			0},
	{"HOME",	KEY_HOME,			0},
	{"HOME",	KEY_HOME,			0},
	{"BACK",	KEY_BACK,			0},
	{"Q",		KEY_Q,				0},
	{"F1",		KEY_F1,				0},
	{"F2",		KEY_F2,				0},

	{"W",		KEY_W,				0},
	{"E",		KEY_E,				0},
	{"R",		KEY_R,				0},
	{"T",		KEY_T,				0},
	{"Y",		KEY_Y,				0},
	{"U",		KEY_U,				0},
	{"F3",		KEY_F3,				0},
	{"F4",		KEY_F4,				0},

	{"I",		KEY_I,				0},
	{"O",		KEY_O,				0},
	{"P",		KEY_P,				0},
	{"J",		KEY_J,				0},
	{"K",		KEY_K,				0},
	{"L",		KEY_L,				0},
	{"F5",		KEY_F5,				0},
	{"F6",		KEY_F6,				0},

	{"A",		KEY_A,				0},
	{"S",		KEY_S,				0},
	{"D",		KEY_D,				0},
	{"F",		KEY_F,				0},
	{"G",		KEY_G,				0},
	{"H",		KEY_H,				0},
	{"F7",		KEY_F7,				0},
	{"F8",		KEY_F8,				0},

	{"Caps Lock",	KEY_CAPSLOCK,	0},
	{"Z",		KEY_Z,				0},
	{"X",		KEY_X,				0},
	{"C",		KEY_C,				0},
	{"V",		KEY_V,				0},
	{"B",		KEY_B,				0},
	{"F9",		KEY_F9,				0},
	{"F10",		KEY_F10,			0},

	{"N",		KEY_N,				0},
	{"M",		KEY_M,				0},
	{"Backspace",	KEY_BACKSPACE,	0},
	{"Down",	KEY_DOWN,			0},
	{"Up",		KEY_UP,				0},
	{"ENTER",	KEY_ENTER,			0},
	{"F11",		KEY_F11,			0},
	{"F12",		KEY_F12,			0},

	{"ALT",		KEY_ALTERASE,			0},
	{".",		KEY_DOT,			0},
	{"Left",	KEY_LEFT,			0},
	{"Right",	KEY_RIGHT,			0},
	{"Space",	KEY_SPACE,			0},
	{"Volup",	KEY_VOLUMEUP,			0},
	{"KEY_STOP",	KEY_STOP,			0},
	{"KEY_AGAIN",	KEY_AGAIN,			0},

	{"PROPS",	KEY_PROPS,			0},
	{"UNDO",	KEY_UNDO,			0},
	{"FRONT",	KEY_FRONT,			0},
	{"COPY",	KEY_COPY,			0},
	{"OPEN",	KEY_OPEN,			0},
	{"PASTE",	KEY_PASTE,			0},
	{"FIND",	KEY_FIND,			0},
	{"CUT",		KEY_CUT,			0},

};

/******************************************************
 *
 * aw91xxx i2c write/read
 *
 ******************************************************/
static int aw91xxx_i2c_write(struct aw91xxx *aw91xxx,
		unsigned char reg_addr, unsigned char reg_data)
{
	int ret = -1;
	unsigned char cnt = 0;

	while (cnt < AW_I2C_RETRIES) {
		ret = i2c_smbus_write_byte_data(aw91xxx->i2c, reg_addr, reg_data);
		if (ret < 0)
			pr_err("%s: i2c_write cnt=%d error=%d\n", __func__, cnt, ret);
		else
			break;

		cnt++;
		usleep_range(1000, 2000);
	}

	return ret;
}

static int aw91xxx_i2c_read(struct aw91xxx *aw91xxx,
		unsigned char reg_addr, unsigned char *reg_data)
{
	int ret = -1;
	unsigned char cnt = 0;

	while (cnt < AW_I2C_RETRIES) {
		ret = i2c_smbus_read_byte_data(aw91xxx->i2c, reg_addr);
		if (ret < 0) {
			pr_err("%s: i2c_read cnt=%d error=%d\n", __func__, cnt, ret);
		} else {
			*reg_data = ret;
			break;
		}
		cnt++;
		usleep_range(1000, 2000);
	}

	return ret;
}

static int aw91xxx_i2c_write_bits(struct aw91xxx *aw91xxx,
		unsigned char reg_addr, unsigned char *buf, unsigned int len)
{
	int ret = -1;
	unsigned char *data;
	unsigned char cnt = 0;

	data = kmalloc(len+1, GFP_KERNEL);
	if (data == NULL)
		return  -ENOMEM;

	data[0] = reg_addr;
	memcpy(&data[1], buf, len);

	while (cnt < AW_I2C_RETRIES) {
		ret = i2c_master_send(aw91xxx->i2c, data, len + 1);
		if (ret < 0)
			pr_err("%s: i2c master send [0x%x] error\n", __func__, aw91xxx->i2c->addr);
		else
			break;
		cnt++;
		usleep_range(1000, 2000);
	}

	kfree(data);

	return ret;
}

static int aw91xxx_i2c_read_bits(struct aw91xxx *aw91xxx,
		unsigned char reg_addr, unsigned char *buf, unsigned int len)
{
	int ret = 0;
	unsigned char *rdbuf = NULL;
	unsigned char cnt = 0;

	struct i2c_msg msgs[] = {
		{
			.addr	= aw91xxx->i2c->addr,
			.flags	= 0,
			.len	= 1,
			.buf	= &reg_addr,
		},
		{
			.addr	= aw91xxx->i2c->addr,
			.flags	= I2C_M_RD,
			.len	= len,
		},
	};

	rdbuf = kmalloc(len, GFP_KERNEL);
	if (rdbuf == NULL)
		return  -ENOMEM;

	msgs[1].buf = rdbuf;

	while (cnt < AW_I2C_RETRIES) {
		ret = i2c_transfer(aw91xxx->i2c->adapter, msgs, ARRAY_SIZE(msgs));
		if (ret < 0)
			pr_err("msg %s i2c read error: %d\n", __func__, ret);
		else
			break;
		cnt++;
		usleep_range(1000, 2000);
	}

	if (buf != NULL)
		memcpy(buf, rdbuf, len);

	kfree(rdbuf);

	return ret;
}


/******************************************************
 *
 * aw91xxx led
 *
 ******************************************************/
static void aw91xxx_brightness_work(struct work_struct *work)
{
	struct aw91xxx *aw91xxx = container_of(work, struct aw91xxx,
		brightness_work);

	unsigned char i;

	if (aw91xxx->cdev.brightness > aw91xxx->cdev.max_brightness)
		aw91xxx->cdev.brightness = aw91xxx->cdev.max_brightness;

	aw91xxx_i2c_write(aw91xxx, REG_EN_BREATH, 0x00);

	aw91xxx_i2c_write(aw91xxx, REG_CTRL, 0x03);

	for (i = 0; i < AW91XXX_WORK_LED_NUM; i++)
		aw91xxx_i2c_write(aw91xxx, REG_DIM00 + i, aw91xxx->cdev.brightness);
}

static void aw91xxx_set_brightness(struct led_classdev *cdev, enum led_brightness brightness)
{
	struct aw91xxx *aw91xxx = container_of(cdev, struct aw91xxx, cdev);

	aw91xxx->cdev.brightness = brightness;

	schedule_work(&aw91xxx->brightness_work);
}

static void aw91xxc_led_blink(struct work_struct *work)
{
	struct aw91xxx *aw91xxx = container_of(work, struct aw91xxx, int_work.work);

	aw91xxx_i2c_write(aw91xxx, REG_CONFIG_P0, ((aw91xxx->blink >> 4) & 0x03));
	aw91xxx_i2c_write(aw91xxx, REG_CONFIG_P1, (aw91xxx->blink & 0x0f));

	aw91xxx_i2c_write(aw91xxx, REG_FADE_TIME, (aw91xxx->fall_time<<3) | (aw91xxx->rise_time));
	aw91xxx_i2c_write(aw91xxx, REG_FULL_TIME, (aw91xxx->off_time<<3) | (aw91xxx->on_time));


	aw91xxx_i2c_write(aw91xxx, REG_CTRL, 0x80 | aw91xxx->imax);
}

static void aw91xxx_led_blink(struct aw91xxx *aw91xxx, unsigned char blink, unsigned char delay)
{
	unsigned char i;

	aw91xxx->blink = blink;

	if (aw91xxx->cdev.brightness > aw91xxx->cdev.max_brightness)
		aw91xxx->cdev.brightness = aw91xxx->cdev.max_brightness;
	aw91xxx_i2c_write(aw91xxx, REG_WORK_MODE_P0, 0x00);
	aw91xxx_i2c_write(aw91xxx, REG_WORK_MODE_P1, 0x00);
	aw91xxx_i2c_write(aw91xxx, REG_EN_BREATH, blink);


	if (blink) {
		if (aw91xxx->vendor_id == 0xB) {
			aw91xxx_i2c_write(aw91xxx, REG_CONFIG_P0, 0x03);
			aw91xxx_i2c_write(aw91xxx, REG_CONFIG_P1, 0x0f);

			aw91xxx_i2c_write(aw91xxx, REG_FADE_TIME,
				(aw91xxx->fall_time<<3) | (aw91xxx->rise_time));
			aw91xxx_i2c_write(aw91xxx, REG_FULL_TIME,
				(aw91xxx->off_time<<3) | (aw91xxx->on_time));

			for (i = 0; i < 6; i++) {
				aw91xxx_i2c_write(aw91xxx, REG_DIM00 + i, aw91xxx->cdev.brightness);
				aw91xxx_i2c_write(aw91xxx, REG_DLY0_BREATH + i, delay);
			}

			aw91xxx_i2c_write(aw91xxx, REG_CTRL, 0x80 | aw91xxx->imax);
		} else {
			schedule_delayed_work(&aw91xxx->int_work, msecs_to_jiffies(delay * 315));
		}
	} else {
		aw91xxx_i2c_write(aw91xxx, REG_EN_BREATH, 0x00);

		aw91xxx_i2c_write(aw91xxx, REG_CTRL, 0x03);

		for (i = 0; i < 10; i++) {
			aw91xxx_i2c_write(aw91xxx, REG_DIM00 + i, 0x00);
			aw91xxx_i2c_write(aw91xxx, REG_DLY0_BREATH + i, 0);
		}
	}
}

/*****************************************************
 *
 * device tree
 *
 *****************************************************/
static int aw91xxx_parse_dt(struct device *dev, struct aw91xxx *aw91xxx, struct device_node *np)
{
	int ret = 0;
	unsigned int val = 0;

	aw91xxx->reset_gpio = of_get_named_gpio(np, "reset-gpio", 0);

	aw91xxx->single_key_enable = false;
	ret = of_property_read_u32(np, "aw91xxx,single_key_enable", &val);
	if (ret) {
		dev_err(aw91xxx->dev,
			"%s: no aw91xxx,single_key_enable provided,The default function is not key\n",
			__func__);
	} else {
		AW_DEBUG("%s:aw91xxx,single_key_enable provided ok, val = %d\n", __func__, val);
		if (val == 1)
			aw91xxx->single_key_enable = true;
	}

	aw91xxx->matrix_key_enable = false;
	ret = of_property_read_u32(np, "aw91xxx,matrix_key_enable", &val);
	if (ret) {
		dev_err(aw91xxx->dev, "%s: no aw91xxx,key_enable provided, The default function is not key\n",
			__func__);
	} else {
		AW_DEBUG("%s:aw91xxx,key_enable provided ok, val = %d\n", __func__, val);
		if (val == 1)
			aw91xxx->matrix_key_enable = true;
	}

	aw91xxx->led_feature_enable = false;
	ret = of_property_read_u32(np, "aw91xxx,led_enable", &val);
	if (ret < 0) {
		dev_err(aw91xxx->dev, "%s: no aw91xxx,led_enable provided, The default function is not LED\n",
			__func__);
	} else {
		AW_DEBUG("%s:aw91xxx,led_enable provided ok, val = %d\n", __func__, val);
		if (val == 1)
			aw91xxx->led_feature_enable = true;
	}

	aw91xxx->gpio_feature_enable = false;
	ret = of_property_read_u32(np, "aw91xxx,gpio_enable", &val);
	if (ret < 0) {
		dev_err(aw91xxx->dev, "%s: aw91xxx,gpio_enable provided, The default function is not GPIO\n",
			__func__);
	} else {
		AW_DEBUG("%s:aw91xxx,gpio_enable provided ok, val = %d\n", __func__, val);
		if (val == 1)
			aw91xxx->gpio_feature_enable = true;
	}

	return 0;
}

static int aw91xxx_hw_reset(struct aw91xxx *aw91xxx)
{
	if (aw91xxx && gpio_is_valid(aw91xxx->reset_gpio)) {
		gpio_set_value_cansleep(aw91xxx->reset_gpio, 0);
		usleep_range(1000, 2000);
		gpio_set_value_cansleep(aw91xxx->reset_gpio, 1);
		usleep_range(6000, 7000);
	} else {
		pr_err("%s: failed\n", __func__);
	}

	return 0;
}

static int aw91xxx_hw_off(struct aw91xxx *aw91xxx)
{
	if (aw91xxx && gpio_is_valid(aw91xxx->reset_gpio)) {
		gpio_set_value_cansleep(aw91xxx->reset_gpio, 0);
		usleep_range(1000, 2000);
	} else {
		pr_err("%s: failed\n", __func__);
	}

	return 0;
}


/*****************************************************
 *
 * check chip id
 *
 *****************************************************/
static int aw91xxx_read_chipid(struct aw91xxx *aw91xxx)
{
	int ret = -1;
	unsigned char cnt = 0;
	unsigned char reg_val = 0;

	while (cnt < AW_READ_CHIPID_RETRIES) {
		ret = aw91xxx_i2c_read(aw91xxx, REG_ID, &reg_val);
		if (ret < 0) {
			dev_err(aw91xxx->dev,
				"%s: failed to read register AW91XXX_REG_ID: %d\n", __func__, ret);
		}
		switch (reg_val) {
		case AW91XXX_ID:
			pr_err("%s aw91xxx detected,chip_id=0x%02x\n", __func__, reg_val);
			aw91xxx->chipid = AW91XXX_ID;
			return 0;
		default:
			pr_err("%s unsupported device revision (0x%x)\n", __func__, reg_val);
			break;
		}
		cnt++;

		msleep(AW_READ_CHIPID_RETRY_DELAY);
	}

	return -EINVAL;
}

static void aw91xxx_identify_vendorid(struct aw91xxx *aw91xxx)
{
	unsigned char reg_val = 0;

	aw91xxx_i2c_read(aw91xxx, REG_81H, &reg_val);
	if ((reg_val != 0x01) && (reg_val != 0x03)) {
		pr_err("devices is 91xxB\n");
		aw91xxx->vendor_id = 0xB;
	} else {
		pr_err("devices is 91xxC\n");
		aw91xxx->vendor_id = 0xC;
	}
}


/******************************************************
 *
 * sys group attribute: reg
 *
 ******************************************************/
static ssize_t
reg_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct aw91xxx *aw91xxx = container_of(led_cdev, struct aw91xxx, cdev);

	unsigned int databuf[2] = {0, 0};

	if (sscanf(buf, "%x %x", &databuf[0], &databuf[1]) == 2)
		aw91xxx_i2c_write(aw91xxx, (unsigned char)databuf[0], (unsigned char)databuf[1]);

	return count;
}

static ssize_t reg_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct aw91xxx *aw91xxx = container_of(led_cdev, struct aw91xxx, cdev);
	ssize_t len = 0;
	unsigned char i = 0;
	unsigned char reg_val = 0;

	for (i = 0; i < AW91XXX_REG_MAX; i++) {
		if (!(aw91xxx_reg_access[i] & REG_RD_ACCESS))
			continue;
		aw91xxx_i2c_read(aw91xxx, i, &reg_val);
		len += snprintf(buf + len, PAGE_SIZE - len, "reg:0x%02x=0x%02x\n", i, reg_val);
	}

	reg_val = 0x12;
	aw91xxx_i2c_read(aw91xxx, REG_81H, &reg_val);
	pr_err("REG_81H = 0x%x\n", reg_val);

	return len;
}

/* 081H = 01->aw9106c,081H = 03->aw9110c */
static void aw91xxx_led_chip_init(struct aw91xxx *aw91xxx)
{
	aw91xxx_i2c_write(aw91xxx, REG_WORK_MODE_P0, 0x00);
	aw91xxx_i2c_write(aw91xxx, REG_WORK_MODE_P1, 0x00);
}

static ssize_t
hwen_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct aw91xxx *aw91xxx = container_of(led_cdev, struct aw91xxx, cdev);
	unsigned int databuf[1] = { 0 };
	int ret = -1;

	ret = kstrtou32(buf, 0, &databuf[0]);
	if (ret < 0) {
		dev_err(aw91xxx->dev, "%s: input data invalid!", __func__);
		return ret;
	}

	if (databuf[0] == 1)
		aw91xxx_hw_reset(aw91xxx);
	else
		aw91xxx_hw_off(aw91xxx);


	return count;
}

static ssize_t hwen_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct aw91xxx *aw91xxx = container_of(led_cdev, struct aw91xxx, cdev);
	ssize_t len = 0;

	len +=
	snprintf(buf + len, PAGE_SIZE - len, "hwen=%d\n", gpio_get_value(aw91xxx->reset_gpio));

	return len;
}

static ssize_t
blink_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
	unsigned int databuf[2];
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct aw91xxx *aw91xxx = container_of(led_cdev, struct aw91xxx, cdev);

	if (sscanf(buf, "%x %x", &databuf[0]) == 1)
		aw91xxx_led_blink(aw91xxx, databuf[0], 3);

	return len;
}

static ssize_t
blink_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	ssize_t len = 0;

	len += snprintf(buf + len, PAGE_SIZE - len, "aw91xx_blink()\n");
	len += snprintf(buf + len, PAGE_SIZE - len, "echo 0x9 > blink\n");
	len += snprintf(buf + len, PAGE_SIZE - len, "echo 0x12> blink\n");
	len += snprintf(buf + len, PAGE_SIZE - len, "echo 0x24> blink\n");

	return len;
}

static ssize_t
dim_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
	unsigned int databuf[2];

	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct aw91xxx *aw91xxx = container_of(led_cdev, struct aw91xxx, cdev);

	if (sscanf(buf, "%x %x", &databuf[0], &databuf[1]) == 2) {
		aw91xxx_i2c_write(aw91xxx, REG_EN_BREATH, 0x0);
		aw91xxx_i2c_write(aw91xxx, REG_DIM00 + databuf[0], databuf[1]);
	}

	return len;
}

static ssize_t
all_dim_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
	unsigned int databuf[1] = { 0 };
	int ret = -1;
	int led_num = 0;
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct aw91xxx *aw91xxx = container_of(led_cdev, struct aw91xxx, cdev);

	ret = kstrtou32(buf, 0, &databuf[0]);
	if (ret < 0) {
		dev_err(aw91xxx->dev, "%s: input data invalid!", __func__);
		return ret;
	}

	aw91xxx_i2c_write(aw91xxx, REG_EN_BREATH, 0x0);
	for (led_num = 0; led_num < AW91XXX_DIM_MAX; led_num++)
		aw91xxx_i2c_write(aw91xxx, REG_DIM00 + led_num, databuf[0]);

	return len;
}

static ssize_t
fade_mode_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
	unsigned int databuf[1] = { 0 };
	int ret = -1;
	unsigned char reg_val = 0;
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct aw91xxx *aw91xxx = container_of(led_cdev, struct aw91xxx, cdev);

	ret = kstrtou32(buf, 0, &databuf[0]);
	if (ret < 0) {
		dev_err(aw91xxx->dev, "%s: input data invalid!", __func__);
		return ret;
	}

	/* 1. set breath mode */
	aw91xxx_i2c_write(aw91xxx, REG_EN_BREATH, 0x3F);

	/* 2. set fade mode */
	aw91xxx_i2c_write(aw91xxx, REG_CONFIG_P0, 0x00);
	aw91xxx_i2c_write(aw91xxx, REG_CONFIG_P1, 0x00);

	/* 3. set fade time (fade away << 3) | (fade in) */
	aw91xxx_i2c_write(aw91xxx, REG_FADE_TIME,
			(AW91XXX_FADE_TIME_2520MS << 3) | AW91XXX_FADE_TIME_1260MS);

	/* 4. fade mode on or off */
	if (databuf[0] == 0) {
		aw91xxx_i2c_read(aw91xxx, REG_OUTPUT_P0, &reg_val);
		aw91xxx_i2c_write(aw91xxx, REG_OUTPUT_P0, 0xFC & reg_val);
		aw91xxx_i2c_read(aw91xxx, REG_OUTPUT_P1, &reg_val);
		aw91xxx_i2c_write(aw91xxx, REG_OUTPUT_P1, 0xF0 & reg_val);
	} else {
		aw91xxx_i2c_read(aw91xxx, REG_OUTPUT_P0, &reg_val);
		aw91xxx_i2c_write(aw91xxx, REG_OUTPUT_P0, 0x03 | reg_val);
		aw91xxx_i2c_read(aw91xxx, REG_OUTPUT_P1, &reg_val);
		aw91xxx_i2c_write(aw91xxx, REG_OUTPUT_P1, 0x0F | reg_val);
	}

	return len;
}

static DEVICE_ATTR_RW(reg);
static DEVICE_ATTR_RW(hwen);
static DEVICE_ATTR_RW(blink);
static DEVICE_ATTR_WO(dim);
static DEVICE_ATTR_WO(all_dim);
static DEVICE_ATTR_WO(fade_mode);


static struct attribute *aw91xxx_attributes[] = {
	&dev_attr_reg.attr,
	&dev_attr_hwen.attr,
	&dev_attr_blink.attr,
	&dev_attr_dim.attr,
	&dev_attr_all_dim.attr,
	&dev_attr_fade_mode.attr,
	NULL
};

static struct attribute_group aw91xxx_attribute_group = {
	.attrs = aw91xxx_attributes
};


/******************************************************
 *
 * led class dev
 *
 ******************************************************/
static int aw91xxx_parse_led_cdev(struct aw91xxx *aw91xxx, struct device_node *np)
{
	struct device_node *temp;
	int ret = -1;

	temp = of_find_node_by_name(aw91xxx->dev->of_node, "aw91xxx,led");

	ret = of_property_read_string(temp, "aw91xxx,name", &aw91xxx->cdev.name);
	if (ret < 0) {
		dev_err(aw91xxx->dev, "Failure reading led name, ret = %d\n", ret);
		goto free_pdata;
	}
	ret = of_property_read_u32(temp, "aw91xxx,imax", &aw91xxx->imax);
	if (ret < 0) {
		dev_err(aw91xxx->dev, "Failure reading imax, ret = %d\n", ret);
		goto free_pdata;
	}
	ret = of_property_read_u32(temp, "aw91xxx,brightness",
		&aw91xxx->cdev.brightness);
	if (ret < 0) {
		dev_err(aw91xxx->dev, "Failure reading brightness, ret = %d\n", ret);
		goto free_pdata;
	}
	ret = of_property_read_u32(temp, "aw91xxx,max_brightness", &aw91xxx->cdev.max_brightness);
	if (ret < 0) {
		dev_err(aw91xxx->dev, "Failure reading max brightness, ret = %d\n", ret);
		goto free_pdata;
	}
	ret = of_property_read_u32(temp, "aw91xxx,rise_time", &aw91xxx->rise_time);
	if (ret < 0) {
		dev_err(aw91xxx->dev, "Failure reading rise_time, ret = %d\n", ret);
		goto free_pdata;
	}
	ret = of_property_read_u32(temp, "aw91xxx,on_time", &aw91xxx->on_time);
	if (ret < 0) {
		dev_err(aw91xxx->dev, "Failure reading on_time, ret = %d\n", ret);
		goto free_pdata;
	}
	ret = of_property_read_u32(temp, "aw91xxx,fall_time", &aw91xxx->fall_time);
	if (ret < 0) {
		dev_err(aw91xxx->dev, "Failure reading fall_time, ret = %d\n", ret);
		goto free_pdata;
	}
	ret = of_property_read_u32(temp, "aw91xxx,off_time", &aw91xxx->off_time);
	if (ret < 0) {
		dev_err(aw91xxx->dev, "Failure reading off_time, ret = %d\n", ret);
		goto free_pdata;
	}

	INIT_DELAYED_WORK(&aw91xxx->int_work, aw91xxc_led_blink);
	INIT_WORK(&aw91xxx->brightness_work, aw91xxx_brightness_work);
	aw91xxx->cdev.brightness_set = aw91xxx_set_brightness;
	ret = led_classdev_register(aw91xxx->dev, &aw91xxx->cdev);
	if (ret) {
		dev_err(aw91xxx->dev, "unable to register led ret=%d\n", ret);
		goto free_pdata;
	}

	ret = sysfs_create_group(&aw91xxx->cdev.dev->kobj, &aw91xxx_attribute_group);
	if (ret) {
		dev_err(aw91xxx->dev, "led sysfs ret: %d\n", ret);
		goto free_class;
	}

	aw91xxx_led_chip_init(aw91xxx);

	return 0;

free_class:
	led_classdev_unregister(&aw91xxx->cdev);
free_pdata:
	return ret;
}


/******************************************************
 *
 * aw91xxx gpio
 *
 ******************************************************/
static int aw91xxx_parse_for_single_gpio(struct device_node *gpio_node,
			struct aw91xxx *aw91xxx,
			struct aw91xxx_gpio *p_gpio_data)
{
	int ret = 0;
	struct device_node *temp = NULL;
	int i = 0;
	struct aw91xxx_singel_gpio *p_single_gpio_data = p_gpio_data->single_gpio_data;

	for_each_child_of_node(gpio_node, temp) {
		ret = of_property_read_u32(temp, "aw91xxx,gpio_idx",
				&p_single_gpio_data[i].gpio_idx);
		if (ret < 0) {
			dev_err(aw91xxx->dev, "%s: no aw91xxx,gpio_idx, abort\n", __func__);
			goto err_id;
		}
		ret = of_property_read_u32(temp, "aw91xxx,gpio_dir",
				&p_single_gpio_data[i].gpio_direction);
		if (ret < 0) {
			dev_err(aw91xxx->dev, "%s, no aw91xxx,gpio_dir\n", __func__);
			goto err_id;
		}
		ret = of_property_read_u32(temp, "aw91xxx,gpio_default_val",
				&p_single_gpio_data[i].state);
		if (ret < 0) {
			dev_err(aw91xxx->dev, "%s, no aw91xxx,gpio_default_val, abort\n", __func__);
			goto err_id;
		}

		p_gpio_data->gpio_mask |= 0x01 << p_single_gpio_data[i].gpio_idx;
		AW_DEBUG("idx = %d, aw91xxx,gpio_idx = %d\n", i, p_single_gpio_data[i].gpio_idx);
		i++;
	}

	AW_DEBUG("%s gpio_mask = 0x%x\r\n", __func__, p_gpio_data->gpio_mask);

	return 0;

err_id:
	return ret;
}

/* val 0 -> led, gpio -> 1 */
static void
aw91xxx_set_port_mode_by_mask(struct aw91xxx *aw91xxx, unsigned int mask, unsigned int val)
{
	unsigned char reg_val[2] = {0};
	int i = 0;

	/* p0_0 - P1_7 */
	aw91xxx_i2c_read_bits(aw91xxx, REG_WORK_MODE_P0, &reg_val[0], ARRAY_SIZE(reg_val));
	if (aw91xxx->gpio_feature_enable) {
		for (i = 0; i < AW91XXX_PORT_NUM; i++) {
			if (mask & (0x01 << i)) {
				if (val) {
					if (i < 4)
						reg_val[1] |= 0x1 << i;
					else
						reg_val[0] |= 0x1 << (i - 4);
				} else {
					if (i < 4)
						reg_val[1] &= ~(0x1 << i);
					else
						reg_val[0] &= ~(0x1 << (i - 4));
				}
			}
		}
	} else {
		for (i = 0; i < AW91XXX_KEY_PORT_MAX; i++) {
			if (mask & (0x01 << i)) {
				if (val) {
					if (i < 8)
						reg_val[0] |= 0x1 << i;
					else
						reg_val[1] |= 0x1<<(i - 8);
				} else {
					if (i < 8)
						reg_val[0] &= ~(0x1 << i);
					else
						reg_val[1] &= ~(0x1 << (i - 8));
				}
			}
		}
	}

	aw91xxx_i2c_write_bits(aw91xxx, REG_WORK_MODE_P0, reg_val, ARRAY_SIZE(reg_val));
}

static void aw91xxx_set_port_output_mode(struct aw91xxx *aw91xxx, int val)
{
	unsigned char reg_val[1] = {0};

	aw91xxx_i2c_read(aw91xxx, REG_CTRL, &reg_val[0]); /* p0_0 -- p0_7 */
	if (val)
		reg_val[0] |= 0x01 << 4;
	else
		reg_val[0] &= ~(0x01 << 4);
	aw91xxx_i2c_write(aw91xxx, REG_CTRL, reg_val[0]);
}

/* val 1-> output  0->input */
static void aw91xxx_set_port_direction_by_mask(struct aw91xxx *aw91xxx, unsigned int mask, int val)
{
	unsigned char reg_val[2] = {0};
	int i = 0;

	aw91xxx_i2c_read_bits(aw91xxx, REG_CONFIG_P0, reg_val, ARRAY_SIZE(reg_val));
	/* 0-output 1-input */
	if (aw91xxx->gpio_feature_enable) {
		for (i = 0; i < AW91XXX_PORT_NUM; i++) {
			if (mask & (0x01 << i)) {
				if (!val) {
					if (i < 4)
						reg_val[1] |= 0x1 << i;
					else
						reg_val[0] |= 0x1<<(i - 4);
				} else {
					if (i < 4)
						reg_val[1] &= ~(0x1 << i);
					else
						reg_val[0] &= ~(0x1 << (i - 4));
				}
			}
		}
	} else {
		for (i = 0; i < AW91XXX_KEY_PORT_MAX; i++) {
			if (mask & (0x01 << i)) {
				if (!val) {
					if (i < 8)
						reg_val[0] |= 0x1 << i;
					else
						reg_val[1] |= 0x1<<(i - 8);
				} else {
					if (i < 8)
						reg_val[0] &= ~(0x1 << i);
					else
						reg_val[1] &= ~(0x1 << (i - 8));
				}
			}
		}
	}

	if (val == 0)
		aw91xxx_set_port_output_mode(aw91xxx, 1);

	aw91xxx_i2c_write_bits(aw91xxx, REG_CONFIG_P0, reg_val, ARRAY_SIZE(reg_val));
}

/* val 1-> output high  0->output low */
static void aw91xxx_set_port_output_state_by_mask(struct aw91xxx *aw91xxx,
unsigned int mask, int val)
{
	unsigned char reg_val[2] = {0};
	int i = 0;

	aw91xxx_i2c_read_bits(aw91xxx, REG_OUTPUT_P0, reg_val, ARRAY_SIZE(reg_val));
	if (aw91xxx->gpio_feature_enable) {
		for (i = 0; i < AW91XXX_PORT_NUM; i++) {
			if (mask & (0x01 << i)) {
				if (val) {
					if (i < 4)
						reg_val[1] |= 0x1 << i;
					else
						reg_val[0] |= 0x1 << (i - 4);
				} else {
					if (i < 4)
						reg_val[1] &= ~(0x1 << i);
					else
						reg_val[0] &= ~(0x1 << (i - 4));
				}
			}
		}
	} else {
		for (i = 0; i < AW91XXX_KEY_PORT_MAX; i++) {
			if (mask & (0x01 << i)) {
				if (val) {
					if (i < 8)
						reg_val[0] |= 0x1 << i;
					else
						reg_val[1] |= 0x1 << (i - 8);
				} else {
					if (i < 8)
						reg_val[0] &= ~(0x1 << i);
					else
						reg_val[1] &= ~(0x1 << (i - 8));
				}
			}
		}
	}

	aw91xxx_i2c_write_bits(aw91xxx, REG_OUTPUT_P0, reg_val, ARRAY_SIZE(reg_val));
}

/* val 1 -> enable 0 - > disable */
static void
aw91xxx_enbale_interrupt_by_mask(struct aw91xxx *p_aw91xxx, unsigned int mask, unsigned int val)
{
	unsigned char reg_val[2] = {0};
	int i = 0;

	aw91xxx_i2c_read_bits(p_aw91xxx, REG_INT_P0, reg_val, ARRAY_SIZE(reg_val));
	/* 0 enable 1 disable */
	for (i = 0; i < AW91XXX_KEY_PORT_MAX; i++) {
		if (mask & (0x01<<i)) {
			if (!val) {
				if (i < 8)
					reg_val[0] |= 0x1 << i;
				else
					reg_val[1] |= 0x1 << (i - 8);
			} else {
				if (i < 8)
					reg_val[0] &= ~(0x1 << i);
				else
					reg_val[1] &= ~(0x1 << (i - 8));
			}
		}
	}
	aw91xxx_i2c_write_bits(p_aw91xxx, REG_INT_P0, reg_val, ARRAY_SIZE(reg_val));
}

/* Must be read single-byte */
static void aw91xxx_clear_interrupt(struct aw91xxx *p_aw91xxx)
{
	unsigned char reg_val[2] = {0};

	aw91xxx_i2c_read(p_aw91xxx, REG_INPUT_P0, &reg_val[0]);
	aw91xxx_i2c_read(p_aw91xxx, REG_INPUT_P1, &reg_val[1]);
}

static void aw91xxx_gpio_chip_init(struct aw91xxx *aw91xxx, struct aw91xxx_gpio *p_gpio_data)
{
	int i = 0;
	struct aw91xxx_singel_gpio *p_single_gpio_data = p_gpio_data->single_gpio_data;

	aw91xxx_set_port_mode_by_mask(aw91xxx,
			p_gpio_data->gpio_mask, 1); /* gpio mode */
	aw91xxx_set_port_output_mode(aw91xxx,
			p_gpio_data->output_mode); /* OD or pull push mode */
	for (i = 0; i < p_gpio_data->gpio_num; i++) {
		aw91xxx_set_port_direction_by_mask(aw91xxx, 0x01 << p_single_gpio_data[i].gpio_idx,
						p_single_gpio_data[i].gpio_direction);
		if (p_single_gpio_data[i].gpio_direction) { /* output */
			aw91xxx_set_port_output_state_by_mask(aw91xxx,
							0x01 << p_single_gpio_data[i].gpio_idx,
							p_single_gpio_data[i].state);
		}
	}

}

/* echo gpio_idx dirction state > aw91xxx_gpio */
static ssize_t awgpio_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t len)
{
	unsigned int databuf[3];
	int i = 0;
	struct aw91xxx *aw91xxx = dev_get_drvdata(dev);
	struct aw91xxx_gpio *p_gpio_data = aw91xxx->gpio_data;
	struct aw91xxx_singel_gpio *p_single_gpio_data = p_gpio_data->single_gpio_data;

	if (sscanf(buf, "%x %x %x", &databuf[0], &databuf[1], &databuf[2]) == 3)
		AW_DEBUG("aw91xxx gpio cmd param: %x %x %x\n", databuf[0], databuf[1], databuf[2]);
	else
		return len;

	if (p_gpio_data->gpio_mask & (0x01 << databuf[0])) {
		for (i = 0; i < p_gpio_data->gpio_num; i++) {
			if (p_single_gpio_data[i].gpio_idx == databuf[0]) {
				if (p_single_gpio_data[i].gpio_direction != databuf[1]) {
					p_single_gpio_data[i].gpio_direction = databuf[1];
						aw91xxx_set_port_direction_by_mask(aw91xxx, 0x01 << p_single_gpio_data[i].gpio_idx,
								p_single_gpio_data[i].gpio_direction);
				}
				if (p_single_gpio_data[i].gpio_direction == 0x01) { /* output */
					if (p_single_gpio_data[i].state != databuf[2]) {
						p_single_gpio_data[i].state = databuf[2];
							aw91xxx_set_port_output_state_by_mask(aw91xxx,
									0x01 << p_single_gpio_data[i].gpio_idx,
									p_single_gpio_data[i].state);
					}
				}
			}
		}
	}

	return len;
}

static ssize_t awgpio_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	ssize_t len = 0;
	int i = 0;
	struct aw91xxx *aw91xxx = dev_get_drvdata(dev);
	struct aw91xxx_singel_gpio *p_single_gpio_data = aw91xxx->gpio_data->single_gpio_data;

	len += snprintf(buf + len, PAGE_SIZE - len, "Uasge: echo gpio_idx dirction state > aw91xxx_gpio\n");
	for (i = 0; i < aw91xxx->gpio_data->gpio_num; i++) {
		len += snprintf(buf + len, PAGE_SIZE - len, "aw91xxx gpio idx = %x, dir = %x, state = %x\n",
			p_single_gpio_data[i].gpio_idx,
			p_single_gpio_data[i].gpio_direction,
			p_single_gpio_data[i].state);
	}

	return len;
}

static DEVICE_ATTR_RW(awgpio);
static struct attribute *aw91xxx_gpio_attributes[] = {
	&dev_attr_awgpio.attr,
	NULL
};

static struct attribute_group aw91xxx_gpio_attribute_group = {
	.attrs = aw91xxx_gpio_attributes
};

static int aw91xxx_gpio_feature_init(struct aw91xxx *aw91xxx)
{
	int ret = 0;
	int i = 0;
	struct device_node *gpio_node = NULL;
	struct aw91xxx_gpio *p_gpio_data = NULL;
	int gpio_num = 0;

	p_gpio_data = devm_kzalloc(aw91xxx->dev, sizeof(struct aw91xxx_gpio), GFP_KERNEL);
	if (p_gpio_data == NULL)
		return -ENOMEM;

	aw91xxx->gpio_data = p_gpio_data;

	gpio_node = of_find_node_by_name(aw91xxx->dev->of_node, "aw91xxx,gpio");
	if (gpio_node == NULL) {
		dev_err(aw91xxx->dev, "%s: can't find aw91xxx,gpio return failed\r\n", __func__);
		ret = -1;
		goto err_id;
	}
	ret = of_property_read_u32(gpio_node, "aw91xxx,gpio_mode", &p_gpio_data->output_mode);
	if (ret < 0) {
		dev_err(aw91xxx->dev, "%s: no aw91xxx,gpio_mode,abort\n", __func__);
		goto err_id;
	}

	gpio_num = of_get_child_count(gpio_node);
	p_gpio_data->gpio_num = gpio_num;
	p_gpio_data->single_gpio_data = devm_kzalloc(aw91xxx->dev,
				sizeof(struct aw91xxx_singel_gpio) * gpio_num,
				GFP_KERNEL);

	if (p_gpio_data->single_gpio_data == NULL) {
		dev_err(aw91xxx->dev, "%s: malloc memory failed\r\n", __func__);
		ret = -ENOMEM;
		goto err_id;
	}
	ret = aw91xxx_parse_for_single_gpio(gpio_node, aw91xxx, p_gpio_data);
	if (ret) {
		dev_err(aw91xxx->dev, "aw91xxx_parse_single_gpio failed\r\n");
		goto free_mem;
	}

	for (i = 0; i < gpio_num; i++)
		p_gpio_data->single_gpio_data[i].priv = aw91xxx;
	ret = sysfs_create_group(&aw91xxx->dev->kobj, &aw91xxx_gpio_attribute_group);
	if (ret) {
		dev_err(aw91xxx->dev, "gpio sysfs failed ret: %d\n", ret);
		goto free_mem;
	}

	aw91xxx_gpio_chip_init(aw91xxx, p_gpio_data);

	return 0;

free_mem:
	devm_kfree(aw91xxx->dev, p_gpio_data->single_gpio_data);
	devm_kfree(aw91xxx->dev, aw91xxx->gpio_data);
err_id:
	return ret;
}

static void aw91xxx_gpio_free_all_resource(struct aw91xxx *aw91xxx)
{
	if (aw91xxx->gpio_feature_enable) {
		devm_kfree(aw91xxx->dev, aw91xxx->gpio_data->single_gpio_data);
		devm_kfree(aw91xxx->dev, aw91xxx->gpio_data);
	}
}

/*********************************************************
 *
 * aw91xxx key feature
 *
 ********************************************************/
static int aw91xxx_parse_dt_for_key(struct aw91xxx_key *p_key_data, struct device_node *np)
{
	int ret = 0;
	int i = 0;
	unsigned int val = 0;
	struct aw91xxx *aw91xxx = p_key_data->priv;

	p_key_data->wake_up_enable = false;
	ret = of_property_read_u32(np, "aw91xxx,wake_up_enable", &val);
	if (ret == 0) {
		if (val == 1)
			p_key_data->wake_up_enable = true;
	} else {
		dev_err(aw91xxx->dev, "%s: no aw91xxx,wake_up_enable, abort\n", __func__);
		return ret;
	}

	ret = of_property_read_u32(np, "aw91xxx,input_port_mask",  &p_key_data->input_port_mask);
	if (ret) {
		dev_err(aw91xxx->dev, "%s: no aw91xxx,input_port_mask, abort\n", __func__);
		return ret;
	}

	p_key_data->input_port_nums = 0;
	for (i = 0; i < AW91XXX_KEY_PORT_MAX; i++) {
		if (p_key_data->input_port_mask & (0x01 << i))
			p_key_data->input_port_nums++;
	}

	if (aw91xxx->single_key_enable) {
		p_key_data->key_mask =  p_key_data->input_port_mask;
		AW_DEBUG("aw91xxx key input_port_mask = 0x%x, input_num	= %d\n",
						p_key_data->input_port_mask,
						p_key_data->input_port_nums);
	}

	if (aw91xxx->matrix_key_enable) {
		ret = of_property_read_u32(np, "aw91xxx,output_port_mask",
					   &p_key_data->output_port_mask);
		if (ret) {
			dev_err(aw91xxx->dev, "%s, no aw91xxx,output_port_mask, abort\n", __func__);
			return ret;
		}
		p_key_data->output_port_nums = 0;
		for (i = 0; i < AW91XXX_KEY_PORT_MAX; i++) {
			if (p_key_data->output_port_mask & (0x01 << i))
				p_key_data->output_port_nums++;
		}
		p_key_data->key_mask
		= p_key_data->input_port_mask | p_key_data->output_port_mask;
		AW_DEBUG("aw91xxx key output_port_mask = 0x%x, output_nmu = %d\n",
						p_key_data->output_port_mask,
						p_key_data->output_port_nums);
	}

	return 0;
}

irqreturn_t aw91xxx_irq_func(int irq, void *key_data)
{
	struct aw91xxx_key *p_key_data = (struct aw91xxx_key *)key_data;
	struct aw91xxx *p_aw91xxx = p_key_data->priv;

	disable_irq_nosync(p_key_data->priv->irq_num);
	/* disable aw91xxx input interrupt */
	aw91xxx_enbale_interrupt_by_mask(p_aw91xxx, p_key_data->input_port_mask, 0);
	aw91xxx_clear_interrupt(p_aw91xxx);
	schedule_delayed_work(&p_key_data->int_work, msecs_to_jiffies(p_key_data->debounce_delay));
	return 0;
}

static int aw91xxx_irq_register(struct aw91xxx *p_aw91xxx)
{
	int ret = 0;
	struct device_node *np = p_aw91xxx->dev->of_node;

	p_aw91xxx->irq_gpio = of_get_named_gpio(np, "irq-gpio", 0);
	if (p_aw91xxx->irq_gpio < 0) {
		dev_err(p_aw91xxx->dev, "%s: get irq gpio failed\r\n", __func__);
		return -EINVAL;
	}
	ret = devm_gpio_request_one(p_aw91xxx->dev, p_aw91xxx->irq_gpio, GPIOF_OUT_INIT_LOW, "aw91xxx irq gpio");
	if (ret) {
		dev_err(p_aw91xxx->dev, "%s: devm_gpio_request irq gpio failed\r\n", __func__);
		return -EBUSY;
	}
	gpio_direction_input(p_aw91xxx->irq_gpio);
	p_aw91xxx->irq_num = gpio_to_irq(p_aw91xxx->irq_gpio);
	if (p_aw91xxx->irq_num < 0) {
		ret = p_aw91xxx->irq_num;
		dev_err(p_aw91xxx->dev, "%s gpio to irq failed\r\n", __func__);
		goto err;
	}

	AW_DEBUG("aw91xxx irq num=%d\n", p_aw91xxx->irq_num);
	ret = devm_request_threaded_irq(p_aw91xxx->dev, p_aw91xxx->irq_num, NULL,
					aw91xxx_irq_func,
					IRQF_TRIGGER_LOW | IRQF_ONESHOT,
					"aw91xxx_irq", p_aw91xxx->key_data);
	if (ret < 0) {
		dev_err(p_aw91xxx->dev, "%s register irq failed\r\n", __func__);
		goto err;
	}
	/* enable_irq_wake(p_aw91xxx->irq_num); */
	device_init_wakeup(p_aw91xxx->dev, 1);
	return 0;
err:
	gpio_free(p_aw91xxx->irq_gpio);
	return ret;
}

static int aw91xxx_input_dev_register(struct aw91xxx *p_aw91xxx)
{
	int ret = 0;
	int i = 0;
	struct aw91xxx_key *p_key_data = p_aw91xxx->key_data;

	p_key_data->input = input_allocate_device();

	if (p_key_data->input == NULL) {
		ret = -ENOMEM;
		dev_err(p_aw91xxx->dev, "%s: failed to allocate input device\n", __func__);
		return ret;
	}
	p_key_data->input->name = "aw91xxx-key";
	p_key_data->input->dev.parent = p_aw91xxx->dev;
	p_key_data->keymap_len = sizeof(key_map) / sizeof(KEY_STATE);
	p_key_data->keymap = (KEY_STATE *)&key_map;
	input_set_drvdata(p_key_data->input, p_key_data);

	__set_bit(EV_KEY, p_key_data->input->evbit);
	__set_bit(EV_SYN, p_key_data->input->evbit);
	for (i = 0; i < p_key_data->keymap_len; i++)
		__set_bit(p_key_data->keymap[i].key_code & KEY_MAX, p_key_data->input->keybit);

	ret = input_register_device(p_key_data->input);
	if (ret) {
		input_free_device(p_key_data->input);
		dev_err(p_aw91xxx->dev, "%s: failed to allocate input device\n", __func__);
		return ret;
	}
	return 0;
}

void aw91xxx_int_work(struct work_struct *work)
{
	struct delayed_work *p_delayed_work = container_of(work,
							  struct delayed_work,
							  work);
	struct aw91xxx_key *p_key_data = container_of(p_delayed_work,
						     struct aw91xxx_key,
						     int_work);
	schedule_work(&p_key_data->key_work);
}

/* val 1-> output high  0->output low */
static unsigned int aw91xxx_get_port_input_state(struct aw91xxx *p_aw91xxx)
{
	unsigned char reg_val[2] = {0};

	aw91xxx_i2c_read(p_aw91xxx, REG_INPUT_P0, &reg_val[0]);
	aw91xxx_i2c_read(p_aw91xxx, REG_INPUT_P1, &reg_val[1]);
	return reg_val[0] | (reg_val[1] << 8);
}

static void aw91xxx_key_work(struct work_struct *work)
{
	int i = 0;
	int j = 0;
	int real_idx = 0;
	int real_row = 0;
	int real_col = 0;
	int key_code = 0;
	int key_val = 0;
	int key_num = 0;
	unsigned int input_val = 0;
	unsigned int retry_state;
	struct aw91xxx_key *p_key_data = container_of(work, struct aw91xxx_key, key_work);
	struct aw91xxx *p_aw91xxx = p_key_data->priv;
	unsigned int *new_state = p_key_data->new_output_state;
	unsigned int *old_state = p_key_data->old_output_state;
	unsigned char reg_val[2] = {0};

	//if wake_up_enable set to 1,the key fuction can be use in suspend mode.
	//if wake_up_enable set to 0,the key fuction will be banned in suspend mode.

	if ((!p_key_data->wake_up_enable) && (!p_aw91xxx->screen_state)) {
		enable_irq(p_aw91xxx->irq_num);
		aw91xxx_enbale_interrupt_by_mask(p_aw91xxx, p_key_data->input_port_mask, 1);
		aw91xxx_clear_interrupt(p_aw91xxx);
		return;
	}

	if (p_aw91xxx->matrix_key_enable) {
		for (i = 0, real_idx = 0; i < AW91XXX_KEY_PORT_MAX; i++) {
			if (p_key_data->output_port_mask & (0x01 << i)) {
				aw91xxx_i2c_read_bits(p_aw91xxx, REG_CONFIG_P0,
						reg_val,
						ARRAY_SIZE(reg_val));

				input_val = reg_val[0] | (reg_val[1] << 8);
				input_val |= p_key_data->output_port_mask;
				input_val &= ~(0x01 << i);

				reg_val[0] = input_val & 0xff;
				reg_val[1] = (input_val >> 8) & 0xff;

				aw91xxx_i2c_write_bits(p_aw91xxx, REG_CONFIG_P0,
						reg_val, ARRAY_SIZE(reg_val));

				aw91xxx_i2c_read_bits(p_aw91xxx, REG_INPUT_P0,
						reg_val, ARRAY_SIZE(reg_val));
				new_state[real_idx] = (reg_val[0] | (reg_val[1] << 8)) & p_key_data->input_port_mask;
				real_idx++;
			}
		}
		/* key state change */
		if (memcmp(&new_state[0], &old_state[0], p_key_data->output_port_nums * sizeof(unsigned int))) {
			/* stage changed */
			for (i = 0, real_col = 0; i < AW91XXX_KEY_PORT_MAX; i++) {
				if (p_key_data->output_port_mask & (0x01 << i)) {
					if (new_state[real_col] != old_state[real_col]) {
						for (j = 0, real_row = 0; j < AW91XXX_KEY_PORT_MAX; j++) {
							if (p_key_data->input_port_mask & (0x01 << j)) {
								if ((new_state[real_col] & (0x01 << j)) != (old_state[real_col] & (0x01 << j))) {
									key_code = p_key_data->keymap[real_row * p_key_data->output_port_nums + real_col].key_code;
									key_val = (old_state[real_col] & (0x01 << j)) ? 1 : 0;/* press or release */
									AW_DEBUG("aw91xxx report: key_num = %d\n", (real_row * p_key_data->output_port_nums + real_col));
									AW_DEBUG("aw91xxx report: key_code = %x, key_val = %d\n", key_code, key_val);
									AW_DEBUG("aw91xxx real_row = %d, real_col = %d\n", real_row, real_col);
									input_report_key(p_key_data->input, key_code, key_val);
									input_sync(p_key_data->input);
								}
								real_row++;
							}
						}
					}
					real_col++;
				}
			}
		}

		memcpy(&old_state[0], &new_state[0], p_key_data->output_port_nums * sizeof(unsigned int));

		/* all key release */
		if (!memcmp(&new_state[0], &p_key_data->def_output_state[0],
			p_key_data->output_port_nums * sizeof(unsigned int))) {
			aw91xxx_set_port_direction_by_mask(p_aw91xxx, p_key_data->output_port_mask, 1); /* set output mode */
			aw91xxx_clear_interrupt(p_aw91xxx); /* clear inputerrupt */
			enable_irq(p_aw91xxx->irq_num);
			aw91xxx_enbale_interrupt_by_mask(p_aw91xxx, p_key_data->input_port_mask, 1);/* enable input interrupt */
			retry_state = aw91xxx_get_port_input_state(p_aw91xxx);
			if ((retry_state & p_key_data->input_port_mask) != p_key_data->input_port_mask) {
				disable_irq_nosync(p_aw91xxx->irq_num);
				aw91xxx_enbale_interrupt_by_mask(p_aw91xxx, p_key_data->input_port_mask, 0);
			} else {
				AW_DEBUG("%s enter, all key release.\n", __func__);
				return;
			}
		}
		hrtimer_start(&p_key_data->key_timer, ktime_set(0, (1000 / HRTIMER_FRAME) * 1000), HRTIMER_MODE_REL);
	}

	if (p_aw91xxx->single_key_enable) {
		p_key_data->new_input_state = aw91xxx_get_port_input_state(p_aw91xxx);
		p_key_data->new_input_state &= p_key_data->input_port_mask;
		real_idx = 0;
		if (p_key_data->new_input_state != p_key_data->old_input_state) {
			for (i = 0; i < AW91XXX_KEY_PORT_MAX; i++) {
				if (p_key_data->input_port_mask & (0x01 << i)) {
					if ((p_key_data->new_input_state & (0x01 << i)) != (p_key_data->old_input_state & (0x01 << i))) {
						key_code = p_key_data->keymap[real_idx].key_code;
						key_val = (p_key_data->old_input_state & 0x01 << i) ? 1 : 0; /* press or release */
						key_num = aw91xxx_separate_key_data[real_idx];
						/*if (key_val == 1) */
						/*	AW_DEBUG("%s, key%d pressed\n", __func__, key_num);*/
						/*else*/
						/*	AW_DEBUG("%s, key%d release\n", __func__, key_num);*/
						/* AW_DEBUG("%s, key_code = 0x%x, key_val = 0x%x\r\n", __func__, key_code, key_val); */
						input_report_key(p_key_data->input, key_code, key_val);
						input_sync(p_key_data->input);
					}
					real_idx++;
				}
			}
		}
		/* AW_DEBUG("%s,new input state = 0x%x, old input state = 0x%x\r\n", */
		/*			__func__, p_key_data->new_input_state, */
		/*			p_key_data->old_input_state); */

		p_key_data->old_input_state = p_key_data->new_input_state;
		aw91xxx_clear_interrupt(p_aw91xxx);
		enable_irq(p_aw91xxx->irq_num);
		/* enable input interrupt */
		aw91xxx_enbale_interrupt_by_mask(p_aw91xxx, p_key_data->input_port_mask, 1);
		return;
	}
}

static enum hrtimer_restart aw91xxx_timer_func(struct hrtimer *p_hrtimer)
{
	struct aw91xxx_key *p_key_data = container_of(p_hrtimer, struct aw91xxx_key, key_timer);

	schedule_work(&p_key_data->key_work);
	return HRTIMER_NORESTART;
}

static void aw91xxx_key_chip_init(struct aw91xxx_key *p_key_data)
{
	unsigned int all_mask = 0;
	struct aw91xxx *p_aw91xxx = p_key_data->priv;



	disable_irq_nosync(p_key_data->priv->irq_num);
	aw91xxx_enbale_interrupt_by_mask(p_aw91xxx, AW91XXX_INT_MASK, 0); /* disale P0 P1 interrupt */
	all_mask = p_key_data->input_port_mask | p_key_data->output_port_mask;
	aw91xxx_set_port_mode_by_mask(p_aw91xxx, all_mask, 1); /* set  gpio mode */
	aw91xxx_set_port_direction_by_mask(p_aw91xxx, p_key_data->input_port_mask, 0); /* input mode */
	aw91xxx_set_port_direction_by_mask(p_aw91xxx, p_key_data->output_port_mask, 1); /* output mode */
	aw91xxx_set_port_output_mode(p_aw91xxx, 1); /* set output port pull push mode */

	aw91xxx_set_port_output_state_by_mask(p_aw91xxx, p_key_data->output_port_mask, 0);/* set output low */

	aw91xxx_enbale_interrupt_by_mask(p_aw91xxx, p_key_data->input_port_mask, 1); /* enable input interrupt */
	/* clear inputerrupt */
	aw91xxx_clear_interrupt(p_aw91xxx);
	enable_irq(p_key_data->priv->irq_num);
}

static void aw91xxx_key_free_all_resource(struct aw91xxx *aw91xxx)
{
	if (aw91xxx->matrix_key_enable) {
		devm_kfree(aw91xxx->dev, aw91xxx->key_data->new_output_state);
		devm_kfree(aw91xxx->dev, aw91xxx->key_data->old_output_state);
		devm_kfree(aw91xxx->dev, aw91xxx->key_data->def_output_state);
		input_unregister_device(aw91xxx->key_data->input);
		input_free_device(aw91xxx->key_data->input);
		gpio_free(aw91xxx->irq_gpio);
		devm_kfree(aw91xxx->dev, aw91xxx->key_data);
	}
}

static void aw91xxx_single_key_chip_init(struct aw91xxx_key *p_key_data)
{
	unsigned int all_mask = 0;
	struct aw91xxx *p_aw91xxx = p_key_data->priv;

	disable_irq_nosync(p_key_data->priv->irq_num);
	aw91xxx_enbale_interrupt_by_mask(p_aw91xxx, AW91XXX_INT_MASK, 0); /* disale P0 P1 interrupt */
	all_mask = p_key_data->input_port_mask;
	p_key_data->old_input_state = p_key_data->input_port_mask;
	aw91xxx_set_port_mode_by_mask(p_aw91xxx, all_mask, 1); /* set  gpio mode */
	aw91xxx_set_port_direction_by_mask(p_aw91xxx, p_key_data->input_port_mask, 0); /* input mode */
	aw91xxx_set_port_output_mode(p_aw91xxx, 1); /* set output port pull push mode */

	aw91xxx_enbale_interrupt_by_mask(p_aw91xxx, p_key_data->input_port_mask, 1);/* enable input interrupt */
	/* clear inputerrupt */
	aw91xxx_clear_interrupt(p_aw91xxx);
	enable_irq(p_key_data->priv->irq_num);
}

static int aw91xxx_key_feature_init(struct aw91xxx *aw91xxx)
{
	int ret = 0;
	int i = 0;
	struct aw91xxx_key *p_aw91xxx = NULL;
	struct device_node *key_node = NULL;

	p_aw91xxx = devm_kzalloc(aw91xxx->dev, sizeof(struct aw91xxx_key), GFP_KERNEL);
	if (p_aw91xxx == NULL)
		return -ENOMEM;

	aw91xxx->key_data = p_aw91xxx;
	p_aw91xxx->priv = aw91xxx;
	key_node = of_find_node_by_name(aw91xxx->dev->of_node, "aw91xxx,key");
	if (key_node == NULL) {
		dev_err(aw91xxx->dev, "%s: can't find aw91xxx,key node return failed\n", __func__);
		ret = -EINVAL;
		goto err_id;
	}
	ret = aw91xxx_parse_dt_for_key(p_aw91xxx, key_node);
	if (ret) {
		dev_err(aw91xxx->dev, "aw91xxx_parse_dt_for_key failed, check dts\n");
		goto err_id;
	}
	p_aw91xxx->old_output_state = devm_kzalloc(aw91xxx->dev,
						    sizeof(unsigned int) * p_aw91xxx->output_port_nums,
						    GFP_KERNEL);
	if (p_aw91xxx->old_output_state == NULL) {
		dev_err(aw91xxx->dev, "%s:aw91xxx->old_output_state malloc memory failed\r\n", __func__);
		goto err_id;
	}
	p_aw91xxx->new_output_state = devm_kzalloc(aw91xxx->dev,
						    sizeof(unsigned int) * p_aw91xxx->output_port_nums,
						    GFP_KERNEL);
	if (p_aw91xxx->new_output_state == NULL) {
		dev_err(aw91xxx->dev, "%s:aw91xxx->new_output_state malloc memory failed\r\n", __func__);
		goto free_old_output;
	}

	p_aw91xxx->def_output_state = devm_kzalloc(aw91xxx->dev,
						    sizeof(unsigned int) * p_aw91xxx->output_port_nums, GFP_KERNEL);
	if (p_aw91xxx->def_output_state  == NULL) {
		ret = -ENOMEM;
		goto free_new_output;
	}

	for (i = 0; i < p_aw91xxx->output_port_nums; i++) {
		p_aw91xxx->new_output_state[i] = p_aw91xxx->input_port_mask;
		p_aw91xxx->old_output_state[i] = p_aw91xxx->input_port_mask;
		p_aw91xxx->def_output_state[i] = p_aw91xxx->input_port_mask;
	}

	ret = aw91xxx_input_dev_register(aw91xxx);
	if (ret < 0) {
		dev_err(aw91xxx->dev, "%s input dev register failed\r\n", __func__);
		goto free_def_output;
	}

	p_aw91xxx->debounce_delay = 1;
	INIT_DELAYED_WORK(&p_aw91xxx->int_work, aw91xxx_int_work);
	INIT_WORK(&p_aw91xxx->key_work, aw91xxx_key_work);
	hrtimer_setup(&p_aw91xxx->key_timer, aw91xxx_timer_func, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	ret = aw91xxx_irq_register(aw91xxx);
	if (ret < 0) {
		dev_err(aw91xxx->dev, "%s irq register failed\r\n", __func__);
		goto input_dev;
	}

	if (aw91xxx->matrix_key_enable)
		aw91xxx_key_chip_init(p_aw91xxx);

	if (aw91xxx->single_key_enable)
		aw91xxx_single_key_chip_init(p_aw91xxx);
	return 0;
input_dev:
	input_unregister_device(p_aw91xxx->input);
free_def_output:
	devm_kfree(aw91xxx->dev, p_aw91xxx->def_output_state);
free_new_output:
	devm_kfree(aw91xxx->dev, p_aw91xxx->new_output_state);
free_old_output:
	devm_kfree(aw91xxx->dev, p_aw91xxx->old_output_state);
err_id:
	devm_kfree(aw91xxx->dev, aw91xxx->key_data);
	return ret;
}

/******************************************************
 *
 * i2c driver
 *
 ******************************************************/
static int aw91xxx_i2c_probe(struct i2c_client *i2c)
{
	struct aw91xxx *aw91xxx;
	struct device_node *np = i2c->dev.of_node;
	int ret;

	if (!i2c_check_functionality(i2c->adapter, I2C_FUNC_I2C)) {
		dev_err(&i2c->dev, "check_functionality failed\n");
		return -EIO;
	}

	aw91xxx = devm_kzalloc(&i2c->dev, sizeof(struct aw91xxx), GFP_KERNEL);
	if (aw91xxx == NULL)
		return -ENOMEM;

	aw91xxx->dev = &i2c->dev;
	aw91xxx->i2c = i2c;

	i2c_set_clientdata(i2c, aw91xxx);

	/* aw91xxx rst & int */
	if (np) {
		ret = aw91xxx_parse_dt(&i2c->dev, aw91xxx, np);
		if (ret) {
			dev_err(&i2c->dev, "%s: failed to parse device tree node\n", __func__);
			goto err;
		}
		} else {
			aw91xxx->reset_gpio = -1;
	}

	if (gpio_is_valid(aw91xxx->reset_gpio)) {
		ret = devm_gpio_request_one(&i2c->dev, aw91xxx->reset_gpio,
			GPIOF_OUT_INIT_LOW, "aw91xxx_rst");
		if (ret) {
			dev_err(&i2c->dev, "%s: rst request failed\n", __func__);
			goto err;
		}
	}

	/* hardware reset */
	aw91xxx_hw_reset(aw91xxx);

	/* aw91xxx chip id */
	ret = aw91xxx_read_chipid(aw91xxx);
	if (ret < 0) {
		dev_err(&i2c->dev, "%s: aw91xxx_read_chipid failed ret=%d\n", __func__, ret);
		goto err_free_rst;
	}

	aw91xxx_identify_vendorid(aw91xxx);

	dev_set_drvdata(&i2c->dev, aw91xxx);

	if (aw91xxx->single_key_enable) {
		/* single key init */
		ret = aw91xxx_key_feature_init(aw91xxx);
		if (ret) {
			dev_err(aw91xxx->dev, "aw91xxx single key feature init failed \r\n");
			goto err_free_rst;
		}
	}

	if (aw91xxx->matrix_key_enable) {
		/* key init */
		ret = aw91xxx_key_feature_init(aw91xxx);
		if (ret) {
			dev_err(aw91xxx->dev, "aw91xxx key feature init failed \r\n");
			goto err_free_rst;
		}
	}

	if (aw91xxx->led_feature_enable) {
		ret = aw91xxx_parse_led_cdev(aw91xxx, np);
		if (ret < 0) {
			dev_err(&i2c->dev, "%s error creating led class dev\n", __func__);
			goto free_key;
		}
	}

	if (aw91xxx->gpio_feature_enable) {
		/* gpio init */
		ret = aw91xxx_gpio_feature_init(aw91xxx);
		if (ret) {
			dev_err(aw91xxx->dev, "aw91xxx gpio feature init failed \r\n");
			goto free_key;
		}
	}
	aw91xxx->screen_state = true;

	pr_err("%s probe completed successfully!\n", __func__);

	return 0;

free_key:
	aw91xxx_key_free_all_resource(aw91xxx);
err_free_rst:
	gpio_free(aw91xxx->reset_gpio);
err:
	devm_kfree(&i2c->dev, aw91xxx);
	return ret;
}

static void aw91xxx_i2c_remove(struct i2c_client *i2c)
{
	struct aw91xxx *aw91xxx = i2c_get_clientdata(i2c);

	if (gpio_is_valid(aw91xxx->reset_gpio))
		gpio_free(aw91xxx->reset_gpio);
	aw91xxx_gpio_free_all_resource(aw91xxx);
	devm_kfree(aw91xxx->dev, aw91xxx);

}

static const struct i2c_device_id aw91xxx_i2c_id[] = {
	{ AW91XXX_I2C_NAME, 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, aw91xxx_i2c_id);

static const struct of_device_id aw91xxx_dt_match[] = {
	{ .compatible = "awinic,aw91xxx_led" },
	{ },
};

static struct i2c_driver aw91xxx_i2c_driver = {
	.driver = {
		.name = AW91XXX_I2C_NAME,
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(aw91xxx_dt_match),
	},
	.probe = aw91xxx_i2c_probe,
	.remove = aw91xxx_i2c_remove,
	.id_table = aw91xxx_i2c_id,
};

static int __init aw91xxx_i2c_init(void)
{
	int ret = 0;

	pr_err("aw91xxx driver version %s\n", AW91XXX_DRIVER_VERSION);

	ret = i2c_add_driver(&aw91xxx_i2c_driver);
	if (ret) {
		pr_err("fail to add aw91xxx device into i2c\n");
		return ret;
	}

	return 0;
}
module_init(aw91xxx_i2c_init);

static void __exit aw91xxx_i2c_exit(void)
{
	i2c_del_driver(&aw91xxx_i2c_driver);
}
module_exit(aw91xxx_i2c_exit);


MODULE_DESCRIPTION("AW91XXX LED Driver");
MODULE_LICENSE("GPL v2");
