// SPDX-License-Identifier: GPL-2.0-only

#include <linux/array_size.h>
#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/dev_printk.h>
#include <linux/device.h>
#include <linux/export.h>
#include <linux/math64.h>
#include <linux/mfd/zl3073x.h>
#include <linux/module.h>
#include <linux/netlink.h>
#include <linux/regmap.h>
#include <linux/sprintf.h>
#include <linux/unaligned.h>
#include <net/devlink.h>
#include "zl3073x.h"
#include "zl3073x-regs.h"

/* Chip IDs for zl30731 */
static const u16 zl30731_ids[] = {
	0x0E93,
	0x1E93,
	0x2E93,
};

/* Chip IDs for zl30732 */
static const u16 zl30732_ids[] = {
	0x0E30,
	0x0E94,
	0x1E94,
	0x1F60,
	0x2E94,
	0x3FC4,
};

/* Chip IDs for zl30733 */
static const u16 zl30733_ids[] = {
	0x0E95,
	0x1E95,
	0x2E95,
};

/* Chip IDs for zl30734 */
static const u16 zl30734_ids[] = {
	0x0E96,
	0x1E96,
	0x2E96,
};

/* Chip IDs for zl30735 */
static const u16 zl30735_ids[] = {
	0x0E97,
	0x1E97,
	0x2E97,
};

const struct zl3073x_chip_info zl3073x_chip_info[] = {
	[ZL30731] = {
		.ids = zl30731_ids,
		.num_ids = ARRAY_SIZE(zl30731_ids),
		.num_channels = 1,
	},
	[ZL30732] = {
		.ids = zl30732_ids,
		.num_ids = ARRAY_SIZE(zl30732_ids),
		.num_channels = 2,
	},
	[ZL30733] = {
		.ids = zl30733_ids,
		.num_ids = ARRAY_SIZE(zl30733_ids),
		.num_channels = 3,
	},
	[ZL30734] = {
		.ids = zl30734_ids,
		.num_ids = ARRAY_SIZE(zl30734_ids),
		.num_channels = 4,
	},
	[ZL30735] = {
		.ids = zl30735_ids,
		.num_ids = ARRAY_SIZE(zl30735_ids),
		.num_channels = 5,
	},
};
EXPORT_SYMBOL_NS_GPL(zl3073x_chip_info, "ZL3073X");

#define ZL_RANGE_OFFSET		0x80
#define ZL_PAGE_SIZE		0x80
#define ZL_NUM_PAGES		15
#define ZL_PAGE_SEL		0x7F
#define ZL_PAGE_SEL_MASK	GENMASK(3, 0)
#define ZL_NUM_REGS		(ZL_NUM_PAGES * ZL_PAGE_SIZE)

/* Regmap range configuration */
static const struct regmap_range_cfg zl3073x_regmap_range = {
	.range_min	= ZL_RANGE_OFFSET,
	.range_max	= ZL_RANGE_OFFSET + ZL_NUM_REGS - 1,
	.selector_reg	= ZL_PAGE_SEL,
	.selector_mask	= ZL_PAGE_SEL_MASK,
	.selector_shift	= 0,
	.window_start	= 0,
	.window_len	= ZL_PAGE_SIZE,
};

static bool
zl3073x_is_volatile_reg(struct device *dev __maybe_unused, unsigned int reg)
{
	/* Only page selector is non-volatile */
	return reg != ZL_PAGE_SEL;
}

static const struct regmap_config zl3073x_regmap_config = {
	.reg_bits	= 8,
	.val_bits	= 8,
	.max_register	= ZL_RANGE_OFFSET + ZL_NUM_REGS - 1,
	.ranges		= &zl3073x_regmap_range,
	.num_ranges	= 1,
	.cache_type	= REGCACHE_MAPLE,
	.volatile_reg	= zl3073x_is_volatile_reg,
};

static bool
zl3073x_check_reg(struct zl3073x_dev *zldev, unsigned int reg, size_t size)
{
	/* Check that multiop lock is held when accessing registers
	 * from page 10 and above.
	 */
	if (ZL_REG_PAGE(reg) >= 10)
		lockdep_assert_held(&zldev->multiop_lock);

	/* Check the index is in valid range for indexed register */
	if (ZL_REG_OFFSET(reg) > ZL_REG_MAX_OFFSET(reg)) {
		dev_err(zldev->dev, "Index out of range for reg 0x%04lx\n",
			ZL_REG_ADDR(reg));
		return false;
	}
	/* Check the requested size corresponds to register size */
	if (ZL_REG_SIZE(reg) != size) {
		dev_err(zldev->dev, "Invalid size %zu for reg 0x%04lx\n",
			size, ZL_REG_ADDR(reg));
		return false;
	}

	return true;
}

static int
zl3073x_read_reg(struct zl3073x_dev *zldev, unsigned int reg, void *val,
		 size_t size)
{
	int rc;

	if (!zl3073x_check_reg(zldev, reg, size))
		return -EINVAL;

	/* Map the register address to virtual range */
	reg = ZL_REG_ADDR(reg) + ZL_RANGE_OFFSET;

	rc = regmap_bulk_read(zldev->regmap, reg, val, size);
	if (rc) {
		dev_err(zldev->dev, "Failed to read reg 0x%04x: %pe\n", reg,
			ERR_PTR(rc));
		return rc;
	}

	return 0;
}

static int
zl3073x_write_reg(struct zl3073x_dev *zldev, unsigned int reg, const void *val,
		  size_t size)
{
	int rc;

	if (!zl3073x_check_reg(zldev, reg, size))
		return -EINVAL;

	/* Map the register address to virtual range */
	reg = ZL_REG_ADDR(reg) + ZL_RANGE_OFFSET;

	rc = regmap_bulk_write(zldev->regmap, reg, val, size);
	if (rc) {
		dev_err(zldev->dev, "Failed to write reg 0x%04x: %pe\n", reg,
			ERR_PTR(rc));
		return rc;
	}

	return 0;
}

/**
 * zl3073x_read_u8 - read value from 8bit register
 * @zldev: zl3073x device pointer
 * @reg: register to write to
 * @val: value to write
 *
 * Reads value from given 8bit register.
 *
 * Returns: 0 on success, <0 on error
 */
int zl3073x_read_u8(struct zl3073x_dev *zldev, unsigned int reg, u8 *val)
{
	return zl3073x_read_reg(zldev, reg, val, sizeof(*val));
}
EXPORT_SYMBOL_NS_GPL(zl3073x_read_u8, "ZL3073X");

/**
 * zl3073x_write_u8 - write value to 16bit register
 * @zldev: zl3073x device pointer
 * @reg: register to write to
 * @val: value to write
 *
 * Writes value into given 8bit register.
 *
 * Returns: 0 on success, <0 on error
 */
int zl3073x_write_u8(struct zl3073x_dev *zldev, unsigned int reg, u8 val)
{
	return zl3073x_write_reg(zldev, reg, &val, sizeof(val));
}
EXPORT_SYMBOL_NS_GPL(zl3073x_write_u8, "ZL3073X");

/**
 * zl3073x_read_u16 - read value from 16bit register
 * @zldev: zl3073x device pointer
 * @reg: register to write to
 * @val: value to write
 *
 * Reads value from given 16bit register.
 *
 * Returns: 0 on success, <0 on error
 */
int zl3073x_read_u16(struct zl3073x_dev *zldev, unsigned int reg, u16 *val)
{
	int rc;

	rc = zl3073x_read_reg(zldev, reg, val, sizeof(*val));
	if (!rc)
		be16_to_cpus(val);

	return rc;
}
EXPORT_SYMBOL_NS_GPL(zl3073x_read_u16, "ZL3073X");

/**
 * zl3073x_write_u16 - write value to 16bit register
 * @zldev: zl3073x device pointer
 * @reg: register to write to
 * @val: value to write
 *
 * Writes value into given 16bit register.
 *
 * Returns: 0 on success, <0 on error
 */
int zl3073x_write_u16(struct zl3073x_dev *zldev, unsigned int reg, u16 val)
{
	cpu_to_be16s(&val);

	return zl3073x_write_reg(zldev, reg, &val, sizeof(val));
}
EXPORT_SYMBOL_NS_GPL(zl3073x_write_u16, "ZL3073X");

/**
 * zl3073x_read_u32 - read value from 32bit register
 * @zldev: zl3073x device pointer
 * @reg: register to write to
 * @val: value to write
 *
 * Reads value from given 32bit register.
 *
 * Returns: 0 on success, <0 on error
 */
int zl3073x_read_u32(struct zl3073x_dev *zldev, unsigned int reg, u32 *val)
{
	int rc;

	rc = zl3073x_read_reg(zldev, reg, val, sizeof(*val));
	if (!rc)
		be32_to_cpus(val);

	return rc;
}
EXPORT_SYMBOL_NS_GPL(zl3073x_read_u32, "ZL3073X");

/**
 * zl3073x_write_u32 - write value to 32bit register
 * @zldev: zl3073x device pointer
 * @reg: register to write to
 * @val: value to write
 *
 * Writes value into given 32bit register.
 *
 * Returns: 0 on success, <0 on error
 */
int zl3073x_write_u32(struct zl3073x_dev *zldev, unsigned int reg, u32 val)
{
	cpu_to_be32s(&val);

	return zl3073x_write_reg(zldev, reg, &val, sizeof(val));
}
EXPORT_SYMBOL_NS_GPL(zl3073x_write_u32, "ZL3073X");

/**
 * zl3073x_read_u48 - read value from 48bit register
 * @zldev: zl3073x device pointer
 * @reg: register to write to
 * @val: value to write
 *
 * Reads value from given 48bit register.
 *
 * Returns: 0 on success, <0 on error
 */
int zl3073x_read_u48(struct zl3073x_dev *zldev, unsigned int reg, u64 *val)
{
	u8 buf[6];
	int rc;

	rc = zl3073x_read_reg(zldev, reg, buf, sizeof(buf));
	if (!rc)
		*val = get_unaligned_be48(buf);

	return rc;
}
EXPORT_SYMBOL_NS_GPL(zl3073x_read_u48, "ZL3073X");

/**
 * zl3073x_write_u48 - write value to 48bit register
 * @zldev: zl3073x device pointer
 * @reg: register to write to
 * @val: value to write
 *
 * Writes value into given 48bit register.
 *
 * Returns: 0 on success, <0 on error
 */
int zl3073x_write_u48(struct zl3073x_dev *zldev, unsigned int reg, u64 val)
{
	u8 buf[6];

	/* Check the value belongs to <S48_MIN, U48_MAX> */
	if (!(GENMASK_ULL(63, 49) & (val + BIT_ULL(47)))) {
		dev_err(zldev->dev, "Value 0x%0llx out of range\n", val);
		return -EINVAL;
	}

	put_unaligned_be48(val, buf);

	return zl3073x_write_reg(zldev, reg, buf, sizeof(buf));
}
EXPORT_SYMBOL_NS_GPL(zl3073x_write_u48, "ZL3073X");

/**
 * zl3073x_poll_zero_u8 - wait for register to be cleared by device
 * @zldev: zl3073x device pointer
 * @reg: register to poll (has to be 8bit register)
 * @mask: bit mask for polling
 *
 * Waits for bits specified by @mask in register @reg value to be cleared
 * by the device.
 *
 * Returns: 0 on success, <0 on error
 */
int zl3073x_poll_zero_u8(struct zl3073x_dev *zldev, unsigned int reg, u8 mask)
{
	/* Register polling sleep & timeout */
#define ZL_POLL_SLEEP_US   10
#define ZL_POLL_TIMEOUT_US 2000000
	unsigned int val;

	/* Check the register is 8bit */
	if (ZL_REG_SIZE(reg) != 1) {
		dev_err(zldev->dev, "Invalid reg 0x%04lx size for polling\n",
			ZL_REG_ADDR(reg));
		return -EINVAL;
	}

	/* Map the register address to virtual range */
	reg = ZL_REG_ADDR(reg) + ZL_RANGE_OFFSET;

	return regmap_read_poll_timeout(zldev->regmap, reg, val, !(val & mask),
					ZL_POLL_SLEEP_US, ZL_POLL_TIMEOUT_US);
}
EXPORT_SYMBOL_NS_GPL(zl3073x_poll_zero_u8, "ZL3073X");

/**
 * zl3073x_devlink_info_get - Devlink device info callback
 * @devlink: devlink structure pointer
 * @req: devlink request pointer to store information
 * @extack: netlink extack pointer to report errors
 *
 * Return: 0 on success, <0 on error
 */
static int zl3073x_devlink_info_get(struct devlink *devlink,
				    struct devlink_info_req *req,
				    struct netlink_ext_ack *extack)
{
	struct zl3073x_dev *zldev = devlink_priv(devlink);
	u16 id, revision, fw_ver;
	char buf[16];
	u32 cfg_ver;
	int rc;

	rc = zl3073x_read_u16(zldev, ZL_REG_ID, &id);
	if (rc)
		return rc;

	snprintf(buf, sizeof(buf), "%X", id);
	rc = devlink_info_version_fixed_put(req,
					    DEVLINK_INFO_VERSION_GENERIC_ASIC_ID,
					    buf);
	if (rc)
		return rc;

	rc = zl3073x_read_u16(zldev, ZL_REG_REVISION, &revision);
	if (rc)
		return rc;

	snprintf(buf, sizeof(buf), "%X", revision);
	rc = devlink_info_version_fixed_put(req,
					    DEVLINK_INFO_VERSION_GENERIC_ASIC_REV,
					    buf);
	if (rc)
		return rc;

	rc = zl3073x_read_u16(zldev, ZL_REG_FW_VER, &fw_ver);
	if (rc)
		return rc;

	snprintf(buf, sizeof(buf), "%u", fw_ver);
	rc = devlink_info_version_fixed_put(req,
					    DEVLINK_INFO_VERSION_GENERIC_FW,
					    buf);
	if (rc)
		return rc;

	rc = zl3073x_read_u32(zldev, ZL_REG_CUSTOM_CONFIG_VER, &cfg_ver);
	if (rc)
		return rc;

	/* No custom config version */
	if (cfg_ver == U32_MAX)
		return 0;

	snprintf(buf, sizeof(buf), "%lu.%lu.%lu.%lu",
		 FIELD_GET(GENMASK(31, 24), cfg_ver),
		 FIELD_GET(GENMASK(23, 16), cfg_ver),
		 FIELD_GET(GENMASK(15, 8), cfg_ver),
		 FIELD_GET(GENMASK(7, 0), cfg_ver));

	return devlink_info_version_running_put(req, "cfg.custom_ver", buf);
}

static const struct devlink_ops zl3073x_devlink_ops = {
	.info_get = zl3073x_devlink_info_get,
};

static void zl3073x_devlink_free(void *ptr)
{
	devlink_free(ptr);
}

/**
 * zl3073x_devm_alloc - allocates zl3073x device structure
 * @dev: pointer to device structure
 *
 * Allocates zl3073x device structure as device resource.
 *
 * Return: pointer to zl3073x device on success, error pointer on error
 */
struct zl3073x_dev *zl3073x_devm_alloc(struct device *dev)
{
	struct zl3073x_dev *zldev;
	struct devlink *devlink;
	int rc;

	devlink = devlink_alloc(&zl3073x_devlink_ops, sizeof(*zldev), dev);
	if (!devlink)
		return ERR_PTR(-ENOMEM);

	/* Add devres action to free devlink device */
	rc = devm_add_action_or_reset(dev, zl3073x_devlink_free, devlink);
	if (rc)
		return ERR_PTR(rc);

	zldev = devlink_priv(devlink);
	zldev->dev = dev;
	dev_set_drvdata(zldev->dev, zldev);

	return zldev;
}
EXPORT_SYMBOL_NS_GPL(zl3073x_devm_alloc, "ZL3073X");

/**
 * zl3073x_dev_init_regmap_config - initialize regmap config
 * @regmap_cfg: regmap_config structure to fill
 *
 * Initializes regmap config common for I2C and SPI.
 */
void zl3073x_dev_init_regmap_config(struct regmap_config *regmap_cfg)
{
	*regmap_cfg = zl3073x_regmap_config;
}
EXPORT_SYMBOL_NS_GPL(zl3073x_dev_init_regmap_config, "ZL3073X");

static int
zl3073x_mb_op(struct zl3073x_dev *zldev, unsigned int op_reg, u8 op_val,
	      unsigned int mask_reg, u16 mask_val)
{
	int rc;

	/* Set mask for the operation */
	rc = zl3073x_write_u16(zldev, mask_reg, mask_val);
	if (rc)
		return rc;

	/* Trigger the operation */
	rc = zl3073x_write_u8(zldev, op_reg, op_val);
	if (rc)
		return rc;

	/* Wait for the operation to actually finish */
	return zl3073x_poll_zero_u8(zldev, op_reg, op_val);
}

/**
 * zl3073x_input_state_fetch - get input state
 * @zldev: pointer to zl3073x_dev structure
 * @index: input pin index to fetch state for
 *
 * Function fetches information for the given input reference that are
 * invariant and stores them for later use.
 *
 * Return: 0 on success, <0 on error
 */
static int
zl3073x_input_state_fetch(struct zl3073x_dev *zldev, u8 index)
{
	struct zl3073x_input *input;
	u8 ref_config;
	int rc;

	input = &zldev->input[index];

	/* If the input is differential then the configuration for N-pin
	 * reference is ignored and P-pin config is used for both.
	 */
	if (zl3073x_is_n_pin(index) &&
	    zl3073x_input_is_diff(zldev, index - 1)) {
		input->enabled = zl3073x_input_is_enabled(zldev, index - 1);
		input->diff = true;

		return 0;
	}

	guard(mutex)(&zldev->multiop_lock);

	/* Read reference configuration */
	rc = zl3073x_mb_op(zldev, ZL_REG_REF_MB_SEM, ZL_REF_MB_SEM_RD,
			   ZL_REG_REF_MB_MASK, BIT(index));
	if (rc)
		return rc;

	/* Read ref_config register */
	rc = zl3073x_read_u8(zldev, ZL_REG_REF_CONFIG, &ref_config);
	if (rc)
		return rc;

	input->enabled = FIELD_GET(ZL_REF_CONFIG_ENABLE, ref_config);
	input->diff = FIELD_GET(ZL_REF_CONFIG_DIFF_EN, ref_config);

	dev_dbg(zldev->dev, "INPUT%u is %s and configured as %s\n", index,
		input->enabled ? "enabled" : "disabled",
		input->diff ? "differential" : "single-ended");

	return rc;
}

/**
 * zl3073x_output_state_fetch - get output state
 * @zldev: pointer to zl3073x_dev structure
 * @index: output index to fetch state for
 *
 * Function fetches information for the given output (not output pin)
 * that are invariant and stores them for later use.
 *
 * Return: 0 on success, <0 on error
 */
static int
zl3073x_output_state_fetch(struct zl3073x_dev *zldev, u8 index)
{
	struct zl3073x_output *output;
	u8 output_ctrl, output_mode;
	int rc;

	output = &zldev->output[index];

	/* Read output configuration */
	rc = zl3073x_read_u8(zldev, ZL_REG_OUTPUT_CTRL(index), &output_ctrl);
	if (rc)
		return rc;

	/* Store info about output enablement and synthesizer the output
	 * is connected to.
	 */
	output->enabled = FIELD_GET(ZL_OUTPUT_CTRL_EN, output_ctrl);
	output->synth = FIELD_GET(ZL_OUTPUT_CTRL_SYNTH_SEL, output_ctrl);

	dev_dbg(zldev->dev, "OUTPUT%u is %s, connected to SYNTH%u\n",
		index, output->enabled ? "enabled" : "disabled", output->synth);

	guard(mutex)(&zldev->multiop_lock);

	/* Read output configuration */
	rc = zl3073x_mb_op(zldev, ZL_REG_OUTPUT_MB_SEM, ZL_OUTPUT_MB_SEM_RD,
			   ZL_REG_OUTPUT_MB_MASK, BIT(index));
	if (rc)
		return rc;

	/* Read output_mode */
	rc = zl3073x_read_u8(zldev, ZL_REG_OUTPUT_MODE, &output_mode);
	if (rc)
		return rc;

	/* Extract and store output signal format */
	output->signal_format = FIELD_GET(ZL_OUTPUT_MODE_SIGNAL_FORMAT,
					  output_mode);

	dev_dbg(zldev->dev, "OUTPUT%u has signal format 0x%02x\n", index,
		output->signal_format);

	return rc;
}

/**
 * zl3073x_synth_state_fetch - get synth state
 * @zldev: pointer to zl3073x_dev structure
 * @index: synth index to fetch state for
 *
 * Function fetches information for the given synthesizer that are
 * invariant and stores them for later use.
 *
 * Return: 0 on success, <0 on error
 */
static int
zl3073x_synth_state_fetch(struct zl3073x_dev *zldev, u8 index)
{
	u16 base, num, denom;
	u8 synth_ctrl;
	u32 mult;
	int rc;

	/* Read synth control register */
	rc = zl3073x_read_u8(zldev, ZL_REG_SYNTH_CTRL(index), &synth_ctrl);
	if (rc)
		return rc;

	/* Extract and store DPLL channel the synth is driven by */
	zldev->synth[index].dpll = FIELD_GET(ZL_SYNTH_CTRL_DPLL_SEL,
					     synth_ctrl);

	dev_dbg(zldev->dev, "SYNTH%u is connected to DPLL%u\n", index,
		zldev->synth[index].dpll);

	guard(mutex)(&zldev->multiop_lock);

	/* Read synth configuration */
	rc = zl3073x_mb_op(zldev, ZL_REG_SYNTH_MB_SEM, ZL_SYNTH_MB_SEM_RD,
			   ZL_REG_SYNTH_MB_MASK, BIT(index));
	if (rc)
		return rc;

	/* The output frequency is determined by the following formula:
	 * base * multiplier * numerator / denominator
	 *
	 * Read registers with these values
	 */
	rc = zl3073x_read_u16(zldev, ZL_REG_SYNTH_FREQ_BASE, &base);
	if (rc)
		return rc;

	rc = zl3073x_read_u32(zldev, ZL_REG_SYNTH_FREQ_MULT, &mult);
	if (rc)
		return rc;

	rc = zl3073x_read_u16(zldev, ZL_REG_SYNTH_FREQ_M, &num);
	if (rc)
		return rc;

	rc = zl3073x_read_u16(zldev, ZL_REG_SYNTH_FREQ_N, &denom);
	if (rc)
		return rc;

	/* Check denominator for zero to avoid div by 0 */
	if (!denom) {
		dev_err(zldev->dev,
			"Zero divisor for SYNTH%u retrieved from device\n",
			index);
		return -EINVAL;
	}

	/* Compute and store synth frequency */
	zldev->synth[index].freq = mul_u64_u32_div(mul_u32_u32(base, mult),
						   num, denom);

	dev_dbg(zldev->dev, "SYNTH%u frequency: %llu Hz\n", index,
		zldev->synth[index].freq);

	return rc;
}

static int
zl3073x_dev_state_fetch(struct zl3073x_dev *zldev)
{
	int rc;
	u8 i;

	for (i = 0; i < ZL3073X_NUM_INPUTS; i++) {
		rc = zl3073x_input_state_fetch(zldev, i);
		if (rc) {
			dev_err(zldev->dev,
				"Failed to fetch input state: %pe\n",
				ERR_PTR(rc));
			return rc;
		}
	}

	for (i = 0; i < ZL3073X_NUM_SYNTHS; i++) {
		rc = zl3073x_synth_state_fetch(zldev, i);
		if (rc) {
			dev_err(zldev->dev,
				"Failed to fetch synth state: %pe\n",
				ERR_PTR(rc));
			return rc;
		}
	}

	for (i = 0; i < ZL3073X_NUM_OUTPUTS; i++) {
		rc = zl3073x_output_state_fetch(zldev, i);
		if (rc) {
			dev_err(zldev->dev,
				"Failed to fetch output state: %pe\n",
				ERR_PTR(rc));
			return rc;
		}
	}

	return rc;
}

static void zl3073x_devlink_unregister(void *ptr)
{
	devlink_unregister(ptr);
}

/**
 * zl3073x_dev_probe - initialize zl3073x device
 * @zldev: pointer to zl3073x device
 * @chip_info: chip info based on compatible
 * @dev_id: device ID to be used as part of clock ID
 *
 * Common initialization of zl3073x device structure.
 *
 * Returns: 0 on success, <0 on error
 */
int zl3073x_dev_probe(struct zl3073x_dev *zldev,
		      const struct zl3073x_chip_info *chip_info, u8 dev_id)
{
	u16 id, revision, fw_ver;
	struct devlink *devlink;
	unsigned int i;
	u32 cfg_ver;
	int rc;

	/* Read chip ID */
	rc = zl3073x_read_u16(zldev, ZL_REG_ID, &id);
	if (rc)
		return rc;

	/* Check it matches */
	for (i = 0; i < chip_info->num_ids; i++) {
		if (id == chip_info->ids[i])
			break;
	}

	if (i == chip_info->num_ids) {
		return dev_err_probe(zldev->dev, -ENODEV,
				     "Unknown or non-match chip ID: 0x%0x\n",
				     id);
	}

	/* Read revision, firmware version and custom config version */
	rc = zl3073x_read_u16(zldev, ZL_REG_REVISION, &revision);
	if (rc)
		return rc;
	rc = zl3073x_read_u16(zldev, ZL_REG_FW_VER, &fw_ver);
	if (rc)
		return rc;
	rc = zl3073x_read_u32(zldev, ZL_REG_CUSTOM_CONFIG_VER, &cfg_ver);
	if (rc)
		return rc;

	dev_dbg(zldev->dev, "ChipID(%X), ChipRev(%X), FwVer(%u)\n", id,
		revision, fw_ver);
	dev_dbg(zldev->dev, "Custom config version: %lu.%lu.%lu.%lu\n",
		FIELD_GET(GENMASK(31, 24), cfg_ver),
		FIELD_GET(GENMASK(23, 16), cfg_ver),
		FIELD_GET(GENMASK(15, 8), cfg_ver),
		FIELD_GET(GENMASK(7, 0), cfg_ver));

	/* Use chip ID and given dev ID as clock ID */
	zldev->clock_id = ((u64)id << 8) | dev_id;

	/* Initialize mutex for operations where multiple reads, writes
	 * and/or polls are required to be done atomically.
	 */
	rc = devm_mutex_init(zldev->dev, &zldev->multiop_lock);
	if (rc)
		return dev_err_probe(zldev->dev, rc,
				     "Failed to initialize mutex\n");

	/* Fetch device state */
	rc = zl3073x_dev_state_fetch(zldev);
	if (rc)
		return rc;

	/* Register the device as devlink device */
	devlink = priv_to_devlink(zldev);
	devlink_register(devlink);

	/* Add devres action to unregister devlink device */
	rc = devm_add_action_or_reset(zldev->dev, zl3073x_devlink_unregister,
				      devlink);
	if (rc)
		return rc;

	return 0;
}
EXPORT_SYMBOL_NS_GPL(zl3073x_dev_probe, "ZL3073X");

MODULE_AUTHOR("Ivan Vecera <ivecera@redhat.com>");
MODULE_DESCRIPTION("Microchip ZL3073x core driver");
MODULE_LICENSE("GPL");
