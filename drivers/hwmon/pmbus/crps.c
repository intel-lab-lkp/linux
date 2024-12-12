// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright 2024 IBM Corp.
 */

#include <linux/debugfs.h>
#include <linux/i2c.h>
#include <linux/of.h>
#include <linux/pmbus.h>

#include "pmbus.h"

/* Intel crps185 specific commands. */
#define CRPS185_MFR_IOUT_MAX		0xA6
#define CRPS185_MFR_POUT_MAX		0xA7

enum {
	CRPS_DEBUGFS_PMBUS_REVISION = 0,
	CRPS_DEBUGFS_MAX_POWER_OUT,
	CRPS_DEBUGFS_MAX_CURRENT_OUT,
	CRPS_DEBUGFS_NUM_ENTRIES
};

enum models { crps185 = 1, crps_unknown };

struct crps {
	enum models version;
	struct i2c_client *client;

	int debugfs_entries[CRPS_DEBUGFS_NUM_ENTRIES];
};

#define to_psu(x, y) container_of((x), struct crps, debugfs_entries[(y)])

static struct pmbus_platform_data crps_pdata = {
	.flags = PMBUS_SKIP_STATUS_CHECK,
};

static const struct i2c_device_id crps_id[] = {
	{ "intel_crps185", crps185 },
	{}
};
MODULE_DEVICE_TABLE(i2c, crps_id);

/*
 * Convert linear format word to machine format. 11 LSB side bits are two's
 * complement integer mantissa and 5 MSB side bits are two's complement
 * exponent
 */
static int crps_convert_linear(int rc)
{
	s16 exponent;
	s32 mantissa;
	s64 val;

	exponent = ((s16)rc) >> 11;
	mantissa = ((s16)((rc & 0x7ff) << 5)) >> 5;

	val = mantissa;
	if (exponent >= 0)
		val <<= exponent;
	else
		val >>= -exponent;

	return (int)val;
}

static ssize_t crps_debugfs_read(struct file *file, char __user *buf,
				 size_t count, loff_t *ppos)
{
	int rc;
	int *idxp = file->private_data;
	int idx = *idxp;
	struct crps *psu = to_psu(idxp, idx);
	char data[2 * I2C_SMBUS_BLOCK_MAX] = { 0 };

	rc = pmbus_lock_interruptible(psu->client);
	if (rc)
		return rc;

	rc = pmbus_set_page(psu->client, 0, 0xff);
	if (rc)
		goto unlock;

	switch (idx) {
	case CRPS_DEBUGFS_PMBUS_REVISION:
		rc = i2c_smbus_read_byte_data(psu->client, PMBUS_REVISION);
		if (rc >= 0) {
			if (psu->version == crps185) {
				if (rc == 0)
					rc = sprintf(data, "%s", "1.0");
				else if (rc == 0x11)
					rc = sprintf(data, "%s", "1.1");
				else if (rc == 0x22)
					rc = sprintf(data, "%s", "1.2");
				else
					rc = snprintf(data, 3, "0x%02x", rc);
			} else {
				rc = snprintf(data, 3, "%02x", rc);
			}
		}
		break;
	case CRPS_DEBUGFS_MAX_POWER_OUT:
		rc = i2c_smbus_read_word_data(psu->client, PMBUS_MFR_POUT_MAX);
		if (rc >= 0) {
			rc = crps_convert_linear(rc);
			rc = snprintf(data, I2C_SMBUS_BLOCK_MAX, "%d", rc);
		}
		break;
	case CRPS_DEBUGFS_MAX_CURRENT_OUT:
		rc = i2c_smbus_read_word_data(psu->client, PMBUS_MFR_IOUT_MAX);
		if (rc >= 0) {
			rc = crps_convert_linear(rc);
			rc = snprintf(data, I2C_SMBUS_BLOCK_MAX, "%d", rc);
		}
		break;
	default:
		rc = -EINVAL;
		break;
	}

unlock:
	pmbus_unlock(psu->client);
	if (rc < 0)
		return rc;

	data[rc] = '\n';
	rc += 2;

	return simple_read_from_buffer(buf, count, ppos, data, rc);
}

static const struct file_operations crps_debugfs_fops = {
	.llseek = noop_llseek,
	.read = crps_debugfs_read,
	.open = simple_open,
};

static int crps_read_word_data(struct i2c_client *client, int page,
				int phase, int reg)
{
	int rc;

	switch (reg) {
	case PMBUS_STATUS_WORD:
		rc = pmbus_read_word_data(client, page, phase, reg);
		if (rc < 0)
			return rc;
		break;
	case PMBUS_OT_WARN_LIMIT:
		rc = pmbus_read_word_data(client, page, phase,
					  PMBUS_MFR_MAX_TEMP_1);
		if (rc < 0)
			return rc;
		break;
	case PMBUS_IOUT_OC_WARN_LIMIT:
		rc = pmbus_read_word_data(client, page, phase,
					  CRPS185_MFR_IOUT_MAX);
		if (rc < 0)
			return rc;
		break;
	case PMBUS_POUT_OP_WARN_LIMIT:
		rc = pmbus_read_word_data(client, page, phase,
					  CRPS185_MFR_POUT_MAX);
		if (rc < 0)
			return rc;
		break;
	default:
		rc = -ENODATA;
		break;
	}

	return rc;
}

static struct pmbus_driver_info crps_info[] = {
	[crps185] = {
		.pages = 1,
		/* PSU uses default linear data format. */
		.func[0] = PMBUS_HAVE_PIN | PMBUS_HAVE_IOUT |
			PMBUS_HAVE_STATUS_IOUT | PMBUS_HAVE_IIN |
			PMBUS_HAVE_VIN | PMBUS_HAVE_STATUS_INPUT |
			PMBUS_HAVE_VOUT | PMBUS_HAVE_STATUS_VOUT |
			PMBUS_HAVE_TEMP | PMBUS_HAVE_TEMP2 |
			PMBUS_HAVE_STATUS_TEMP |
			PMBUS_HAVE_FAN12 | PMBUS_HAVE_STATUS_FAN12,
		.read_word_data = crps_read_word_data,
	},
};

#define to_psu(x, y) container_of((x), struct crps, debugfs_entries[(y)])

static void crps_init_debugfs(struct crps *psu)
{
	struct i2c_client *client = psu->client;
	struct dentry *debugfs;
	int i;

	/* Don't fail the probe if we can't create debugfs */
	debugfs = pmbus_get_debugfs_dir(client);
	if (!debugfs)
		return;

	for (i = 0; i < CRPS_DEBUGFS_NUM_ENTRIES; ++i)
		psu->debugfs_entries[i] = i;

	debugfs_create_file("pmbus_revision", 0444, debugfs,
			    &psu->debugfs_entries[CRPS_DEBUGFS_PMBUS_REVISION],
			    &crps_debugfs_fops);
	debugfs_create_file("max_power_out", 0444, debugfs,
			    &psu->debugfs_entries[CRPS_DEBUGFS_MAX_POWER_OUT],
			    &crps_debugfs_fops);
	debugfs_create_file("max_current_out", 0444, debugfs,
			    &psu->debugfs_entries[CRPS_DEBUGFS_MAX_CURRENT_OUT],
			    &crps_debugfs_fops);
}

static int crps_probe(struct i2c_client *client)
{
	int rc;
	struct device *dev = &client->dev;
	enum models vs = crps_unknown;
	struct crps *psu;
	const void *md = of_device_get_match_data(&client->dev);
	const struct i2c_device_id *id = NULL;
	char buf[I2C_SMBUS_BLOCK_MAX + 2] = { 0 };

	if (md) {
		vs = (uintptr_t)md;
	} else {
		id = i2c_match_id(crps_id, client);
		if (id)
			vs = (enum models)id->driver_data;
	}

	if (!vs || vs >= crps_unknown) {
		dev_err(dev, "Version %d not supported\n", vs);
		return -EINVAL;
	}

	rc = i2c_smbus_read_block_data(client, PMBUS_MFR_MODEL, buf);
	if (rc < 0) {
		dev_err(dev, "Failed to read PMBUS_MFR_MODEL\n");
		return rc;
	}
	if (strncmp(buf, "03NK260", 7)) {
		buf[rc] = '\0';
		dev_err(dev, "Model '%s' not supported\n", buf);
		return -ENODEV;
	}

	client->dev.platform_data = &crps_pdata;
	rc = pmbus_do_probe(client, &crps_info[vs]);
	if (rc) {
		dev_err(dev, "Failed to probe %d\n", rc);
		return rc;
	}

	/*
	 * Don't fail the probe if there isn't enough memory for debugfs.
	 */
	psu = devm_kzalloc(&client->dev, sizeof(*psu), GFP_KERNEL);
	if (!psu) {
		dev_warn(dev, "Failed to allocate memory. debugfs are not supported.\n");
		return 0;
	}

	psu->version = vs;
	psu->client = client;

	crps_init_debugfs(psu);

	return 0;
}

static const struct of_device_id crps_of_match[] = {
	{
		.compatible = "intel,crps185",
		.data = (void *)crps185
	},
	{}
};
MODULE_DEVICE_TABLE(of, crps_of_match);

static struct i2c_driver crps_driver = {
	.driver = {
		.name = "crps",
		.of_match_table = crps_of_match,
	},
	.probe = crps_probe,
	.id_table = crps_id,
};

module_i2c_driver(crps_driver);

MODULE_AUTHOR("Ninad Palsule");
MODULE_DESCRIPTION("PMBus driver for Common Redundant power supplies");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("PMBUS");
