// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * m24lr_ctl.c - Sysfs control interface for ST M24LR series RFID/NFC chips
 *
 * Copyright (c) 2025 Abd-Alrhman Masalkhi <abd.masalkhi@gmail.com>
 *
 * This driver implements a sysfs-based control interface for interacting with
 * STMicroelectronics M24LR series chips (e.g., M24LR04E-R). It enables access
 * to control registers for features such as password authentication, memory
 * access, and device configuration. It also synchronize access to the device
 * (the EEPROM uses a separate driver, see the note below for details)
 *
 * NOTE:
 * This driver provides only the control interface for M24LR chips. It acts
 * as an I2C mux (gate) for the EEPROM. Therefore, the EEPROM is represented
 * as a child node in the Device Tree and is accessed through a separate driver
 * (the standard 'at24' driver). This setup is possible because M24LR chips use
 * two I2C addresses: one for accessing the system parameter sector, and another
 * for accessing the EEPROM.
 */

#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/module.h>
#include <linux/i2c-mux.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/of_device.h>

#define M24LR_CTL_PAGESIZE_DEFAULT    1u

/*
 * Limits the number of I/O control bytes to 64 to prevent holding the
 * I2C bus for too long, especially important when operating at low I2C
 * frequencies
 */
#define M24LR_CTL_PAGESIZE_LIMIT      64u
#define M24LR_CTL_WRITE_TIMEOUT	      25u
#define M24LR_CTL_READ_TIMEOUT	      (M24LR_CTL_WRITE_TIMEOUT)

#define to_sys_entry(attrp)   container_of(attrp, struct m24lr_sys_entry, attr)

/**
 * struct m24lr_sys_entry - describes a sysfs entry for M24LR device access
 * @reg_addr:  register address in the M24LR device
 * @reg_size:  size of the register in bytes
 * @attr:      sysfs attribute used for exposing register access to userspace
 *
 * Used to define readable/writable register entries through sysfs.
 */
struct m24lr_sys_entry {
	unsigned int reg_addr;
	unsigned int reg_size;
	struct device_attribute attr;
};

/**
 * struct m24lr_ctl_chip - describes chip-specific sysfs layout
 * @entries:       array of sysfs entries specific to the chip variant
 * @n_entries:     number of entries in the array
 * @n_sss_entries: number of sss entries required for the chip
 *
 * Supports multiple M24LR chip variants (e.g., M24LRxx) by allowing each
 * to define its own set of sysfs attributes, depending on its available
 * registers and features.
 */
struct m24lr_ctl_chip {
	const struct m24lr_sys_entry *entries;
	unsigned int n_entries;
	unsigned int n_sss_entries;
};

/**
 * struct m24lr_ctl - core driver data for M24LR chip control
 * @page_size:     chip-specific limit on the maximum number of bytes allowed
 *                 in a single write operation.
 * @muxc:          mux core struct as the driver act as a gate for the eeprom
 * @regmap:        regmap interface for accessing chip registers
 * @gate_lock:     mutex to synchronize operations to the device from this
 *                 driver and the eeprom driver
 * @sss_entries:   array of sssc sysfs entries specific to the chip variant
 * @n_sss_entries: number of entries in the sss entries array
 *
 * Central data structure holding the state and resources used by the
 * M24LR device driver.
 */
struct m24lr_ctl {
	unsigned int page_size;
	struct regmap *regmap;
	struct i2c_mux_core *muxc;
	struct mutex gate_lock;  /* synchronize operations to the device */
	struct m24lr_sys_entry *sss_entries;
	unsigned int n_sss_entries;
};

static ssize_t m24lr_ctl_show(struct device *dev,
			      struct device_attribute *attr, char *buf);
static ssize_t m24lr_ctl_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t count);
static ssize_t m24lr04e_r_ctl_unlock_store(struct device *dev,
					   struct device_attribute *attr,
					   const char *buf, size_t count);
static ssize_t m24lr04e_r_ctl_newpass_store(struct device *dev,
					    struct device_attribute *attr,
					    const char *buf, size_t count);

static const struct regmap_range m24lr_ctl_vo_ranges[] = {
	regmap_reg_range(0, 63),
};

static const struct regmap_access_table m24lr_ctl_vo_table = {
	.yes_ranges = m24lr_ctl_vo_ranges,
	.n_yes_ranges = ARRAY_SIZE(m24lr_ctl_vo_ranges),
};

static const struct regmap_config m24lr_ctl_regmap_conf = {
	.name = "m24lr_ctl",
	.reg_stride = 1,
	.reg_bits = 16,
	.val_bits = 8,
	.disable_locking = false,
	.cache_type = REGCACHE_RBTREE,/* Flat can't be used, there's huge gap */
	.volatile_table = &m24lr_ctl_vo_table,
};

/* define the default sysfs entries specific to the M24LR */
static const struct m24lr_sys_entry m24lr_ctl_sys_entry_default_table[] = {
	{.attr = __ATTR(unlock, 0200, NULL, m24lr04e_r_ctl_unlock_store)},
	{.attr = __ATTR(new_pass, 0200, NULL, m24lr04e_r_ctl_newpass_store)},
	{.reg_addr = 2324, .reg_size = 8,
	 .attr = __ATTR(uid, 0444, m24lr_ctl_show, NULL)},
	{.reg_addr = 2334, .reg_size = 1,
	 .attr = __ATTR(mem_size, 0400, m24lr_ctl_show, NULL)},
};

/* Chip descriptor for M24LR04E-R variant */
static const struct m24lr_ctl_chip m24lr04e_r_chip = {
	.entries = m24lr_ctl_sys_entry_default_table,
	.n_entries = ARRAY_SIZE(m24lr_ctl_sys_entry_default_table),
	.n_sss_entries = 4,
};

/* Chip descriptor for M24LR16E-R variant */
static const struct m24lr_ctl_chip m24lr16e_r_chip = {
	.entries = m24lr_ctl_sys_entry_default_table,
	.n_entries = ARRAY_SIZE(m24lr_ctl_sys_entry_default_table),
	.n_sss_entries = 16,
};

/* Chip descriptor for M24LR64E-R variant */
static const struct m24lr_ctl_chip m24lr64e_r_chip = {
	.entries = m24lr_ctl_sys_entry_default_table,
	.n_entries = ARRAY_SIZE(m24lr_ctl_sys_entry_default_table),
	.n_sss_entries = 64,
};

static const struct i2c_device_id m24lr_ctl_ids[] = {
	{ "m24lr04e-r", (kernel_ulong_t)&m24lr04e_r_chip},
	{ "m24lr16e-r", (kernel_ulong_t)&m24lr16e_r_chip},
	{ "m24lr64e-r", (kernel_ulong_t)&m24lr64e_r_chip},
	{ }
};
MODULE_DEVICE_TABLE(i2c, m24lr_ctl_ids);

static const struct of_device_id m24lr_ctl_of_match[] = {
	{ .compatible = "st,m24lr04e-r", .data = &m24lr04e_r_chip},
	{ .compatible = "st,m24lr16e-r", .data = &m24lr16e_r_chip},
	{ .compatible = "st,m24lr64e-r", .data = &m24lr64e_r_chip},
	{ }
};
MODULE_DEVICE_TABLE(of, m24lr_ctl_of_match);

static int m24lr_ctl_gate_select(struct i2c_mux_core *muxc,
				 unsigned int chan_id)
{
	struct m24lr_ctl *ctl = i2c_mux_priv(muxc);

	mutex_lock(&ctl->gate_lock);

	return 0;
}

static int m24lr_ctl_gate_deselect(struct i2c_mux_core *muxc,
				   unsigned int chan_id)
{
	struct m24lr_ctl *ctl = i2c_mux_priv(muxc);

	mutex_unlock(&ctl->gate_lock);

	return 0;
}

/**
 * m24lr_parse_le_value - Parse hex string and convert to little-endian binary
 * @buf:        Input string buffer (hex format)
 * @reg_size:   Size of the register in bytes (must be 1, 2, 4, or 8)
 * @output:     Output buffer to store the value in little-endian format
 *
 * Converts a hexadecimal string to a numeric value of the given register size
 * and writes it in little-endian byte order into the provided buffer.
 *
 * Return: 0 on success, or negative error code on failure
 */
static int m24lr_parse_le_value(const char *buf, u32 reg_size, u8 *output)
{
	int err;

	switch (reg_size) {
	case 1: {
		u8 tmp;

		err = kstrtou8(buf, 16, &tmp);
		if (!err)
			*output = tmp;
		break;
	}
	case 2: {
		u16 tmp;

		err = kstrtou16(buf, 16, &tmp);
		if (!err)
			*(__le16 *)output = cpu_to_le16(tmp);
		break;
	}
	case 4: {
		u32 tmp;

		err = kstrtou32(buf, 16, &tmp);
		if (!err)
			*(__le32 *)output = cpu_to_le32(tmp);
		break;
	}
	case 8: {
		u64 tmp;

		err = kstrtou64(buf, 16, &tmp);
		if (!err)
			*(__le64 *)output = cpu_to_le64(tmp);
		break;
	}
	default:
		err = -EINVAL;
	}

	return err;
}

/**
 * m24lr_ctl_regmap_read - read data using regmap with retry on failure
 * @regmap:  regmap instance for the device
 * @buf:     buffer to store the read data
 * @size:    number of bytes to read
 * @offset:  starting register address
 *
 * Attempts to read a block of data from the device with retries and timeout.
 * Some M24LR chips may transiently NACK reads (e.g., during internal write
 * cycles), so this function retries with a short sleep until the timeout
 * expires.
 *
 * Returns:
 *       Number of bytes read on success,
 *       -ETIMEDOUT if the read fails within the timeout window.
 */
static ssize_t m24lr_ctl_regmap_read(struct regmap *regmap, u8 *buf,
				     size_t size, unsigned int offset)
{
	int err;
	unsigned long timeout, read_time;
	ssize_t ret = -ETIMEDOUT;

	timeout = jiffies + msecs_to_jiffies(M24LR_CTL_READ_TIMEOUT);
	do {
		read_time = jiffies;

		err = regmap_bulk_read(regmap, offset, buf, size);
		if (!err) {
			ret = size;
			break;
		}

		usleep_range(1000, 2000);
	} while (time_before(read_time, timeout));

	return ret;
}

/**
 * m24lr_ctl_regmap_write - write data using regmap with retry on failure
 * @regmap: regmap instance for the device
 * @buf:    buffer containing the data to write
 * @size:   number of bytes to write
 * @offset: starting register address
 *
 * Attempts to write a block of data to the device with retries and a timeout.
 * Some M24LR devices may NACK I2C writes while an internal write operation
 * is in progress. This function retries the write operation with a short delay
 * until it succeeds or the timeout is reached.
 *
 * Returns:
 *       Number of bytes written on success,
 *       -ETIMEDOUT if the write fails within the timeout window.
 */
static ssize_t m24lr_ctl_regmap_write(struct regmap *regmap, const u8 *buf,
				      size_t size, unsigned int offset)
{
	int err;
	unsigned long timeout, write_time;
	ssize_t ret = -ETIMEDOUT;

	timeout = jiffies + msecs_to_jiffies(M24LR_CTL_WRITE_TIMEOUT);

	do {
		write_time = jiffies;

		err = regmap_bulk_write(regmap, offset, buf, size);
		if (!err) {
			ret = size;
			break;
		}

		usleep_range(1000, 2000);
	} while (time_before(write_time, timeout));

	return ret;
}

static ssize_t m24lr_ctl_read(struct m24lr_ctl *ctl, u8 *buf,
			      size_t size, unsigned int offset)
{
	int ret = 0;
	struct regmap *regmap = ctl->regmap;

	if (unlikely(!size))
		return ret;

	m24lr_ctl_gate_select(ctl->muxc, 0);
	ret = m24lr_ctl_regmap_read(regmap, buf, size, offset);
	m24lr_ctl_gate_deselect(ctl->muxc, 0);

	return ret;
}

/**
 * m24lr_ctl_write - write buffer to M24LR device with page alignment handling
 * @ctl: pointer to driver context
 * @buf: data buffer to write
 * @size: number of bytes to write
 * @offset: target register address in the device
 *
 * Writes data to the M24LR device using regmap, split into chunks no larger
 * than page_size to respect device-specific write limitations (e.g., page
 * size or I2C hold-time concerns). Each chunk is aligned to the page boundary
 * defined by page_size.
 *
 * Returns:
 *       Total number of bytes written on success,
 *       A negative error code if any write fails.
 */
static ssize_t m24lr_ctl_write(struct m24lr_ctl *ctl, const u8 *buf,
			       size_t size, unsigned int offset)
{
	unsigned int n, next_sector;
	struct regmap *regmap = ctl->regmap;
	int err;
	ssize_t ret = 0;

	if (unlikely(!size))
		return ret;

	n = min(size, ctl->page_size);
	next_sector = roundup(offset + 1, ctl->page_size);
	if (offset + n > next_sector)
		n = next_sector - offset;

	m24lr_ctl_gate_select(ctl->muxc, 0);
	while (n) {
		err = m24lr_ctl_regmap_write(regmap, buf, n, offset);
		if (IS_ERR_VALUE(err)) {
			m24lr_ctl_gate_deselect(ctl->muxc, 0);
			if (ret)
				return ret;
			else
				return err;
		}

		offset += n;
		size -= n;
		ret += n;
		n = min(size, ctl->page_size);
	}
	m24lr_ctl_gate_deselect(ctl->muxc, 0);

	return ret;
}

/**
 * m24lr04e_r_ctl_write_pass - Write password to M24LR043-R using secure format
 * @ctl:   Pointer to device control structure
 * @buf:   Input buffer containing hex-encoded password
 * @count: Number of bytes in @buf
 * @code:  Operation code to embed between password copies
 *
 * This function parses a 4-byte password, encodes it in  big-endian format,
 * and constructs a 9-byte sequence of the form:
 *
 *        [BE(password), code, BE(password)]
 *
 * The result is written to register 0x0900 (2304), which is the password
 * register in M24LR04E-R chip.
 *
 * Return: Number of bytes written on success, or negative error code on failure
 */
static ssize_t m24lr04e_r_ctl_write_pass(struct m24lr_ctl *ctl, const char *buf,
					 size_t count, u8 code)
{
	int ret;
	u32 pass;
	__be32 be_pass;
	u8 output[9];

	ret = kstrtou32(buf, 16, &pass);
	if (ret)
		return ret;

	be_pass = cpu_to_be32(pass);

	memcpy(output, &be_pass, sizeof(be_pass));
	output[4] = code;
	memcpy(output + 5, &be_pass, sizeof(be_pass));

	m24lr_ctl_gate_select(ctl->muxc, 0);
	ret = m24lr_ctl_regmap_write(ctl->regmap, output, 9, 2304);
	m24lr_ctl_gate_deselect(ctl->muxc, 0);

	return ret;
}

static ssize_t m24lr04e_r_ctl_newpass_store(struct device *dev,
					    struct device_attribute *attr,
					    const char *buf, size_t count)
{
	struct m24lr_ctl *ctl = i2c_get_clientdata(to_i2c_client(dev));

	return m24lr04e_r_ctl_write_pass(ctl, buf, count, 7);
}

static ssize_t m24lr04e_r_ctl_unlock_store(struct device *dev,
					   struct device_attribute *attr,
					   const char *buf, size_t count)
{
	struct m24lr_ctl *ctl = i2c_get_clientdata(to_i2c_client(dev));

	return m24lr04e_r_ctl_write_pass(ctl, buf, count, 9);
}

static ssize_t m24lr_ctl_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct m24lr_ctl *ctl = i2c_get_clientdata(to_i2c_client(dev));
	struct m24lr_sys_entry *entry = to_sys_entry(attr);
	unsigned int reg_size = entry->reg_size;
	unsigned int reg_addr = entry->reg_addr;
	u8 output[8];
	int err = 0;

	if (unlikely(!is_power_of_2(reg_size) || reg_size > 8)) {
		dev_err(dev,
			"Invalid register size: must be a power of 2 and <= 8 bytes (%u)\n",
			reg_size);
		return -EIO;
	}

	err = m24lr_parse_le_value(buf, reg_size, output);
	if (err)
		return err;

	return m24lr_ctl_write(ctl, (u8 *)&output, reg_size, reg_addr);
}

static ssize_t m24lr_ctl_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	int ret;
	u64 val;
	__le64 input = 0;
	struct m24lr_ctl *ctl = i2c_get_clientdata(to_i2c_client(dev));
	struct m24lr_sys_entry *entry = to_sys_entry(attr);
	unsigned int reg_addr = entry->reg_addr;
	unsigned int reg_size = entry->reg_size;

	if (unlikely(!is_power_of_2(reg_size) || reg_size > 8)) {
		dev_dbg(dev,
			"Invalid register size: must be a power of 2 and <= 8 bytes (%u)\n",
			reg_size);
		return -EIO;
	}

	ret = m24lr_ctl_read(ctl, (u8 *)&input, reg_size, reg_addr);
	if (IS_ERR_VALUE(ret))
		return ret;

	if (ret != reg_size)
		return -EIO;

	switch (reg_size) {
	case 1:
		val = *(u8 *)&input;
		break;
	case 2:
		val = le16_to_cpu((__le16)input);
		break;
	case 4:
		val = le32_to_cpu((__le32)input);
		break;
	case 8:
		val = le64_to_cpu((__le64)input);
		break;
	};

	return scnprintf(buf, PAGE_SIZE, "%llx\n", val);
}

static const struct m24lr_ctl_chip *m24lr_ctl_get_chip(struct device *dev)
{
	const struct m24lr_ctl_chip *ret;
	const struct i2c_device_id *id;

	id = i2c_match_id(m24lr_ctl_ids, to_i2c_client(dev));

	if (dev->of_node && of_match_device(m24lr_ctl_of_match, dev))
		ret = of_device_get_match_data(dev);
	else if (id)
		ret = (void *)id->driver_data;
	else
		ret = acpi_device_get_match_data(dev);

	return ret;
}

static int m24lr_ctl_probe(struct i2c_client *client,
			   const struct i2c_device_id *id)
{
	struct regmap *regmap;
	struct m24lr_ctl *ctl;
	struct i2c_mux_core *muxc;
	const struct m24lr_ctl_chip *chip;
	struct m24lr_sys_entry *sss = NULL;
	unsigned int page_size;
	unsigned int n_sss;
	int i, err;
	u8 test;
	struct device *dev = &client->dev;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;

	chip = m24lr_ctl_get_chip(dev);
	if (!chip)
		return -ENODEV;

	ctl = devm_kzalloc(dev, sizeof(struct m24lr_ctl), GFP_KERNEL);
	if (!ctl)
		return -ENOMEM;

	err = device_property_read_u32(dev, "pagesize", &page_size);
	if (!err) {
		if (!is_power_of_2(page_size)) {
			dev_warn(dev,
				 "Invalid pagesize lenngth %d (not power of 2); using default %d byte\n",
				 page_size, M24LR_CTL_PAGESIZE_DEFAULT);
			page_size = M24LR_CTL_PAGESIZE_DEFAULT;
		}
		if (page_size > M24LR_CTL_PAGESIZE_LIMIT) {
			dev_info(dev,
				 "pagesize %d exceeds limit; rounded down to %d\n",
				 page_size, M24LR_CTL_PAGESIZE_LIMIT);
			page_size = M24LR_CTL_PAGESIZE_LIMIT;
		}
	} else { /* use the default */
		page_size = M24LR_CTL_PAGESIZE_DEFAULT;
	}

	for (i = 0; i < chip->n_entries; i++) {
		const struct device_attribute *attr = &chip->entries[i].attr;

		err = device_create_file(dev, attr);
		if (err)
			dev_warn(dev,
				 "Failed to create sysfs entry '%s'\n",
				 attr->attr.name, err);
	}

	n_sss = chip->n_sss_entries;
	if (n_sss) {
		sss = devm_kzalloc(dev, n_sss * sizeof(struct m24lr_sys_entry),
				   GFP_KERNEL);
		if (!sss)
			return -ENOMEM;

		for (i = 0; i < n_sss; i++) {
			char *name = kasprintf(GFP_KERNEL, "sss%02d", i);

			sss[i].reg_size = 1;
			sss[i].reg_addr = i;
			sss[i].attr.attr.name = name;
			sss[i].attr.attr.mode = 0600;
			sss[i].attr.show = m24lr_ctl_show;
			sss[i].attr.store = m24lr_ctl_store;

			err = device_create_file(dev, &sss[i].attr);
			if (err)
				dev_warn(dev,
					 "Failed to create sysfs entry '%s'\n",
					 name, err);
		}
	}

	regmap = devm_regmap_init_i2c(client, &m24lr_ctl_regmap_conf);
	if (IS_ERR(regmap)) {
		err = PTR_ERR(regmap);
		dev_err(dev, "Failed to init regmap (error: %d)\n", err);
		return err;
	}

	muxc = i2c_mux_alloc(client->adapter, &client->dev, 1, 0, I2C_MUX_GATE,
			     m24lr_ctl_gate_select, m24lr_ctl_gate_deselect);
	if (!muxc)
		return -ENOMEM;

	muxc->priv = ctl;

	mutex_init(&ctl->gate_lock);
	ctl->page_size = page_size;
	ctl->regmap = regmap;
	ctl->muxc = muxc;
	ctl->n_sss_entries = n_sss;
	ctl->sss_entries = sss;

	i2c_set_clientdata(client, ctl);

	err = m24lr_ctl_read(ctl, &test, 1, 0);
	if (IS_ERR_VALUE(err))
		return -ENODEV;

	err = i2c_mux_add_adapter(muxc, 0, 0, 0);
	if (err)
		return err;

	dev_info(&client->dev, "control interface initialized for %s\n",
		 client->name);

	return 0;
}

static int remove(struct i2c_client *client)
{
	struct m24lr_ctl *ctl = i2c_get_clientdata(client);

	i2c_mux_del_adapters(ctl->muxc);

	return 0;
}

static struct i2c_driver m24lr_ctl_driver = {
	.driver = {
		.name = "m24lr_ctl",
		.of_match_table = m24lr_ctl_of_match,
	},
	.probe    = m24lr_ctl_probe,
	.remove   = remove,
	.id_table = m24lr_ctl_ids,
};
module_i2c_driver(m24lr_ctl_driver);

MODULE_AUTHOR("Abd-Alrhman Masalkhi");
MODULE_DESCRIPTION("st m24lr control driver");
MODULE_LICENSE("GPL");
