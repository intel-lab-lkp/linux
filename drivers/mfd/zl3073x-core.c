// SPDX-License-Identifier: GPL-2.0-only

#include <linux/array_size.h>
#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/dev_printk.h>
#include <linux/device.h>
#include <linux/export.h>
#include <linux/mfd/zl3073x.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/unaligned.h>
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

	zldev = devm_kzalloc(dev, sizeof(*zldev), GFP_KERNEL);
	if (!zldev)
		return ERR_PTR(-ENOMEM);

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

/**
 * zl3073x_dev_probe - initialize zl3073x device
 * @zldev: pointer to zl3073x device
 * @chip_info: chip info based on compatible
 *
 * Common initialization of zl3073x device structure.
 *
 * Returns: 0 on success, <0 on error
 */
int zl3073x_dev_probe(struct zl3073x_dev *zldev,
		      const struct zl3073x_chip_info *chip_info)
{
	u16 id, revision, fw_ver;
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

	return 0;
}
EXPORT_SYMBOL_NS_GPL(zl3073x_dev_probe, "ZL3073X");

MODULE_AUTHOR("Ivan Vecera <ivecera@redhat.com>");
MODULE_DESCRIPTION("Microchip ZL3073x core driver");
MODULE_LICENSE("GPL");
