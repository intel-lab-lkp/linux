// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Lontium Semiconductor, Inc.
 */
#include <linux/cleanup.h>
#include <linux/crc32.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_graph.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_drv.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_of.h>
#include <drm/drm_probe_helper.h>
#include <video/mipi_display.h>

#define FW_SIZE SZ_64K
#define LT_PAGE_SIZE 32
#define FW_FILE  "Lontium/lt7911exc_fw.bin"
#define LT7911EXC_PAGE_CONTROL 0xff

struct lt7911exc {
	struct device *dev;
	struct regmap *regmap;
	struct drm_bridge bridge;
	struct work_struct work;
	struct mipi_dsi_host dsi_host;
	struct drm_bridge *output_bridge;
	/* Fast lock: guards short register r/w and status checks */
	struct mutex ocm_lock;
	/* Long lock: serializes firmware upgrade process to prevent DRM interference */
	struct mutex upgrade_lock;
	struct gpio_desc *reset_gpio;
	u32 fw_version;
	bool upgrade;
	bool removed;
};

static const struct regmap_range_cfg lt7911exc_range_cfg[] = {
	{
		.name = "lt7911exc_i2c",
		.range_min =  0,
		.range_max = 0xe8ff,
		.selector_reg = LT7911EXC_PAGE_CONTROL,
		.selector_mask = 0xff,
		.selector_shift = 0,
		.window_start = 0,
		.window_len = 0x100,
	},
};

static const struct regmap_config lt7911exc_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0xe8ff,
	.ranges = lt7911exc_range_cfg,
	.num_ranges = ARRAY_SIZE(lt7911exc_range_cfg),
	.cache_type = REGCACHE_NONE,
};

static u32 cal_crc32_custom(const u8 *data, u64 length)
{
	u32 crc = 0xffffffff;
	u8 buf[4];
	u64 i;

	if (!length || (length & 3))
		return 0;

	for (i = 0; i < length; i += 4) {
		buf[0] = data[i + 3];
		buf[1] = data[i + 2];
		buf[2] = data[i + 1];
		buf[3] = data[i + 0];
		crc = crc32_be(crc, buf, 4);
	}

	return crc;
}

static inline struct lt7911exc *bridge_to_lt7911exc(struct drm_bridge *bridge)
{
	return container_of(bridge, struct lt7911exc, bridge);
}

static inline struct lt7911exc *dsi_host_to_lt7911exc(struct mipi_dsi_host *host)
{
	return container_of(host, struct lt7911exc, dsi_host);
}

static void lt7911exc_reset(struct lt7911exc *lt7911exc)
{
	/* Assert reset pin: logical 1 -> physical state low (Reset Active) */
	gpiod_set_value_cansleep(lt7911exc->reset_gpio, 1);
	usleep_range(5000, 6000);

	/* Deassert reset pin: logical 0 -> physical state high (Run state) */
	gpiod_set_value_cansleep(lt7911exc->reset_gpio, 0);
	msleep(400);
}

static int lt7911exc_inside_mcu_stop(struct lt7911exc *lt7911exc)
{
	int ret = regmap_write(lt7911exc->regmap, 0xe0ee, 0x01);

	if (ret)
		dev_err(lt7911exc->dev, "Failed to stop mcu\n");

	return ret;
}

static int lt7911exc_inside_mcu_run(struct lt7911exc *lt7911exc)
{
	int ret = regmap_write(lt7911exc->regmap, 0xe0ee, 0x00);

	if (ret)
		dev_err(lt7911exc->dev, "Failed to run mcu\n");

	return ret;
}

static int lt7911exc_regulator_init(struct lt7911exc *lt7911exc)
{
	int ret;

	ret = devm_regulator_get_enable(lt7911exc->dev, "vcc");
	if (ret < 0)
		return dev_err_probe(lt7911exc->dev, ret, "failed to enable vcc regulator\n");

	usleep_range(5000, 6000);

	ret = devm_regulator_get_enable(lt7911exc->dev, "vdd");
	if (ret < 0)
		return dev_err_probe(lt7911exc->dev, ret, "failed to enable vdd regulator\n");

	return 0;
}

static int lt7911exc_read_version(struct lt7911exc *lt7911exc, u32 *val)
{
	struct device *dev = lt7911exc->dev;
	int ret;
	u8 version[3];

	/* version register is accessible without stopping MCU */
	ret = regmap_bulk_read(lt7911exc->regmap, 0xe081, version, ARRAY_SIZE(version));
	if (ret) {
		dev_err(dev, "Failed to read version\n");
		return ret;
	}

	*val = (u32)version[0] << 16 | (u32)version[1] << 8 | version[2];

	return 0;
}

/* after erase, all data in the flash is 0xff */
static int lt7911exc_block_erase(struct lt7911exc *lt7911exc)
{
	struct device *dev = lt7911exc->dev;
	const u32 addr = 0x00;
	int ret;

	const struct reg_sequence seq_write[] = {
		REG_SEQ0(0xe0ee, 0x01),
		REG_SEQ0(0xe054, 0x01),
		REG_SEQ0(0xe055, 0x06),
		REG_SEQ0(0xe051, 0x01),
		REG_SEQ0(0xe051, 0x00),
		REG_SEQ0(0xe054, 0x05),
		REG_SEQ0(0xe055, 0xd8),
		REG_SEQ0(0xe05a, (addr >> 16) & 0xff),
		REG_SEQ0(0xe05b, (addr >> 8) & 0xff),
		REG_SEQ0(0xe05c, addr & 0xff),
		REG_SEQ0(0xe051, 0x01),
		REG_SEQ0(0xe050, 0x00),
	};

	ret = regmap_multi_reg_write(lt7911exc->regmap, seq_write, ARRAY_SIZE(seq_write));
	if (ret) {
		dev_err(dev, "Failed to erase flash.\n");
		return ret;
	}

	/* hardware need delay 200ms */
	msleep(200);

	return 0;
}

static int lt7911exc_prog_init(struct lt7911exc *lt7911exc, u64 addr)
{
	struct device *dev = lt7911exc->dev;
	int ret;

	const struct reg_sequence seq_write[] = {
		REG_SEQ0(0xe0ee, 0x01),
		REG_SEQ0(0xe05f, 0x01),
		REG_SEQ0(0xe05a, (addr >> 16) & 0xff),
		REG_SEQ0(0xe05b, (addr >> 8) & 0xff),
		REG_SEQ0(0xe05c, addr & 0xff),
	};

	ret = regmap_multi_reg_write(lt7911exc->regmap, seq_write, ARRAY_SIZE(seq_write));
	if (ret) {
		dev_err(dev, "Failed programe init.\n");
		return ret;
	}

	return 0;
}

/*
 * Hardware Flash Erase Behavior Note:
 * The LT7911EXC hardware erases the entire flash area into a default state of 0xff.
 * When doing a partial page short write (where fw->size is not aligned to a 32-byte
 * page boundary), the total unwritten residual bytes on the physical flash sector inherently
 * retain their 0xff state.
 * Therefore, raw physical short-write transmission is perfectly clean and matches the 0xff padded
 * software calculated CRC32. No software-side buffer padding is required.
 */
static int lt7911exc_write_data(struct lt7911exc *lt7911exc, const struct firmware *fw, u64 addr)
{
	struct device *dev = lt7911exc->dev;
	int ret;
	int page = 0, num = 0, page_len = 0;
	u64 size, offset;
	const u8 *data;

	data = fw->data;
	size = fw->size;
	page = (size + LT_PAGE_SIZE - 1) / LT_PAGE_SIZE;
	if (page * LT_PAGE_SIZE > FW_SIZE) {
		dev_err(dev, "firmware size out of range\n");
		return -EINVAL;
	}

	for (num = 0; num < page; num++) {
		offset = num * LT_PAGE_SIZE;
		page_len = (offset + LT_PAGE_SIZE <= size) ? LT_PAGE_SIZE : (size - offset);
		ret = lt7911exc_prog_init(lt7911exc, addr);
		if (ret)
			return ret;

		ret = regmap_raw_write(lt7911exc->regmap, 0xe05d, &data[offset], page_len);
		if (ret) {
			dev_err(dev, "write error at page %d\n", num);
			return ret;
		}

		/* write method for less than LT_PAGE_SIZE bytes */
		if (page_len < LT_PAGE_SIZE) {
			ret = regmap_write(lt7911exc->regmap, 0xe05f, 0x05);
			if (ret)
				return ret;

			ret = regmap_write(lt7911exc->regmap, 0xe05f, 0x01);
			if (ret)
				return ret;

			/* hardware requires delay */
			usleep_range(1000, 2000);
		}

		ret = regmap_write(lt7911exc->regmap, 0xe05f, 0x00);
		if (ret)
			return ret;

		addr += LT_PAGE_SIZE;
	}

	return 0;
}

static int lt7911exc_write_crc(struct lt7911exc *lt7911exc, u32 crc32, u64 addr)
{
	struct device *dev = lt7911exc->dev;
	int ret;

	const struct reg_sequence seq_write[] = {
		REG_SEQ0(0xe05f, 0x01),
		REG_SEQ0(0xe05a, (addr >> 16) & 0xff),
		REG_SEQ0(0xe05b, (addr >> 8) & 0xff),
		REG_SEQ0(0xe05c, addr & 0xff),
		REG_SEQ0(0xe05d, crc32 & 0xff),
		REG_SEQ0(0xe05d, (crc32 >> 8) & 0xff),
		REG_SEQ0(0xe05d, (crc32 >> 16) & 0xff),
		REG_SEQ0(0xe05d, (crc32 >> 24) & 0xff),
		REG_SEQ0(0xe05f, 0x05),
		REG_SEQ(0xe05f, 0x01, 1000), /* hardware requires delay */
		REG_SEQ0(0xe05f, 0x00),
	};

	ret = regmap_multi_reg_write(lt7911exc->regmap, seq_write, ARRAY_SIZE(seq_write));
	if (ret) {
		dev_err(dev, "Failed to write crc\n");
		return ret;
	}

	return 0;
}

static int lt7911exc_upgrade_result(struct lt7911exc *lt7911exc, u32 crc32)
{
	struct device *dev = lt7911exc->dev;
	u32 read_hw_crc;
	u8 crc_tmp[4];
	int ret;

	const struct reg_sequence seq_write[] = {
		REG_SEQ0(0xe0ee, 0x01),
		REG_SEQ0(0xe07b, 0x60),
		REG_SEQ0(0xe07b, 0x40),
	};

	ret = regmap_multi_reg_write(lt7911exc->regmap, seq_write, ARRAY_SIZE(seq_write));
	if (ret) {
		dev_err(dev, "Failed to setup CRC read\n");
		return ret;
	}

	msleep(150);

	ret = regmap_bulk_read(lt7911exc->regmap, 0x22, crc_tmp, ARRAY_SIZE(crc_tmp));
	if (ret) {
		dev_err(dev, "Failed to read CRC: %d\n", ret);
		return ret;
	}

	ret = regmap_write(lt7911exc->regmap, 0xe0ee, 0x00);
	if (ret) {
		dev_err(dev, "Failed to restore MCU state\n");
		return ret;
	}

	read_hw_crc = ((u32)crc_tmp[0] << 24) | ((u32)crc_tmp[1] << 16) |
				((u32)crc_tmp[2] << 8) | crc_tmp[3];

	if (read_hw_crc != crc32) {
		dev_err(dev, "lt7911exc firmware upgrade failed, expected CRC=0x%08x, read CRC=0x%08x\n",
			crc32, read_hw_crc);
		return -EIO;
	}

	return 0;
}

static void lt7911exc_firmware_upgrade_work(struct work_struct *work)
{
	struct lt7911exc *lt7911exc = container_of(work, struct lt7911exc, work);
	struct device *dev = lt7911exc->dev;
	const struct firmware *fw;
	u8 *buffer;
	size_t total_size = FW_SIZE - 4;
	u32 crc32, version;
	int ret;

	mutex_lock(&lt7911exc->upgrade_lock);
	if (lt7911exc->removed)
		goto out_unlock_upgrade;

	ret = request_firmware(&fw, FW_FILE, dev);
	if (ret) {
		dev_err(dev, "failed to load '%s'\n", FW_FILE);
		goto out_unlock_upgrade;
	}

	if (fw->size > total_size) {
		dev_err(dev, "firmware too large (%zu > %zu)\n", fw->size, total_size);
		goto out_release_fw;
	}

	buffer = kvmalloc(total_size, GFP_KERNEL);
	if (!buffer)
		goto out_release_fw;

	memset(buffer, 0xff, total_size);
	memcpy(buffer, fw->data, fw->size);
	crc32 = cal_crc32_custom(buffer, total_size);
	kvfree(buffer);

	lt7911exc_reset(lt7911exc);

	scoped_guard(mutex, &lt7911exc->ocm_lock) {
		ret = lt7911exc_inside_mcu_stop(lt7911exc);
	}
	if (ret)
		goto out_release_fw;

	ret = lt7911exc_block_erase(lt7911exc);
	if (ret) {
		dev_err(dev, "failed to block erase.\n");
		goto out_mcu_run;
	}

	ret = lt7911exc_write_data(lt7911exc, fw, 0);
	if (ret < 0) {
		dev_err(dev, "failed to write firmware data\n");
		goto out_mcu_run;
	}

	ret = lt7911exc_write_crc(lt7911exc, crc32, FW_SIZE - 4);
	if (ret < 0) {
		dev_err(dev, "failed to write firmware crc\n");
		goto out_mcu_run;
	}

	lt7911exc_reset(lt7911exc);

	ret = lt7911exc_upgrade_result(lt7911exc, crc32);
	if (ret)
		dev_err(dev, "firmware verification failed\n");

	/*
	 * Always restore MCU to running state: flash is already erased,
	 * leaving MCU stopped would render the chip completely unusable.
	 */
	scoped_guard(mutex, &lt7911exc->ocm_lock) {
		ret = lt7911exc_inside_mcu_run(lt7911exc);
		if (!ret) {
			ret = lt7911exc_read_version(lt7911exc, &version);
			if (!ret)
				lt7911exc->fw_version = version;
		}
	}

	if (ret)
		dev_err(dev, "failed to read version after upgrade\n");

	goto out_release_fw;

out_mcu_run:
	scoped_guard(mutex, &lt7911exc->ocm_lock)
		lt7911exc_inside_mcu_run(lt7911exc);

out_release_fw:
	release_firmware(fw);

out_unlock_upgrade:
	scoped_guard(mutex, &lt7911exc->ocm_lock) {
		if (!lt7911exc->removed)
			lt7911exc->upgrade = false;
	}

	/* Notify DRM to re-trigger modeset after firmware upgrade */
	if (!lt7911exc->removed && lt7911exc->bridge.dev)
		drm_kms_helper_hotplug_event(lt7911exc->bridge.dev);

	mutex_unlock(&lt7911exc->upgrade_lock);
}

static void lt7911exc_atomic_pre_enable(struct drm_bridge *bridge, struct drm_atomic_commit *state)
{
	struct lt7911exc *lt7911exc = bridge_to_lt7911exc(bridge);
	struct device *dev = lt7911exc->dev;
	int ret;

	guard(mutex)(&lt7911exc->ocm_lock);

	if (!lt7911exc->upgrade) {
		ret = regmap_write(lt7911exc->regmap, 0xe0b0, 0x01);
		if (ret)
			dev_err(dev, "failed to enable mipi output stream\n");
	}
}

static void lt7911exc_atomic_post_disable(struct drm_bridge *bridge,
					  struct drm_atomic_commit *state)
{
	struct lt7911exc *lt7911exc = bridge_to_lt7911exc(bridge);
	struct device *dev = lt7911exc->dev;
	int ret;

	guard(mutex)(&lt7911exc->ocm_lock);

	if (!lt7911exc->upgrade) {
		ret = regmap_write(lt7911exc->regmap, 0xe0b0, 0x00);
		if (ret)
			dev_err(dev, "failed to disable mipi output stream\n");
	}
}

static int lt7911exc_bridge_attach(struct drm_bridge *bridge,
				   struct drm_encoder *encoder,
				   enum drm_bridge_attach_flags flags)
{
	struct lt7911exc *lt7911exc = bridge_to_lt7911exc(bridge);

	if (!drm_core_check_feature(bridge->dev, DRIVER_ATOMIC)) {
		dev_err(lt7911exc->dev, "needs atomic updates support\n");
		return -ENOTSUPP;
	}

	return drm_bridge_attach(encoder, lt7911exc->output_bridge, bridge, flags);
}

static const struct drm_bridge_funcs lt7911exc_bridge_funcs = {
	.attach = lt7911exc_bridge_attach,
	.atomic_pre_enable = lt7911exc_atomic_pre_enable,
	.atomic_post_disable = lt7911exc_atomic_post_disable,
	.atomic_reset = drm_atomic_helper_bridge_reset,
	.atomic_duplicate_state = drm_atomic_helper_bridge_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_bridge_destroy_state,
};

static int lt7911exc_dsi_host_attach(struct mipi_dsi_host *host, struct mipi_dsi_device *dev)
{
	struct lt7911exc *lt7911exc = dsi_host_to_lt7911exc(host);
	struct drm_bridge *bridge;

	/* currently do not support connecting several DSI devices to the same host */
	if (lt7911exc->output_bridge)
		return -EBUSY;

	if (dev->lanes > 4) {
		dev_err(lt7911exc->dev, "unsupported number of data lanes(%u)\n", dev->lanes);
		return -EINVAL;
	}

	bridge = devm_drm_of_get_bridge(lt7911exc->dev, host->dev->of_node, 1, 0);
	if (IS_ERR(bridge)) {
		dev_err(lt7911exc->dev, "failed to add DSI device\n");
		return PTR_ERR(bridge);
	}

	lt7911exc->output_bridge = bridge;

	/*
	 * Force panel-first enable order: the DSI output stream (0xe0b0)
	 * must not be enabled before the panel is powered on.
	 * This override is safe because LT7911EXC's transfer() is a stub —
	 * panel DCS commands are silently discarded and panel init is done
	 * by the chip's internal firmware. The panel driver's
	 * prepare_prev_first preference is therefore irrelevant here.
	 */
	lt7911exc->output_bridge->pre_enable_prev_first = false;

	drm_bridge_add(&lt7911exc->bridge);

	return 0;
}

static int lt7911exc_dsi_host_detach(struct mipi_dsi_host *host, struct mipi_dsi_device *dev)
{
	struct lt7911exc *lt7911exc = dsi_host_to_lt7911exc(host);

	drm_bridge_remove(&lt7911exc->bridge);

	return 0;
}

/*
 * LT7911EXC's internal MCU owns the DSI link and handles all panel
 * initialization. The host transfer() is a no-op sink: accept the
 * message and report it as sent so the panel driver does not abort
 * its init sequence. Actual DSI transmission is done by chip firmware.
 */
static ssize_t lt7911exc_dsi_host_transfer(struct mipi_dsi_host *host,
					   const struct mipi_dsi_msg *msg)
{
	struct lt7911exc *lt7911exc = dsi_host_to_lt7911exc(host);

	if (msg->rx_len) {
		dev_warn(lt7911exc->dev, "MIPI rx is not supported\n");
		return -EOPNOTSUPP;
	}

	switch (msg->type) {
	case MIPI_DSI_DCS_SHORT_WRITE:
	case MIPI_DSI_DCS_SHORT_WRITE_PARAM:
	case MIPI_DSI_DCS_LONG_WRITE:
	case MIPI_DSI_GENERIC_SHORT_WRITE_0_PARAM:
	case MIPI_DSI_GENERIC_SHORT_WRITE_1_PARAM:
	case MIPI_DSI_GENERIC_SHORT_WRITE_2_PARAM:
	case MIPI_DSI_GENERIC_LONG_WRITE:
		break;
	default:
		return -EOPNOTSUPP;
	}

	guard(mutex)(&lt7911exc->ocm_lock);

	if (lt7911exc->upgrade)
		return -EBUSY;

	return msg->tx_len;
}

static const struct mipi_dsi_host_ops lt7911exc_dsi_host_ops = {
	.attach = lt7911exc_dsi_host_attach,
	.detach = lt7911exc_dsi_host_detach,
	.transfer = lt7911exc_dsi_host_transfer,
};

static ssize_t lt7911exc_firmware_store(struct device *dev, struct device_attribute *attr,
					const char *buf, size_t len)
{
	struct lt7911exc *lt7911exc = dev_get_drvdata(dev);
	bool lt7911exc_busy = true;

	if (!lt7911exc)
		return -ENODEV;

	if (!mutex_trylock(&lt7911exc->upgrade_lock))
		return -EBUSY;

	scoped_guard(mutex, &lt7911exc->ocm_lock) {
		if (!lt7911exc->upgrade && !lt7911exc->removed) {
			lt7911exc->upgrade = true;
			lt7911exc_busy = false;
		}
	}
	mutex_unlock(&lt7911exc->upgrade_lock);

	if (lt7911exc_busy)
		return -EBUSY;

	schedule_work(&lt7911exc->work);

	return len;
}

static ssize_t lt7911exc_firmware_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct lt7911exc *lt7911exc = dev_get_drvdata(dev);
	u32 version;

	if (!lt7911exc)
		return -ENODEV;

	guard(mutex)(&lt7911exc->ocm_lock);
	version = lt7911exc->fw_version;

	return sysfs_emit(buf, "0x%04x\n", version);
}

static DEVICE_ATTR_RW(lt7911exc_firmware);

static struct attribute *lt7911exc_attrs[] = {
	&dev_attr_lt7911exc_firmware.attr,
	NULL,
};

static const struct attribute_group lt7911exc_attr_group = {
	.attrs = lt7911exc_attrs,
};

static const struct attribute_group *lt7911exc_attr_groups[] = {
	&lt7911exc_attr_group,
	NULL,
};

static int lt7911exc_probe(struct i2c_client *client)
{
	struct lt7911exc *lt7911exc;
	struct device *dev = &client->dev;
	struct device_node *np = dev->of_node;
	u32 version;
	int ret;

	if (!np)
		return -ENODEV;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return dev_err_probe(dev, -ENODEV, "device doesn't support I2C\n");

	lt7911exc = devm_drm_bridge_alloc(dev, struct lt7911exc, bridge, &lt7911exc_bridge_funcs);
	if (IS_ERR(lt7911exc))
		return dev_err_probe(dev, PTR_ERR(lt7911exc), "drm bridge alloc failed.\n");

	dev_set_drvdata(dev, lt7911exc);

	lt7911exc->dev = dev;
	lt7911exc->upgrade = false;
	lt7911exc->removed = false;

	ret = devm_mutex_init(dev, &lt7911exc->ocm_lock);
	if (ret)
		return dev_err_probe(dev, ret, "failed to init mutex\n");

	ret = devm_mutex_init(dev, &lt7911exc->upgrade_lock);
	if (ret)
		return dev_err_probe(dev, ret, "failed to init upgrade_lock\n");

	lt7911exc->regmap = devm_regmap_init_i2c(client, &lt7911exc_regmap_config);
	if (IS_ERR(lt7911exc->regmap))
		return dev_err_probe(dev, PTR_ERR(lt7911exc->regmap), "regmap i2c init failed\n");

	/* DT reset pin Active-low: assert (logical 1) to hold chip in reset */
	lt7911exc->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(lt7911exc->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(lt7911exc->reset_gpio),
				     "failed to acquire reset gpio\n");

	ret = lt7911exc_regulator_init(lt7911exc);
	if (ret)
		return ret;

	lt7911exc_reset(lt7911exc);

	scoped_guard(mutex, &lt7911exc->ocm_lock)
		ret = lt7911exc_read_version(lt7911exc, &version);

	if (ret < 0)
		return dev_err_probe(dev, ret, "failed read version of chip\n");

	lt7911exc->fw_version = version;

	lt7911exc->dsi_host.dev = dev;
	lt7911exc->dsi_host.ops = &lt7911exc_dsi_host_ops;
	lt7911exc->bridge.of_node = np;

	INIT_WORK(&lt7911exc->work, lt7911exc_firmware_upgrade_work);

	ret = mipi_dsi_host_register(&lt7911exc->dsi_host);
	if (ret)
		return ret;

	return 0;
}

static void lt7911exc_remove(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct lt7911exc *lt7911exc = dev_get_drvdata(dev);

	if (!lt7911exc)
		return;

	scoped_guard(mutex, &lt7911exc->ocm_lock) {
		lt7911exc->removed = true;
		lt7911exc->upgrade = true;
	}

	cancel_work_sync(&lt7911exc->work);

	dev_set_drvdata(dev, NULL);

	mipi_dsi_host_unregister(&lt7911exc->dsi_host);

	gpiod_set_value_cansleep(lt7911exc->reset_gpio, 1);
}

static const struct i2c_device_id lt7911exc_i2c_table[] = {
	{"lt7911exc"},
	{/* sentinel */}
};

MODULE_DEVICE_TABLE(i2c, lt7911exc_i2c_table);

static const struct of_device_id lt7911exc_devices[] = {
	{.compatible = "lontium,lt7911exc"},
	{/* sentinel */}
};
MODULE_DEVICE_TABLE(of, lt7911exc_devices);

static struct i2c_driver lt7911exc_driver = {
	.id_table	= lt7911exc_i2c_table,
	.probe		= lt7911exc_probe,
	.remove		= lt7911exc_remove,
	.driver		= {
		.name	= "lt7911exc",
		.of_match_table = lt7911exc_devices,
		.dev_groups = lt7911exc_attr_groups,
	},
};
module_i2c_driver(lt7911exc_driver);

MODULE_AUTHOR("SunYun Yang <syyang@lontium.com>");
MODULE_DESCRIPTION("Lontium LT7911EXC EDP to MIPI DSI bridge driver");
MODULE_LICENSE("GPL");
MODULE_FIRMWARE(FW_FILE);
