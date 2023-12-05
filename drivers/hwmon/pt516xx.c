// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/hwmon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_device.h>

/** Main-micro-assisted access command codes */
// Wide register reads
#define ARIES_MM_RD_WIDE_REG_2B 0x1d
#define ARIES_MM_RD_WIDE_REG_3B 0x1e
#define ARIES_MM_RD_WIDE_REG_4B 0x1f
#define ARIES_MM_RD_WIDE_REG_5B 0x20
// Wide register writes
#define ARIES_MM_WR_WIDE_REG_2B 0x21
#define ARIES_MM_WR_WIDE_REG_3B 0x22
#define ARIES_MM_WR_WIDE_REG_4B 0x23
#define ARIES_MM_WR_WIDE_REG_5B 0x24

/** Temperature measurement constants */
// Aries current average temp ADC code CSR
#define ARIES_CURRENT_AVG_TEMP_ADC_CSR 0x42c

// Main Micro Heartbeat reg
#define ARIES_MM_HEARTBEAT_ADDR 0x923

/** Main SRAM */
// AL Main SRAM DMEM offset (A0)
#define AL_MAIN_SRAM_DMEM_OFFSET (64 * 1024)
// SRAM read command
#define AL_TG_RD_LOC_IND_SRAM 0x16

/** Micros */
// Offset for main micro FW info
#define ARIES_MAIN_MICRO_FW_INFO (96 * 1024 - 128)

/** Path Micro Members */
// FW Info (Major) offset location in struct
#define ARIES_MM_FW_VERSION_MAJOR 0
// FW Info (Minor) offset location in struct
#define ARIES_MM_FW_VERSION_MINOR 1
// FW Info (Build no.) offset location in struct
#define ARIES_MM_FW_VERSION_BUILD 2

/** Offsets for MM assisted accesses */
// Legacy Address and Data registers (using wide registers)
// Reg offset to specify Address for MM assisted accesses
#define ARIES_MM_ASSIST_REG_ADDR_OFFSET 0xd99
// Reg offset to specify Command
#define ARIES_MM_ASSIST_CMD_OFFSET 0xd9d

// New Address and Data registers (not using wide registers)
// Reg offset to MM SPARE 0 used specify Address[7:0]
#define ARIES_MM_ASSIST_SPARE_0_OFFSET 0xd9f
// Reg offset to MM SPARE 3 used specify Data Byte 0
#define ARIES_MM_ASSIST_SPARE_3_OFFSET 0xda2

/** Misc block offsets */
// Device Load check register
#define ARIES_CODE_LOAD_REG 0x605

/** Value indicating FW was loaded properly */
#define ARIES_LOAD_CODE 0xe

/** EEPROM parameters */
// Time delay between checking MM status of EEPROM write (microseconds)
#define ARIES_MM_STATUS_TIME 5000

#define ARIES_TEMP_CAL_CODE_DEFAULT 84

/* Struct defining FW version loaded on an Aries device */
struct pt516xx_fw_ver {
	u8 major; // FW version major release value
	u8 minor; // FW version minor release value
	u16 build; // FW version build release value
};

/* Each client has this additional data */
struct pt516xx_data {
	struct i2c_client *client;
	struct dentry *debugfs;
	struct pt516xx_fw_ver fw_ver;
	struct mutex lock;
	bool init_done;
	bool pec_enable; // Enable PEC
	bool code_load_okay; // indicate if code load reg value is expected
	bool mm_heartbeat_okay; // indicate if Main Micro heartbeat is good
	bool mm_wide_reg_valid; // MM assisted Wide Register access
	u8 temp_cal_code_avg;
};

static struct dentry *pt516xx_debugfs_dir;

/*
 * Write multiple data bytes to Aries over I2C
 */
static int pt516xx_write_block_data(struct pt516xx_data *data, u32 address,
				    u8 len, u8 *val)
{
	struct i2c_client *client = data->client;
	int ret;
	u8 remain_len = len;
	u8 xfer_len, curr_len;
	u8 buf[16];
	u8 cmd = 0x0F; // [7]:pec_en, [6:5]:rsvd, [4:2]:func, [1]:start, [0]:end
	u8 config = 0x40; // [6]:cfg_type, [4:1]:burst_len, [0]:bit16 of address

	if (data->pec_enable)
		cmd |= 0x80;

	// If byte count is greater than 4, perform multiple iterations
	while (remain_len > 0) {
		if (remain_len > 4) {
			curr_len = 4;
			remain_len -= 4;
		} else {
			curr_len = remain_len;
			remain_len = 0;
		}

		buf[0] = config | (curr_len - 1) << 1 | ((address >> 16) & 0x1);
		buf[1] = (address >> 8) & 0xff;
		buf[2] = address & 0xff;
		memcpy(&buf[3], val, curr_len);

		xfer_len = 3 + curr_len;
		ret = i2c_smbus_write_block_data(client, cmd, xfer_len, buf);
		if (ret)
			return ret;

		val += curr_len;
		address += curr_len;
	}

	return 0;
}

/*
 * Read multiple data bytes from Aries over I2C
 */
static int pt516xx_read_block_data(struct pt516xx_data *data, u32 address,
				   u8 len, u8 *val)
{
	struct i2c_client *client = data->client;
	int ret, tries;
	u8 remain_len = len;
	u8 curr_len;
	u8 wbuf[16], rbuf[24];
	u8 cmd = 0x08; // [7]:pec_en, [6:5]:rsvd, [4:2]:func, [1]:start, [0]:end
	u8 config = 0x00; // [6]:cfg_type, [4:1]:burst_len, [0]:bit16 of address

	if (data->pec_enable)
		cmd |= 0x80;

	// If byte count is greater than 16, perform multiple iterations
	while (remain_len > 0) {
		if (remain_len > 16) {
			curr_len = 16;
			remain_len -= 16;
		} else {
			curr_len = remain_len;
			remain_len = 0;
		}

		wbuf[0] = config | (curr_len - 1) << 1 |
			  ((address >> 16) & 0x1);
		wbuf[1] = (address >> 8) & 0xff;
		wbuf[2] = address & 0xff;

		// Perform read operation
		for (tries = 0; tries < 3; tries++) {
			ret = i2c_smbus_write_block_data(client, (cmd | 0x2), 3,
							 wbuf);
			if (ret)
				return ret;

			ret = i2c_smbus_read_block_data(client, (cmd | 0x1),
							rbuf);
			if (ret == curr_len)
				break;
		}
		if (tries >= 3)
			return -ENODATA;

		memcpy(val, rbuf, curr_len);
		val += curr_len;
		address += curr_len;
	}

	return 0;
}

static int pt516xx_read_wide_reg(struct pt516xx_data *data, u32 address,
				 u8 width, u8 *val)
{
	int ret, tries;
	u8 buf[8];
	u8 status;

	if (data->mm_wide_reg_valid) {
		// Write address (3 bytes)
		buf[0] = address & 0xff;
		buf[1] = (address >> 8) & 0xff;
		buf[2] = (address >> 16) & 0x1;
		ret = pt516xx_write_block_data(
			data, ARIES_MM_ASSIST_SPARE_0_OFFSET, 3, buf);
		if (ret)
			return ret;

		// Set command based on width
		switch (width) {
		case 2:
			buf[0] = ARIES_MM_RD_WIDE_REG_2B;
			break;
		case 3:
			buf[0] = ARIES_MM_RD_WIDE_REG_3B;
			break;
		case 4:
			buf[0] = ARIES_MM_RD_WIDE_REG_4B;
			break;
		case 5:
			buf[0] = ARIES_MM_RD_WIDE_REG_5B;
			break;
		default:
			return -EINVAL;
		}
		ret = pt516xx_write_block_data(data, ARIES_MM_ASSIST_CMD_OFFSET,
					       1, buf);
		if (ret)
			return ret;

		// Check access status
		status = 0xff;
		for (tries = 0; tries < 100; tries++) {
			ret = pt516xx_read_block_data(
				data, ARIES_MM_ASSIST_CMD_OFFSET, 1, &status);
			if (ret)
				return ret;

			if (status == 0)
				break;

			usleep_range(ARIES_MM_STATUS_TIME,
				     ARIES_MM_STATUS_TIME + 1000);
		}
		if (status != 0)
			return -ETIMEDOUT;

		// Read N bytes of data based on width
		ret = pt516xx_read_block_data(
			data, ARIES_MM_ASSIST_SPARE_3_OFFSET, width, val);
		if (ret)
			return ret;
	} else {
		return pt516xx_read_block_data(data, address, width, val);
	}

	return 0;
}

/*
 * Read multiple (up to eight) data bytes from micro SRAM over I2C
 */
static int
pt516xx_read_block_data_main_micro_indirect(struct pt516xx_data *data,
					    u32 address, u8 len, u8 *val)
{
	int ret, tries;
	u8 buf[8];
	u8 i, status;
	u32 uind_offs = ARIES_MM_ASSIST_REG_ADDR_OFFSET;
	u32 eeprom_base, eeprom_addr;

	// No multi-byte indirect support here. Hence read a byte at a time
	eeprom_base = address - AL_MAIN_SRAM_DMEM_OFFSET;
	for (i = 0; i < len; i++) {
		// Write eeprom addr
		eeprom_addr = eeprom_base + i;
		buf[0] = eeprom_addr & 0xff;
		buf[1] = (eeprom_addr >> 8) & 0xff;
		buf[2] = (eeprom_addr >> 16) & 0xff;
		ret = pt516xx_write_block_data(data, uind_offs, 3, buf);
		if (ret)
			return ret;

		// Write eeprom cmd
		buf[0] = AL_TG_RD_LOC_IND_SRAM;
		ret = pt516xx_write_block_data(data, uind_offs + 4, 1, buf);
		if (ret)
			return ret;

		// Test successful access
		status = 0xff;
		for (tries = 0; tries < 255; tries++) {
			ret = pt516xx_read_block_data(data, uind_offs + 4, 1,
						      &status);
			if (ret)
				return ret;

			if (status == 0)
				break;
		}
		if (status != 0)
			return -ETIMEDOUT;

		ret = pt516xx_read_block_data(data, uind_offs + 3, 1, buf);
		if (ret)
			return ret;

		val[i] = buf[0];
	}

	return 0;
}

/*
 * Check the status of firmware
 */
static int pt516xx_fwsts_check(struct pt516xx_data *data)
{
	int ret, tries;
	u8 buf[8];
	u8 heartbeat, major = 0, minor = 0;
	u16 build = 0;
	bool hb_changed = false;

	// Read Code Load reg
	ret = pt516xx_read_block_data(data, ARIES_CODE_LOAD_REG, 1, buf);
	if (ret)
		return ret;

	if (buf[0] < ARIES_LOAD_CODE) {
		dev_warn(
			&data->client->dev,
			"Code Load reg unexpected. Not all modules are loaded %x\n",
			buf[0]);
		data->code_load_okay = false;
	} else {
		data->code_load_okay = true;
	}

	// Check Main Micro heartbeat
	// If heartbeat value does not change for 100 tries, no MM heartbeat
	// Else heartbeat present even if one value changes
	ret = pt516xx_read_block_data(data, ARIES_MM_HEARTBEAT_ADDR, 1, buf);
	if (ret)
		return ret;

	heartbeat = buf[0];
	for (tries = 0; tries < 100; tries++) {
		ret = pt516xx_read_block_data(data, ARIES_MM_HEARTBEAT_ADDR, 1,
					      buf);
		if (ret)
			return ret;

		if (buf[0] != heartbeat) {
			hb_changed = true;
			break;
		}
	}
	data->mm_heartbeat_okay = hb_changed;

	// Read FW version
	// If heartbeat not there, set default FW values to 0.0.0
	// and return success
	if (data->mm_heartbeat_okay) {
		// Get FW version (major)
		ret = pt516xx_read_block_data_main_micro_indirect(
			data,
			ARIES_MAIN_MICRO_FW_INFO + ARIES_MM_FW_VERSION_MAJOR, 1,
			&major);
		if (ret)
			return ret;

		// Get FW version (minor)
		ret = pt516xx_read_block_data_main_micro_indirect(
			data,
			ARIES_MAIN_MICRO_FW_INFO + ARIES_MM_FW_VERSION_MINOR, 1,
			&minor);
		if (ret)
			return ret;

		// Get FW version (build)
		ret = pt516xx_read_block_data_main_micro_indirect(
			data,
			ARIES_MAIN_MICRO_FW_INFO + ARIES_MM_FW_VERSION_BUILD, 2,
			(u8 *)&build);
		if (ret)
			return ret;
	} else {
		dev_warn(&data->client->dev, "No Main Micro Heartbeat\n");
	}
	data->fw_ver.major = major;
	data->fw_ver.minor = minor;
	data->fw_ver.build = build;

	return 0;
}

static int pt516xx_fw_is_at_least(struct pt516xx_data *data, u8 major, u8 minor,
				  u16 build)
{
	u32 ver = major << 24 | minor << 16 | build;
	u32 curr_ver = data->fw_ver.major << 24 | data->fw_ver.minor << 16 |
		       data->fw_ver.build;

	if (curr_ver >= ver)
		return true;

	return false;
}

static int pt516xx_init_dev(struct pt516xx_data *data)
{
	int ret;

	mutex_lock(&data->lock);
	ret = pt516xx_fwsts_check(data);
	mutex_unlock(&data->lock);
	if (ret)
		return ret;

	dev_info(&data->client->dev, "fw_ver: %u.%u.%u\n", data->fw_ver.major,
		 data->fw_ver.minor, data->fw_ver.build);

	if (pt516xx_fw_is_at_least(data, 2, 2, 0))
		data->mm_wide_reg_valid = true;

	data->temp_cal_code_avg = ARIES_TEMP_CAL_CODE_DEFAULT;
	data->init_done = true;

	return 0;
}

static int pt516xx_read(struct device *dev, enum hwmon_sensor_types type,
			u32 attr, int channel, long *val)
{
	struct pt516xx_data *data = dev_get_drvdata(dev);
	int ret = 0;
	long adc_code = 0;

	if (!data->init_done) {
		ret = pt516xx_init_dev(data);
		if (ret) {
			dev_err(dev, "pt516xx_init_dev failed %d\n", ret);
			return ret;
		}
	}

	switch (attr) {
	case hwmon_temp_input:
		mutex_lock(&data->lock);
		ret = pt516xx_read_wide_reg(data,
					    ARIES_CURRENT_AVG_TEMP_ADC_CSR, 4,
					    (u8 *)&adc_code);
		mutex_unlock(&data->lock);
		break;
	default:
		return -EOPNOTSUPP;
	}
	if (ret) {
		dev_err(dev, "Read adc_code failed %d\n", ret);
		return ret;
	}
	if (adc_code == 0 || adc_code >= 0x3ff) {
		dev_err(dev, "Invalid adc_code %lx\n", adc_code);
		return -EINVAL;
	}

	*val = 110000 + ((adc_code - (data->temp_cal_code_avg + 250)) * -320);

	return 0;
}

static umode_t pt516xx_is_visible(const void *data,
				  enum hwmon_sensor_types type, u32 attr,
				  int channel)
{
	switch (attr) {
	case hwmon_temp_input:
		return 0444;
	default:
		break;
	}

	return 0;
}

static const struct hwmon_channel_info *pt516xx_info[] = {
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT),
	NULL
};

static const struct hwmon_ops pt516xx_hwmon_ops = {
	.is_visible = pt516xx_is_visible,
	.read = pt516xx_read,
};

static const struct hwmon_chip_info pt516xx_chip_info = {
	.ops = &pt516xx_hwmon_ops,
	.info = pt516xx_info,
};

static ssize_t pt516xx_debugfs_read_fw_ver(struct file *file, char __user *buf,
					   size_t count, loff_t *ppos)
{
	struct pt516xx_data *data = file->private_data;
	int ret;
	char ver[32];

	mutex_lock(&data->lock);
	ret = pt516xx_fwsts_check(data);
	mutex_unlock(&data->lock);
	if (ret)
		return ret;

	ret = snprintf(ver, sizeof(ver), "%u.%u.%u\n", data->fw_ver.major,
		       data->fw_ver.minor, data->fw_ver.build);
	if (ret < 0)
		return ret;

	return simple_read_from_buffer(buf, count, ppos, ver, ret + 1);
}

static const struct file_operations pt516xx_debugfs_ops_fw_ver = {
	.read = pt516xx_debugfs_read_fw_ver,
	.open = simple_open,
};

static ssize_t pt516xx_debugfs_read_health(struct file *file, char __user *buf,
					   size_t count, loff_t *ppos)
{
	struct pt516xx_data *data = file->private_data;
	int ret;
	u8 status = 0;
	char health[8];

	mutex_lock(&data->lock);
	ret = pt516xx_fwsts_check(data);
	mutex_unlock(&data->lock);
	if (ret == 0) {
		status |= data->mm_heartbeat_okay ? 1 : 0; // bit0
		status |= data->code_load_okay ? 2 : 0; // bit1
	}

	ret = snprintf(health, sizeof(health), "0x%02x\n", status);
	if (ret < 0)
		return ret;

	return simple_read_from_buffer(buf, count, ppos, health, ret + 1);
}

static const struct file_operations pt516xx_debugfs_ops_health = {
	.read = pt516xx_debugfs_read_health,
	.open = simple_open,
};

static int pt516xx_init_debugfs(struct pt516xx_data *data)
{
	if (!pt516xx_debugfs_dir)
		return -ENOENT;

	data->debugfs = debugfs_create_dir(dev_name(&data->client->dev),
					   pt516xx_debugfs_dir);
	if (IS_ERR_OR_NULL(data->debugfs))
		return -ENOENT;

	debugfs_create_file("fw_ver", 0444, data->debugfs, data,
			    &pt516xx_debugfs_ops_fw_ver);

	debugfs_create_file("health", 0444, data->debugfs, data,
			    &pt516xx_debugfs_ops_health);

	return 0;
}

static int pt516xx_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct device *hwmon_dev;
	struct pt516xx_data *data;

	data = devm_kzalloc(dev, sizeof(struct pt516xx_data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->client = client;
	mutex_init(&data->lock);
	pt516xx_init_dev(data);

	hwmon_dev = devm_hwmon_device_register_with_info(
		dev, client->name, data, &pt516xx_chip_info, NULL);

	if (pt516xx_init_debugfs(data))
		dev_warn(dev, "Failed to register debugfs\n");

	return PTR_ERR_OR_ZERO(hwmon_dev);
}

static const struct i2c_device_id pt516xx_id[] = {
	{ "pt5161l", 0 },
	{}
};
MODULE_DEVICE_TABLE(i2c, pt516xx_id);

static const struct of_device_id __maybe_unused pt516xx_of_match[] = {
	{ .compatible = "asteralabs,pt5161l" },
	{},
};
MODULE_DEVICE_TABLE(of, pt516xx_of_match);

static struct i2c_driver pt516xx_driver = {
	.class = I2C_CLASS_HWMON,
	.driver = {
		.name = "pt516xx",
		.of_match_table = of_match_ptr(pt516xx_of_match),
	},
	.probe = pt516xx_probe,
	.id_table = pt516xx_id,
};

module_i2c_driver(pt516xx_driver);

static int __init pt516xx_core_init(void)
{
	pt516xx_debugfs_dir = debugfs_create_dir("pt516xx", NULL);
	if (IS_ERR(pt516xx_debugfs_dir))
		pt516xx_debugfs_dir = NULL;

	return 0;
}

static void __exit pt516xx_core_exit(void)
{
	debugfs_remove_recursive(pt516xx_debugfs_dir);
}

module_init(pt516xx_core_init);
module_exit(pt516xx_core_exit);

MODULE_AUTHOR("Cosmo Chou <cosmo.chou@quantatw.com>");
MODULE_DESCRIPTION("Hwmon driver for Astera Labs Aries PCIe retimer");
MODULE_LICENSE("GPL");
