// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2025 LONTIUM
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <drm/drm_modes.h>
#include <sound/hdmi-codec.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_edid.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_of.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>
#include <linux/pm_runtime.h>

#define FW_SIZE (64 * 1024)
#define LT_PAGE_SIZE 256
#define FW_FILE  "LT9611C.bin"
#define NOT_UPGRADE 0
#define UPGRADE 1

struct lt9611c {
	struct device *dev;
	struct i2c_client *client;
	struct drm_bridge bridge;
	struct drm_bridge *next_bridge;
	struct regmap *regmap;
	/* Protects all accesses to registers by stopping the on-chip MCU */
	struct mutex ocm_lock;
	struct work_struct work;
	struct device_node *dsi0_node;
	struct device_node *dsi1_node;
	struct mipi_dsi_device *dsi0;
	struct mipi_dsi_device *dsi1;
	struct platform_device *audio_pdev;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[2];
	struct device *codec_dev;
	struct task_struct *kthread;
	struct task_struct *test_kthread;
	hdmi_codec_plugged_cb plugged_cb;
	const struct firmware *fw;
	u64 fw_version;
	u8 *edid_buf;
	int edid_len;
	bool edid_valid;
	u8 fw_crc;

	bool work_inited;
	bool bridge_added;
	bool regulators_enabled;
	bool hdmi_connected;
	enum drm_connector_status audio_status;
};

static const struct regmap_range chip_ranges[] = {
	{ .range_min = 0, .range_max = 0xff },
};

static const struct regmap_access_table chip_table = {
	.yes_ranges = chip_ranges,
	.n_yes_ranges = ARRAY_SIZE(chip_ranges),
};

static const struct regmap_config lt9611c_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.volatile_table = &chip_table,
	.cache_type = REGCACHE_NONE,
};

struct crc_info {
	u8 width;
	u32 poly;
	u32 crc_init;
	u32 xor_out;
	bool refin;
	bool refout;
};

static void lt9611c_audio_update_connector_status(struct lt9611c *lt9611c);

static unsigned int bits_reverse(u32 in_val, u8 bits)
{
	u32 out_val = 0;
	u8 i;

	for (i = 0; i < bits; i++) {
		if (in_val & (1 << i))
			out_val |= 1 << (bits - 1 - i);
	}

	return out_val;
}

static unsigned int get_crc(struct crc_info type, const u8 *buf, u64 buf_len)
{
	u8 width = type.width;
	u32 poly = type.poly;
	u32 crc = type.crc_init;
	u32 xorout = type.xor_out;
	bool refin = type.refin;
	bool refout = type.refout;
	u8 n;
	u32 bits;
	u32 data;
	u8 i;

	n = (width < 8) ? 0 : (width - 8);
	crc = (width < 8) ? (crc << (8 - width)) : crc;
	bits = (width < 8) ? 0x80 : (1 << (width - 1));
	poly = (width < 8) ? (poly << (8 - width)) : poly;
	while (buf_len--) {
		data = *(buf++);
		if (refin)
			data = bits_reverse(data, 8);
		crc ^= (data << n);
		for (i = 0; i < 8; i++) {
			if (crc & bits)
				crc = (crc << 1) ^ poly;
			else
				crc = crc << 1;
		}
	}
	crc = (width < 8) ? (crc >> (8 - width)) : crc;
	if (refout)
		crc = bits_reverse(crc, width);
	crc ^= xorout;

	return (crc & ((2 << (width - 1)) - 1));
}

static u8 calculate_crc(struct lt9611c *lt9611c)
{
	struct crc_info type = {
		.width = 8,
		.poly = 0x31,
		.crc_init = 0,
		.xor_out = 0,
		.refout = false,
		.refin = false,
	};
	const u8 *upgrade_data;
	u64 len;
	u64 crc_size = FW_SIZE - 1;
	u8 default_val = 0xFF;

	if (!lt9611c->fw || !lt9611c->fw->data || lt9611c->fw->size == 0) {
		dev_err(lt9611c->dev, "firmware data not available for CRC\n");
		return 0;
	}

	upgrade_data = lt9611c->fw->data;
	len = lt9611c->fw->size;

	type.crc_init = get_crc(type, upgrade_data, len);

	crc_size -= len;
	while (crc_size--)
		type.crc_init = get_crc(type, &default_val, 1);

	return type.crc_init;
}

static int i2c_write_byte(struct lt9611c *lt9611c, u8 reg, u8 val)
{
	struct device *dev = lt9611c->dev;
	int ret = 0;

	ret = regmap_write(lt9611c->regmap, reg, val);
	if (ret < 0) {
		dev_err(dev,
			"regmap_write error: i2c addr=0x%02x, reg addr=0x%02x, error=%d",
			lt9611c->client->addr, reg, ret);
	}

	return ret;
}

static int i2c_read_byte(struct lt9611c *lt9611c, u8 reg, u8 *val)
{
	struct device *dev = lt9611c->dev;
	int ret = 0;
	unsigned int tmp;

	if (!val)
		return -EINVAL;

	ret = regmap_read(lt9611c->regmap, reg, &tmp);
	if (ret < 0) {
		dev_err(dev,
			"regmap_read error: i2c addr=0x%02x, reg addr=0x%02x, error=%d",
			lt9611c->client->addr, reg, ret);

		return ret;
	}

	*val = (u8)tmp;

	return 0;
}

static int i2c_read_write_flow(struct lt9611c *lt9611c, u8 *params,
			       unsigned int param_count, u8 *return_buffer,
			       unsigned int return_count)
{
	int count, i;
	u8 temp;

	i2c_write_byte(lt9611c, 0xFF, 0xE0);
	i2c_write_byte(lt9611c, 0xDE, 0x01);

	count = 0;
	do {
		i2c_read_byte(lt9611c, 0xAE, &temp);
		usleep_range(1000, 2000);
		count++;
	} while (count < 100 && temp != 0x01);

	if (temp != 0x01)
		return -1;

	for (i = 0; i < param_count; i++) {
		if (i > 0xDD - 0xB0)
			break;

		i2c_write_byte(lt9611c, 0xB0 + i, params[i]);
	}

	i2c_write_byte(lt9611c, 0xDE, 0x02);

	count = 0;
	do {
		i2c_read_byte(lt9611c, 0xAE, &temp);
		usleep_range(1000, 2000);
		count++;
	} while (count < 100 && temp != 0x02);

	if (temp != 0x02)
		return -2;

	for (i = 0; i < return_count; i++)
		i2c_read_byte(lt9611c, 0x85 + i, &return_buffer[i]);

	return 0;
}

static int lt9611c_prepare_firmware_data(struct lt9611c *lt9611c)
{
	struct device *dev = lt9611c->dev;
	int ret;

	/* ensure filesystem ready */
	msleep(3000);
	ret = request_firmware(&lt9611c->fw, FW_FILE, dev);
	if (ret) {
		dev_err(dev, "failed load file '%s', error type %d\n", FW_FILE, ret);
		return ret;
	}

	if (lt9611c->fw->size > FW_SIZE - 1) {
		dev_err(dev, "firmware too large (%zu > %d)\n", lt9611c->fw->size, FW_SIZE - 1);
		lt9611c->fw = NULL;
		return -EINVAL;
	}

	dev_info(dev, "firmware size: %zu bytes\n", lt9611c->fw->size);

	lt9611c->fw_crc = calculate_crc(lt9611c);

	dev_info(dev, "LT9611C.bin crc: 0x%02X\n", lt9611c->fw_crc);

	return 0;
}

static void lt9611c_config_parameters(struct lt9611c *lt9611c)
{
	i2c_write_byte(lt9611c, 0xFF, 0xE0);
	i2c_write_byte(lt9611c, 0xEE, 0x01);
	//fifo_rst_n
	i2c_write_byte(lt9611c, 0xFF, 0xE1);
	i2c_write_byte(lt9611c, 0x03, 0x3F);
	i2c_write_byte(lt9611c, 0x03, 0xFF);

	i2c_write_byte(lt9611c, 0xFF, 0xE0);
	i2c_write_byte(lt9611c, 0x5E, 0xC1);
	i2c_write_byte(lt9611c, 0x58, 0x00);
	i2c_write_byte(lt9611c, 0x59, 0x50);
	i2c_write_byte(lt9611c, 0x5A, 0x10);
	i2c_write_byte(lt9611c, 0x5A, 0x00);
	i2c_write_byte(lt9611c, 0x58, 0x21);
}

static void lt9611c_flash_to_fifo(struct lt9611c *lt9611c, u64 addr)
{
	i2c_write_byte(lt9611c, 0xff, 0xe0);
	i2c_write_byte(lt9611c, 0x5e, 0x5f);
	i2c_write_byte(lt9611c, 0x5a, 0x20);
	i2c_write_byte(lt9611c, 0x5a, 0x00);
	i2c_write_byte(lt9611c, 0x5b, ((addr & 0xFF0000) >> 16));
	i2c_write_byte(lt9611c, 0x5c, ((addr & 0xFF00) >> 8));
	i2c_write_byte(lt9611c, 0x5d, (addr & 0xFF));
	i2c_write_byte(lt9611c, 0x5a, 0x10);
	i2c_write_byte(lt9611c, 0x5a, 0x00);
}

static void lt9611c_wren(struct lt9611c *lt9611c)
{
	i2c_write_byte(lt9611c, 0xff, 0xe0);
	i2c_write_byte(lt9611c, 0x5a, 0x04);
	i2c_write_byte(lt9611c, 0x5a, 0x00);
}

static void lt9611c_wrdi(struct lt9611c *lt9611c)
{
	i2c_write_byte(lt9611c, 0xFF, 0xE0);
	i2c_write_byte(lt9611c, 0x5A, 0x08);
	i2c_write_byte(lt9611c, 0x5A, 0x00);
}

static int lt9611c_upgrade_judgment(struct lt9611c *lt9611c)
{
	struct device *dev = lt9611c->dev;
	int ret;
	u8 flash_crc;

	if (!lt9611c)
		return -EINVAL;

	lt9611c_config_parameters(lt9611c);
	lt9611c_flash_to_fifo(lt9611c, FW_SIZE - 1);
	i2c_write_byte(lt9611c, 0x58, 0x21);

	ret = i2c_read_byte(lt9611c, 0x5f, &flash_crc);
	if (ret < 0) {
		dev_err(dev, "failed to read flash crc\n");
		return ret;
	}

	dev_info(dev, "flash firmware crc=0x%02X, expected crc=0x%02X",
		 flash_crc, lt9611c->fw_crc);

	lt9611c_wrdi(lt9611c);

	return (flash_crc == lt9611c->fw_crc) ? NOT_UPGRADE : UPGRADE;
}

static int read_flash_reg_status(struct lt9611c *lt9611c, u8 *status)
{
	struct device *dev = lt9611c->dev;
	int ret;

	//fifo_rst_n
	i2c_write_byte(lt9611c, 0xFF, 0xE1);
	i2c_write_byte(lt9611c, 0x03, 0x3F);
	i2c_write_byte(lt9611c, 0x03, 0xFF);

	i2c_write_byte(lt9611c, 0xFF, 0xE0);
	i2c_write_byte(lt9611c, 0x5e, 0x40);
	i2c_write_byte(lt9611c, 0x56, 0x05);
	i2c_write_byte(lt9611c, 0x55, 0x25);
	i2c_write_byte(lt9611c, 0x55, 0x01);
	i2c_write_byte(lt9611c, 0x58, 0x21);

	ret = i2c_read_byte(lt9611c, 0x5f, status);
	if (ret < 0)
		dev_err(dev, "failed to read flash register status\n");

	return ret;
}

static void lt9611c_block_erase(struct lt9611c *lt9611c)
{
	struct device *dev = lt9611c->dev;
	u32 i = 0;
	u8 flash_status = 0;
	u8 block_num = 0x00;
	u32 flash_addr = 0x00;

	for (block_num = 0; block_num < 2; block_num++) {
		flash_addr = (block_num * 0x008000);
		i2c_write_byte(lt9611c, 0xFF, 0xE0);
		i2c_write_byte(lt9611c, 0xEE, 0x01);
		i2c_write_byte(lt9611c, 0x5A, 0x04);
		i2c_write_byte(lt9611c, 0x5A, 0x00);
		i2c_write_byte(lt9611c, 0x5B, flash_addr >> 16);
		i2c_write_byte(lt9611c, 0x5C, flash_addr >> 8);
		i2c_write_byte(lt9611c, 0x5D, flash_addr);
		i2c_write_byte(lt9611c, 0x5A, 0x01);
		i2c_write_byte(lt9611c, 0x5A, 0x00);
		msleep(100);
		i = 0;
		while (1) {
			read_flash_reg_status(lt9611c, &flash_status);
			if ((flash_status & 0x01) == 0)
				break;

			if (i > 50)
				break;

			i++;
			msleep(50);
		}
	}

	dev_info(dev, "erase flash done.\n");
}

static void lt9611c_crc_to_sram(struct lt9611c *lt9611c)
{
	i2c_write_byte(lt9611c, 0xff, 0xe0);
	i2c_write_byte(lt9611c, 0x51, 0x00);
	i2c_write_byte(lt9611c, 0x55, 0xc0);
	i2c_write_byte(lt9611c, 0x55, 0x80);
	i2c_write_byte(lt9611c, 0x5e, 0xc0);
	i2c_write_byte(lt9611c, 0x58, 0x21);
}

static void lt9611c_data_to_sram(struct lt9611c *lt9611c)
{
	i2c_write_byte(lt9611c, 0xff, 0xe0);
	i2c_write_byte(lt9611c, 0x51, 0xff);
	i2c_write_byte(lt9611c, 0x55, 0x80);
	i2c_write_byte(lt9611c, 0x5e, 0xc0);
	i2c_write_byte(lt9611c, 0x58, 0x21);
}

static void lt9611c_sram_to_flash(struct lt9611c *lt9611c, u64 addr)
{
	i2c_write_byte(lt9611c, 0xff, 0xe0);
	i2c_write_byte(lt9611c, 0x5b, ((addr & 0xFF0000) >> 16));
	i2c_write_byte(lt9611c, 0x5c, ((addr & 0xFF00) >> 8));
	i2c_write_byte(lt9611c, 0x5d, (addr & 0xFF));
	i2c_write_byte(lt9611c, 0x5A, 0x30);
	i2c_write_byte(lt9611c, 0x5A, 0x00);
}

static int lt9611c_write_data(struct lt9611c *lt9611c, u64 addr)
{
	struct device *dev = lt9611c->dev;
	int ret;
	int page = 0, num = 0, i = 0;
	const u8 *data;
	u64 size, index;
	u8 value;

	data = lt9611c->fw->data;
	size = lt9611c->fw->size;

	page = (size + LT_PAGE_SIZE - 1) / LT_PAGE_SIZE;

	if (page * LT_PAGE_SIZE > 64 * 1024) {
		dev_err(dev, "firmware size out of range\n");
		return -EINVAL;
	}

	dev_info(dev, "%u pages, total size %llu byte\n", page, size);

	for (num = 0; num < page; num++) {
		lt9611c_data_to_sram(lt9611c);

		for (i = 0; i < LT_PAGE_SIZE; i++) {
			index = num * LT_PAGE_SIZE + i;
			value = (index < size) ? data[index] : 0xFF;

			ret = i2c_write_byte(lt9611c, 0x59, value);
			if (ret < 0) {
				dev_err(dev, "write error at page %u, index %u\n", num, i);
				return ret;
			}
		}

		lt9611c_wren(lt9611c);
		lt9611c_sram_to_flash(lt9611c, addr);

		addr += LT_PAGE_SIZE;
	}

	lt9611c_wrdi(lt9611c);

	return 0;
}

static int lt9611c_write_crc(struct lt9611c *lt9611c, u64 addr)
{
	struct device *dev = lt9611c->dev;
	int ret;
	u8 crc;

	crc = lt9611c->fw_crc;
	lt9611c_crc_to_sram(lt9611c);
	ret = i2c_write_byte(lt9611c, 0x59, crc);
	if (ret < 0) {
		dev_err(dev, "failed to write CRC\n");
		return -1;
	}

	lt9611c_wren(lt9611c);
	lt9611c_sram_to_flash(lt9611c, addr);
	lt9611c_wrdi(lt9611c);

	dev_info(dev, "CRC 0x%02X written to flash at addr 0x%llX\n", crc, addr);

	return 0;
}

static int lt9611c_firmware_upgrade(struct lt9611c *lt9611c)
{
	struct device *dev = lt9611c->dev;
	int ret;

	dev_info(dev, "starting firmware upgrade, size: %zu bytes\n", lt9611c->fw->size);

	lt9611c_config_parameters(lt9611c);
	lt9611c_block_erase(lt9611c);

	ret = lt9611c_write_data(lt9611c, 0);
	if (ret < 0) {
		dev_err(dev, "Failed to write firmware data\n");
		return ret;
	}

	ret = lt9611c_write_crc(lt9611c, FW_SIZE - 1);
	if (ret < 0) {
		dev_err(dev, "Failed to write firmware CRC\n");
		return ret;
	}

	return 0;
}

static int lt9611c_upgrade_result(struct lt9611c *lt9611c)
{
	struct device *dev = lt9611c->dev;
	u8 crc_result;

	i2c_write_byte(lt9611c, 0xff, 0xe0);
	i2c_write_byte(lt9611c, 0xee, 0x01);
	i2c_read_byte(lt9611c, 0x21, &crc_result);

	if (crc_result == lt9611c->fw_crc) {
		dev_info(dev, "LT9611C firmware upgrade success, CRC=0x%02X\n", crc_result);
		return 0;
	}

	dev_err(dev, "LT9611C firmware upgrade failed, expected CRC=0x%02X, read CRC=0x%02X\n",
		lt9611c->fw_crc, crc_result);
	return -EIO;
}

static struct lt9611c *bridge_to_lt9611c(struct drm_bridge *bridge)
{
	return container_of(bridge, struct lt9611c, bridge);
}

static void lt9611c_lock(struct lt9611c *lt9611c)
{
	mutex_lock(&lt9611c->ocm_lock);
	i2c_write_byte(lt9611c, 0xff, 0xe0);
	i2c_write_byte(lt9611c, 0xee, 0x01);
}

static void lt9611c_unlock(struct lt9611c *lt9611c)
{
	i2c_write_byte(lt9611c, 0xff, 0xe0);
	i2c_write_byte(lt9611c, 0xee, 0x00);
	mutex_unlock(&lt9611c->ocm_lock);
}

static irqreturn_t lt9611c_irq_thread_handler(int irq, void *dev_id)
{
	struct lt9611c *lt9611c = dev_id;
	struct device *dev = lt9611c->dev;
	int ret;
	u8 irq_status;
	u8 cmd[5] = {0x52, 0x48, 0x31, 0x3a, 0x00};
	u8 data[5];

	mutex_lock(&lt9611c->ocm_lock);
	i2c_write_byte(lt9611c, 0xff, 0xe0);
	i2c_read_byte(lt9611c, 0x84, &irq_status);

	if (!(irq_status & BIT(0))) {
		mutex_unlock(&lt9611c->ocm_lock);
		return IRQ_HANDLED;
	}
	dev_info(dev, "HPD interrupt triggered.\n");

	i2c_write_byte(lt9611c, 0xdf, irq_status & BIT(0));
	usleep_range(10000, 12000);
	i2c_write_byte(lt9611c, 0xdf, irq_status & (~BIT(0)));

	ret = i2c_read_write_flow(lt9611c, cmd, 5, data, 5);
	if (ret) {
		dev_err(dev, "failed to read HPD status\n");
	} else {
		lt9611c->hdmi_connected = (data[4] == 0x02);
		dev_info(dev, "HDMI %s\n", lt9611c->hdmi_connected ? "connected" : "disconnected");
	}

	lt9611c->audio_status = lt9611c->hdmi_connected ?
			connector_status_connected :
			connector_status_disconnected;

	schedule_work(&lt9611c->work);

	mutex_unlock(&lt9611c->ocm_lock);

	return IRQ_HANDLED;
}

static void lt9611c_hpd_work(struct work_struct *work)
{
	struct lt9611c *lt9611c = container_of(work, struct lt9611c, work);
	bool connected;

	mutex_lock(&lt9611c->ocm_lock);
	connected = lt9611c->hdmi_connected;
	mutex_unlock(&lt9611c->ocm_lock);

	drm_bridge_hpd_notify(&lt9611c->bridge,
			      connected ?
			 connector_status_connected :
			 connector_status_disconnected);

	lt9611c_audio_update_connector_status(lt9611c);
}

static void lt9611c_reset(struct lt9611c *lt9611c)
{
	gpiod_set_value_cansleep(lt9611c->reset_gpio, 1);
	msleep(20);
	gpiod_set_value_cansleep(lt9611c->reset_gpio, 0);
	msleep(20);
	gpiod_set_value_cansleep(lt9611c->reset_gpio, 1);
	msleep(400);
}

static int lt9611c_regulator_init(struct lt9611c *lt9611c)
{
	struct device *dev = lt9611c->dev;
	int ret;

	lt9611c->supplies[0].supply = "vcc";
	lt9611c->supplies[1].supply = "vdd";

	ret = devm_regulator_bulk_get(dev, 2, lt9611c->supplies);

	return ret;
}

static int lt9611c_regulator_enable(struct lt9611c *lt9611c)
{
	int ret;

	ret = regulator_enable(lt9611c->supplies[0].consumer);
	if (ret < 0)
		return ret;

	usleep_range(5000, 10000);

	ret = regulator_enable(lt9611c->supplies[1].consumer);
	if (ret < 0) {
		regulator_disable(lt9611c->supplies[0].consumer);
		return ret;
	}

	return ret;
}

static int lt9611c_regulator_disable(struct lt9611c *lt9611c)
{
	int ret;

	ret = regulator_disable(lt9611c->supplies[0].consumer);
	if (ret < 0)
		return ret;

	usleep_range(5000, 10000);

	ret = regulator_disable(lt9611c->supplies[1].consumer);
	if (ret < 0)
		return ret;

	return 0;
}

static struct mipi_dsi_device *lt9611c_attach_dsi(struct lt9611c *lt9611c,
						  struct device_node *dsi_node)
{
	const struct mipi_dsi_device_info info = { "lt9611c", 0, NULL };
	struct mipi_dsi_device *dsi;
	struct mipi_dsi_host *host;
	struct device *dev = lt9611c->dev;
	int ret;

	host = of_find_mipi_dsi_host_by_node(dsi_node);
	if (!host)
		return ERR_PTR(dev_err_probe(dev, -EPROBE_DEFER, "failed to find dsi host\n"));

	dsi = devm_mipi_dsi_device_register_full(dev, host, &info);
	if (IS_ERR(dsi)) {
		dev_err(dev, "failed to create dsi device\n");
		return dsi;
	}

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_SYNC_PULSE |
			 MIPI_DSI_MODE_VIDEO_HSE;

	ret = devm_mipi_dsi_attach(dev, dsi);
	if (ret < 0) {
		dev_err(dev, "failed to attach dsi to host\n");
		return ERR_PTR(ret);
	}

	return dsi;
}

static int lt9611c_bridge_attach(struct drm_bridge *bridge,
				 struct drm_encoder *encoder,
				 enum drm_bridge_attach_flags flags)
{
	struct lt9611c *lt9611c = bridge_to_lt9611c(bridge);

	return drm_bridge_attach(encoder, lt9611c->next_bridge,
				 bridge, flags);
}

static enum drm_mode_status lt9611c_bridge_mode_valid(struct drm_bridge *bridge,
						      const struct drm_display_info *info,
			 const struct drm_display_mode *mode)
{
	u32 pixclk;

	pixclk = (mode->htotal * mode->vtotal * drm_mode_vrefresh(mode)) / 1000000;

	if (pixclk >= 25 && pixclk <= 340)
		return MODE_OK;
	else
		return MODE_BAD;
}

static void lt9611c_bridge_mode_set(struct drm_bridge *bridge,
				    const struct drm_display_mode *mode,
				 const struct drm_display_mode *adj_mode)
{
	struct lt9611c *lt9611c = bridge_to_lt9611c(bridge);
	struct device *dev = lt9611c->dev;
	int ret;
	u32 h_total, hactive, hsync_len, hfront_porch, hback_porch;
	u32 v_total, vactive, vsync_len, vfront_porch, vback_porch;
	u8 video_timing_set_cmd[26] = {0x57, 0x4D, 0x33, 0x3A};
	u8 return_timing_set_param[3];
	u8 framerate;
	u8 vic = 0x00;

	h_total = mode->htotal;
	hactive = mode->hdisplay;
	hsync_len = mode->hsync_end - mode->hsync_start;
	hfront_porch = mode->hsync_start - mode->hdisplay;
	hback_porch = mode->htotal - mode->hsync_end;

	v_total = mode->vtotal;
	vactive = mode->vdisplay;
	vsync_len = mode->vsync_end - mode->vsync_start;
	vfront_porch = mode->vsync_start - mode->vdisplay;
	vback_porch = mode->vtotal - mode->vsync_end;
	framerate = drm_mode_vrefresh(mode);
	vic = drm_match_cea_mode(mode);

	dev_info(dev, "Out video info:\n");
	dev_info(dev,
		 "h_total=%d, hactive=%d, hsync_len=%d, hfront_porch=%d, hback_porch=%d\n",
	h_total, hactive, hsync_len, hfront_porch, hback_porch);
	dev_info(dev,
		 "v_total=%d, vactive=%d, vsync_len=%d, vfront_porch=%d, vback_porch=%d\n",
	v_total, vactive, vsync_len, vfront_porch, vback_porch);

	dev_info(dev, "framerate=%d\n", framerate);
	dev_info(dev, "vic = 0x%02X\n", vic);

	video_timing_set_cmd[4] = (h_total >> 8) & 0xFF;
	video_timing_set_cmd[5] = h_total & 0xFF;
	video_timing_set_cmd[6] = (hactive >> 8) & 0xFF;
	video_timing_set_cmd[7] = hactive & 0xFF;
	video_timing_set_cmd[8] = (hfront_porch >> 8) & 0xFF;
	video_timing_set_cmd[9] = hfront_porch & 0xFF;
	video_timing_set_cmd[10] = (hsync_len >> 8) & 0xFF;
	video_timing_set_cmd[11] = hsync_len & 0xFF;
	video_timing_set_cmd[12] = (hback_porch >> 8) & 0xFF;
	video_timing_set_cmd[13] = hback_porch & 0xFF;
	video_timing_set_cmd[14] = (v_total >> 8) & 0xFF;
	video_timing_set_cmd[15] = v_total & 0xFF;
	video_timing_set_cmd[16] = (vactive >> 8) & 0xFF;
	video_timing_set_cmd[17] = vactive & 0xFF;
	video_timing_set_cmd[18] = (vfront_porch >> 8) & 0xFF;
	video_timing_set_cmd[19] = vfront_porch & 0xFF;
	video_timing_set_cmd[20] = (vsync_len >> 8) & 0xFF;
	video_timing_set_cmd[21] = vsync_len & 0xFF;
	video_timing_set_cmd[22] = (vback_porch >> 8) & 0xFF;
	video_timing_set_cmd[23] = vback_porch & 0xFF;
	video_timing_set_cmd[24] = framerate;
	video_timing_set_cmd[25] = vic;

	mutex_lock(&lt9611c->ocm_lock);
	ret = i2c_read_write_flow(lt9611c, video_timing_set_cmd, 26, return_timing_set_param, 3);
	if (ret)
		dev_err(dev, "video set failed\n");
	mutex_unlock(&lt9611c->ocm_lock);
}

static enum drm_connector_status lt9611c_bridge_detect(struct drm_bridge *bridge,
						       struct drm_connector *connector)
{
	struct lt9611c *lt9611c = bridge_to_lt9611c(bridge);
	struct device *dev = lt9611c->dev;
	int ret;
	bool connected = false;
	u8 cmd[5] = {0x52, 0x48, 0x31, 0x3a, 0x00};
	u8 data[5];

	mutex_lock(&lt9611c->ocm_lock);
	ret = i2c_read_write_flow(lt9611c, cmd, 5, data, 5);
	if (ret) {
		dev_err(dev, "Failed to read HPD status, cannot determine HDMI connection (err=%d)\n",
			ret);
	} else {
		connected = (data[4] == 0x02);
	}

	lt9611c->hdmi_connected = connected;

	if (lt9611c->hdmi_connected)
		lt9611c->audio_status = connector_status_connected;
	else
		lt9611c->audio_status = connector_status_disconnected;

	mutex_unlock(&lt9611c->ocm_lock);

	return connected ? connector_status_connected :
				connector_status_disconnected;
}

static int lt9611c_read_edid(struct lt9611c *lt9611c)
{
	struct device *dev = lt9611c->dev;
	int ret, i, bytes_to_copy, offset = 0;
	u8 packets_num;
	u8 read_edid_data_cmd[5] = {0x52, 0x48, 0x33, 0x3A, 0x00};
	u8 return_edid_data[37];
	u8 read_edid_byte_num_cmd[5] = {0x52, 0x48, 0x32, 0x3A, 0x00};
	u8 return_edid_byte_num[6];

	ret = i2c_read_write_flow(lt9611c, read_edid_byte_num_cmd, 5, return_edid_byte_num, 6);
	if (ret) {
		dev_err(dev, "Failed to read EDID byte number\n");
		lt9611c->edid_valid = false;
		return ret;
	}

	lt9611c->edid_len = (return_edid_byte_num[4] << 8) | return_edid_byte_num[5];

	if (!lt9611c->edid_buf || lt9611c->edid_len > (lt9611c->edid_valid ?
				lt9611c->edid_len : 0)) {
		kfree(lt9611c->edid_buf);
		lt9611c->edid_buf = kzalloc(lt9611c->edid_len, GFP_KERNEL);
		if (!lt9611c->edid_buf) {
			dev_err(dev, "Failed to allocate EDID buffer\n");
			lt9611c->edid_len = 0;
			lt9611c->edid_valid = false;
			return -ENOMEM;
		}
	}

	packets_num = (lt9611c->edid_len % 32) ? (lt9611c->edid_len / 32 + 1) :
		(lt9611c->edid_len / 32);
	for (i = 0; i < packets_num; i++) {
		read_edid_data_cmd[4] = (u8)i;
		ret = i2c_read_write_flow(lt9611c, read_edid_data_cmd, 5, return_edid_data, 37);
		if (ret) {
			dev_err(dev, "Failed to read EDID packet %d\n", i);
			lt9611c->edid_valid = false;
			return -EIO;
		}
		offset = i * 32;
		bytes_to_copy = min(32, lt9611c->edid_len - offset);
		memcpy(lt9611c->edid_buf + offset, &return_edid_data[5], bytes_to_copy);
		}

	lt9611c->edid_valid = true;

	return ret;
}

static int lt9611c_get_edid_block(void *data, u8 *buf, unsigned int block, size_t len)
{
	struct lt9611c *lt9611c = data;
	struct device *dev = lt9611c->dev;
	unsigned int total_blocks;
	int ret;

	if (len > 128)
		return -EINVAL;

	guard(mutex)(&lt9611c->ocm_lock);
	if (block == 0 || !lt9611c->edid_valid) {
		ret = lt9611c_read_edid(lt9611c);
		if (ret) {
			dev_err(dev, "EDID read failed\n");
			return ret;
		}
	}

	total_blocks = lt9611c->edid_len / 128;
	if (!total_blocks) {
		dev_err(dev, "No valid EDID blocks\n");
		return -EIO;
	}

	if (block >= total_blocks) {
		dev_err(dev,  "Requested block %u exceeds total blocks %u\n",
			block, total_blocks);
		return -EINVAL;
	}

	memcpy(buf, lt9611c->edid_buf + block * 128, len);

	return 0;
}

static const struct drm_edid *lt9611c_bridge_edid_read(struct drm_bridge *bridge,
						       struct drm_connector *connector)
{
	struct lt9611c *lt9611c = bridge_to_lt9611c(bridge);

	usleep_range(10000, 20000);
	return drm_edid_read_custom(connector, lt9611c_get_edid_block, lt9611c);
}

static const struct drm_bridge_funcs lt9611c_bridge_funcs = {
	.attach = lt9611c_bridge_attach,
	.mode_valid = lt9611c_bridge_mode_valid,
	.mode_set = lt9611c_bridge_mode_set,
	.detect = lt9611c_bridge_detect,
	.edid_read = lt9611c_bridge_edid_read,
};

static int lt9611c_parse_dt(struct device *dev,
			    struct lt9611c *lt9611c)
{
	lt9611c->dsi0_node = of_graph_get_remote_node(dev->of_node, 0, -1);
	if (!lt9611c->dsi0_node) {
		dev_err(dev, "failed to get remote node for primary dsi\n");
		return -ENODEV;
	}

	lt9611c->dsi1_node = of_graph_get_remote_node(dev->of_node, 1, -1);

	return drm_of_find_panel_or_bridge(dev->of_node, 2, -1, NULL, &lt9611c->next_bridge);
}

static int lt9611c_gpio_init(struct lt9611c *lt9611c)
{
	struct device *dev = lt9611c->dev;

	lt9611c->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(lt9611c->reset_gpio)) {
		dev_err(dev, "failed to acquire reset gpio\n");
		return PTR_ERR(lt9611c->reset_gpio);
	}

	return 0;
}

static void lt9611c_read_version(struct lt9611c *lt9611c, u64 *version)
{
	u8 val;
	u64 ver = 0;

	i2c_write_byte(lt9611c, 0xff, 0xe0);
	i2c_write_byte(lt9611c, 0xee, 0x01);

	i2c_read_byte(lt9611c, 0x80, &val);
	ver = val;

	i2c_read_byte(lt9611c, 0x81, &val);
	ver = (ver << 8) | val;

	*version = ver;
}

static int lt9611c_read_chipid(struct lt9611c *lt9611c)
{
	struct device *dev = lt9611c->dev;
	u8 val = 0;

	i2c_write_byte(lt9611c, 0xff, 0xe0);
	i2c_write_byte(lt9611c, 0xee, 0x01);
	i2c_write_byte(lt9611c, 0xff, 0xe1);

	i2c_read_byte(lt9611c, 0x00, &val);
	if (val != 0x23)
		return -ENODEV;

	i2c_read_byte(lt9611c, 0x01, &val);
	if (val != 0x06)
		return -ENODEV;

	dev_info(dev, "ChipId = 0x2306\n");

	return 0;
}

static int lt9611c_hdmi_hw_params(struct device *dev, void *data,
				  struct hdmi_codec_daifmt *fmt,
				 struct hdmi_codec_params *hparms)
{
	struct lt9611c *lt9611c = dev_get_drvdata(dev);

	 dev_info(lt9611c->dev, "SOC sample_rate: %d, sample_width: %d, fmt: %d\n",
		  hparms->sample_rate, hparms->sample_width, fmt->fmt);

	switch (hparms->sample_rate) {
	case 32000:
	case 44100:
	case 48000:
	case 88200:
	case 96000:
	case 176400:
	case 192000:
		break;
	default:
		return -EINVAL;
	}

	switch (hparms->sample_width) {
	case 16:
	case 18:
	case 20:
	case 24:
		break;
	default:
		return -EINVAL;
	}

	switch (fmt->fmt) {
	case HDMI_I2S:
	case HDMI_SPDIF:
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static void lt9611c_audio_shutdown(struct device *dev, void *data)
{
}

static int lt9611c_audio_startup(struct device *dev, void *data)
{
	return 0;
}

static void lt9611c_audio_update_connector_status(struct lt9611c *lt9611c)
{
	enum drm_connector_status status;

	status = lt9611c->audio_status;
	if (lt9611c->plugged_cb && lt9611c->codec_dev)
		lt9611c->plugged_cb(lt9611c->codec_dev,
				 status == connector_status_connected);
}

static int lt9611c_hdmi_audio_hook_plugged_cb(struct device *dev,
					      void *data,
					 hdmi_codec_plugged_cb fn,
					 struct device *codec_dev)
{
	struct lt9611c *lt9611c = data;

	lt9611c->plugged_cb = fn;
	lt9611c->codec_dev = codec_dev;
	lt9611c_audio_update_connector_status(lt9611c);

	return 0;
}

static const struct hdmi_codec_ops lt9611c_codec_ops = {
	.hw_params	= lt9611c_hdmi_hw_params,
	.audio_shutdown = lt9611c_audio_shutdown,
	.audio_startup = lt9611c_audio_startup,
	.hook_plugged_cb = lt9611c_hdmi_audio_hook_plugged_cb,
};

static int lt9611c_audio_init(struct device *dev, struct lt9611c *lt9611c)
{
	struct hdmi_codec_pdata codec_data = {
		.ops = &lt9611c_codec_ops,
		.max_i2s_channels = 2,
		.i2s = 1,
		.data = lt9611c,
	};

	lt9611c->audio_pdev =
		platform_device_register_data(dev, HDMI_CODEC_DRV_NAME,
					      PLATFORM_DEVID_AUTO,
					 &codec_data, sizeof(codec_data));

	return PTR_ERR_OR_ZERO(lt9611c->audio_pdev);
}

static void lt9611c_audio_exit(struct lt9611c *lt9611c)
{
	if (lt9611c->audio_pdev) {
		platform_device_unregister(lt9611c->audio_pdev);
		lt9611c->audio_pdev = NULL;
	}
}

static int lt9611c_firmware_update_store(struct lt9611c *lt9611c)
{
	struct device *dev = lt9611c->dev;
	int ret;

	lt9611c_lock(lt9611c);
	ret = lt9611c_prepare_firmware_data(lt9611c);
	if (ret < 0) {
		dev_err(dev, "Failed prepare firmware data: %d\n", ret);
		goto out;
	}

	ret = lt9611c_firmware_upgrade(lt9611c);
	if (ret < 0) {
		dev_err(dev, "upgrade failure\n");
		goto out;
	}
	lt9611c_reset(lt9611c);
	ret = lt9611c_upgrade_result(lt9611c);
	if (ret < 0)
		goto out;

out:
	lt9611c_unlock(lt9611c);
	lt9611c_reset(lt9611c);
	if (lt9611c->fw) {
		release_firmware(lt9611c->fw);
		lt9611c->fw = NULL;
	}

	return ret;
}

static ssize_t lt9611c_firmware_store(struct device *dev, struct device_attribute *attr,
				      const char *buf, size_t len)
{
	struct lt9611c *lt9611c = dev_get_drvdata(dev);
	int ret;

	ret = lt9611c_firmware_update_store(lt9611c);
	if (ret < 0)
		return ret;
	return len;
}

static ssize_t lt9611c_firmware_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct lt9611c *lt9611c = dev_get_drvdata(dev);

	return sysfs_emit(buf, "0x%04llx\n", lt9611c->fw_version);
}

static DEVICE_ATTR_RW(lt9611c_firmware);

static struct attribute *lt9611c_attrs[] = {
	&dev_attr_lt9611c_firmware.attr,
	NULL,
};

static const struct attribute_group lt9611c_attr_group = {
	.attrs = lt9611c_attrs,
};

static const struct attribute_group *lt9611c_attr_groups[] = {
	&lt9611c_attr_group,
	NULL,
};

static void lt9611c_cleanup_resources(struct lt9611c *lt9611c)
{
	struct device *dev = lt9611c->dev;

	if (lt9611c->work_inited) {
		cancel_work_sync(&lt9611c->work);
		lt9611c->work_inited = false;
		dev_err(dev, "work cancelled\n");
	}

	if (lt9611c->bridge_added) {
		drm_bridge_remove(&lt9611c->bridge);
		lt9611c->bridge_added = false;
		dev_err(dev, "DRM bridge removed\n");
	}

	if (lt9611c->regulators_enabled) {
		regulator_bulk_disable(ARRAY_SIZE(lt9611c->supplies), lt9611c->supplies);
		lt9611c->regulators_enabled = false;
		dev_err(dev, "regulators disabled\n");
	}

	if (lt9611c->audio_pdev)
		lt9611c_audio_exit(lt9611c);

	if (lt9611c->fw) {
		release_firmware(lt9611c->fw);
		lt9611c->fw = NULL;
		dev_err(dev, "firmware released\n");
	}

	if (lt9611c->dsi0_node) {
		of_node_put(lt9611c->dsi0_node);
		lt9611c->dsi0_node = NULL;
		dev_err(dev, "dsi0 node released\n");
	}

	if (lt9611c->dsi1_node) {
		of_node_put(lt9611c->dsi1_node);
		lt9611c->dsi1_node = NULL;
		dev_err(dev, "dsi1 node released\n");
	}
}

static int lt9611c_main(void *data)
{
	struct lt9611c *lt9611c = data;
	struct device *dev = lt9611c->dev;
	struct i2c_client *client = lt9611c->client;
	int ret;

	lt9611c->work_inited = false;
	lt9611c->bridge_added = false;
	lt9611c->regulators_enabled = false;

	ret = lt9611c_parse_dt(dev, lt9611c);
	if (ret) {
		dev_err(dev, "failed to parse device tree\n");
		return ret;
	}

	ret = lt9611c_gpio_init(lt9611c);
	if (ret < 0)
		goto err_cleanup;

	ret = lt9611c_regulator_init(lt9611c);
	if (ret < 0)
		goto err_cleanup;

	ret = lt9611c_regulator_enable(lt9611c);
	if (ret)
		goto err_cleanup;

	lt9611c->regulators_enabled = true;

	lt9611c_reset(lt9611c);

	ret = lt9611c_read_chipid(lt9611c);
	if (ret < 0) {
		dev_err(dev, "failed to read chip id.\n");
		goto err_cleanup;
	}

	lt9611c_lock(lt9611c);
	lt9611c_read_version(lt9611c, &lt9611c->fw_version);

	ret = lt9611c_prepare_firmware_data(lt9611c);
	if (ret == 0 && lt9611c_upgrade_judgment(lt9611c) == UPGRADE) {
		dev_info(dev, "firmware upgrade needed\n");

		ret = lt9611c_firmware_upgrade(lt9611c);
		if (ret < 0) {
			dev_err(dev, "firmware upgrade failed\n");
			lt9611c_unlock(lt9611c);
			goto err_cleanup;
		}

		lt9611c_reset(lt9611c);
		ret = lt9611c_upgrade_result(lt9611c);
		if (ret < 0) {
			lt9611c_unlock(lt9611c);
			goto err_cleanup;
		}

		lt9611c_read_version(lt9611c, &lt9611c->fw_version);
		lt9611c_unlock(lt9611c);

	} else {
		dev_info(dev, "skip firmware upgrade, using chip internal firmware\n");
		lt9611c_unlock(lt9611c);
	}

	if (lt9611c->fw) {
		release_firmware(lt9611c->fw);
		lt9611c->fw = NULL;
	}
	dev_info(dev, "current version:0x%04llx", lt9611c->fw_version);

	INIT_WORK(&lt9611c->work, lt9611c_hpd_work);
	lt9611c->work_inited = true;

	if (!client->irq) {
		dev_err(dev, "failed to get INTP IRQ\n");
		ret = -ENODEV;
		goto err_cleanup;
	}

	ret = devm_request_threaded_irq(&client->dev, client->irq, NULL,
					lt9611c_irq_thread_handler,
					IRQF_TRIGGER_HIGH | IRQF_ONESHOT |
					IRQF_NO_AUTOEN,
					"lt9611c", lt9611c);
	if (ret) {
		dev_err(dev, "failed to request irq\n");
		goto err_cleanup;
	}

	lt9611c->bridge.funcs = &lt9611c_bridge_funcs;
	lt9611c->bridge.of_node = lt9611c->client->dev.of_node;
	lt9611c->bridge.ops = DRM_BRIDGE_OP_DETECT | DRM_BRIDGE_OP_EDID | DRM_BRIDGE_OP_HPD;
	lt9611c->bridge.type = DRM_MODE_CONNECTOR_HDMIA;

	drm_bridge_add(&lt9611c->bridge);
	lt9611c->bridge_added = true;

	/* Attach primary DSI */
	lt9611c->dsi0 = lt9611c_attach_dsi(lt9611c, lt9611c->dsi0_node);
	if (IS_ERR(lt9611c->dsi0)) {
		ret = PTR_ERR(lt9611c->dsi0);
		dev_err(dev, "Failed to attach primary DSI, error=%d\n", ret);
		goto err_cleanup;
	}

	/* Attach secondary DSI, if specified */
	if (lt9611c->dsi1_node) {
		lt9611c->dsi1 = lt9611c_attach_dsi(lt9611c, lt9611c->dsi1_node);
		if (IS_ERR(lt9611c->dsi1)) {
			ret = PTR_ERR(lt9611c->dsi1);
			dev_err(dev, "Failed to attach secondary DSI, error=%d\n", ret);
			goto err_cleanup;
		}
	}

	lt9611c->audio_status = connector_status_disconnected;

	ret = lt9611c_audio_init(dev, lt9611c);
	if (ret < 0) {
		dev_err(dev, "audio init failed\n");
		goto err_cleanup;
	}

	lt9611c_reset(lt9611c);
	enable_irq(lt9611c->client->irq);

	return 0;

err_cleanup:
	lt9611c_cleanup_resources(lt9611c);
	return ret;
}

static int lt9611c_probe(struct i2c_client *client)
{
	struct lt9611c *lt9611c;
	struct device *dev = &client->dev;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(dev, "device doesn't support I2C\n");
		return -ENODEV;
	}

	lt9611c = devm_kzalloc(dev, sizeof(*lt9611c), GFP_KERNEL);
	if (!lt9611c)
		return -ENOMEM;

	lt9611c->dev = dev;
	lt9611c->client = client;
	mutex_init(&lt9611c->ocm_lock);

	lt9611c->regmap = devm_regmap_init_i2c(client, &lt9611c_regmap_config);
	if (IS_ERR(lt9611c->regmap)) {
		dev_err(dev, "regmap i2c init failed\n");
		return PTR_ERR(lt9611c->regmap);
	}

	i2c_set_clientdata(client, lt9611c);

	lt9611c->kthread = kthread_run(lt9611c_main, lt9611c, "lt9611c");
	if (IS_ERR(lt9611c->kthread)) {
		dev_err(dev, "Failed to create kernel thread\n");
		return PTR_ERR(lt9611c->kthread);
	}

	return 0;
}

static void lt9611c_remove(struct i2c_client *client)
{
	struct lt9611c *lt9611c = i2c_get_clientdata(client);
	struct device *dev = lt9611c->dev;

	kfree(lt9611c->edid_buf);
	disable_irq(client->irq);
	lt9611c_cleanup_resources(lt9611c);
	mutex_destroy(&lt9611c->ocm_lock);
	dev_info(dev, "remove driver\n");
}

static int lt9611c_bridge_suspend(struct device *dev)
{
	struct lt9611c *lt9611c = dev_get_drvdata(dev);
	int ret;

	dev_info(lt9611c->dev, "suspend\n");
	disable_irq(lt9611c->client->irq);
	ret = lt9611c_regulator_disable(lt9611c);
	gpiod_set_value_cansleep(lt9611c->reset_gpio, 0);

	return ret;
}

static int lt9611c_bridge_resume(struct device *dev)
{
	struct lt9611c *lt9611c = dev_get_drvdata(dev);
	int ret;

	ret = lt9611c_regulator_enable(lt9611c);
	lt9611c_reset(lt9611c);
	enable_irq(lt9611c->client->irq);
	dev_info(lt9611c->dev, "resume\n");

	return ret;
}

static const struct dev_pm_ops lt9611c_bridge_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(lt9611c_bridge_suspend,
				lt9611c_bridge_resume)
};

static struct i2c_device_id lt9611c_id[] = {
	{ "lontium,lt9611c", 0 },
	{ /* sentinel */ }
};

static const struct of_device_id lt9611c_match_table[] = {
	{ .compatible = "lontium,lt9611c" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, lt9611c_match_table);

static struct i2c_driver lt9611c_driver = {
	.driver = {
		.name = "lt9611c",
		.of_match_table = lt9611c_match_table,
		.pm = &lt9611c_bridge_pm_ops,
		.dev_groups = lt9611c_attr_groups,
	},
	.probe = lt9611c_probe,
	.remove = lt9611c_remove,
	.id_table = lt9611c_id,
};
module_i2c_driver(lt9611c_driver);

MODULE_AUTHOR("syyang <syyang@lontium.com>");
MODULE_LICENSE("GPL v2");

MODULE_FIRMWARE(FW_FILE);
