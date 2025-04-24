// SPDX-License-Identifier: GPL-2.0-only

#include <linux/array_size.h>
#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/dev_printk.h>
#include <linux/device.h>
#include <linux/export.h>
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
#define ZL_NUM_SIMPLE_PAGES	10
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

static int
zl3073x_read_reg(struct zl3073x_dev *zldev, unsigned int reg, void *val)
{
	unsigned int len;
	u8 buf[6];
	int rc;

	/* Offset of the last item in the indexed register or offset of
	 * the non-indexed register itself.
	 */
	if (ZL_REG_OFFSET(reg) > ZL_REG_MAX_OFFSET(reg)) {
		dev_err(zldev->dev, "Index of out range for reg 0x%04lx\n",
			ZL_REG_ADDR(reg));
		return -EINVAL;
	}

	/* Get register size */
	len = ZL_REG_SIZE(reg);

	/* Map the register address to virtual range */
	reg = ZL_REG_ADDR(reg) + ZL_RANGE_OFFSET;

	rc = regmap_bulk_read(zldev->regmap, reg, buf, len);
	if (rc) {
		dev_err(zldev->dev, "Failed to read reg 0x%04x: %pe\n", reg,
			ERR_PTR(rc));
		return rc;
	}

	switch (len) {
	case 1:
		*(u8 *)val = buf[0];
		break;
	case 2:
		*(u16 *)val = get_unaligned_be16(buf);
		break;
	case 4:
		*(u32 *)val = get_unaligned_be32(buf);
		break;
	case 6:
		*(u64 *)val = get_unaligned_be48(buf);
		break;
	default:
		dev_err(zldev->dev, "Invalid reg-width %u for reg 0x%04x\n",
			len, reg);
		return -EINVAL;
	}

	return rc;
}

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

	rc = zl3073x_read_reg(zldev, ZL_REG_ID, &id);
	if (rc)
		return rc;

	snprintf(buf, sizeof(buf), "%X", id);
	rc = devlink_info_version_fixed_put(req,
					    DEVLINK_INFO_VERSION_GENERIC_ASIC_ID,
					    buf);
	if (rc)
		return rc;

	rc = zl3073x_read_reg(zldev, ZL_REG_REVISION, &revision);
	if (rc)
		return rc;

	snprintf(buf, sizeof(buf), "%X", revision);
	rc = devlink_info_version_fixed_put(req,
					    DEVLINK_INFO_VERSION_GENERIC_ASIC_REV,
					    buf);
	if (rc)
		return rc;

	rc = zl3073x_read_reg(zldev, ZL_REG_FW_VER, &fw_ver);
	if (rc)
		return rc;

	snprintf(buf, sizeof(buf), "%u", fw_ver);
	rc = devlink_info_version_fixed_put(req,
					    DEVLINK_INFO_VERSION_GENERIC_FW,
					    buf);
	if (rc)
		return rc;

	rc = zl3073x_read_reg(zldev, ZL_REG_CUSTOM_CONFIG_VER, &cfg_ver);
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

static void zl3073x_devlink_unregister(void *ptr)
{
	devlink_unregister(ptr);
}

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
	struct devlink *devlink;
	unsigned int i;
	u32 cfg_ver;
	int rc;

	/* Read chip ID */
	rc = zl3073x_read_reg(zldev, ZL_REG_ID, &id);
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
	rc = zl3073x_read_reg(zldev, ZL_REG_REVISION, &revision);
	if (rc)
		return rc;
	rc = zl3073x_read_reg(zldev, ZL_REG_FW_VER, &fw_ver);
	if (rc)
		return rc;
	rc = zl3073x_read_reg(zldev, ZL_REG_CUSTOM_CONFIG_VER, &cfg_ver);
	if (rc)
		return rc;

	dev_dbg(zldev->dev, "ChipID(%X), ChipRev(%X), FwVer(%u)\n", id,
		revision, fw_ver);
	dev_dbg(zldev->dev, "Custom config version: %lu.%lu.%lu.%lu\n",
		FIELD_GET(GENMASK(31, 24), cfg_ver),
		FIELD_GET(GENMASK(23, 16), cfg_ver),
		FIELD_GET(GENMASK(15, 8), cfg_ver),
		FIELD_GET(GENMASK(7, 0), cfg_ver));

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
