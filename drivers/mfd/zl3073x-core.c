// SPDX-License-Identifier: GPL-2.0-only

#include <linux/array_size.h>
#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/cleanup.h>
#include <linux/dev_printk.h>
#include <linux/device.h>
#include <linux/export.h>
#include <linux/lockdep.h>
#include <linux/mfd/zl3073x.h>
#include <linux/module.h>
#include <linux/netlink.h>
#include <linux/regmap.h>
#include <linux/sprintf.h>
#include <linux/unaligned.h>
#include <net/devlink.h>
#include "zl3073x.h"

/*
 * Register Map Page 0, General
 */
ZL3073X_REG16_DEF(id,			0x0001);
ZL3073X_REG16_DEF(revision,		0x0003);
ZL3073X_REG16_DEF(fw_ver,		0x0005);
ZL3073X_REG32_DEF(custom_config_ver,	0x0007);

/*
 * Register Map Page 10, Ref Mailbox
 */
ZL3073X_REG16_DEF(ref_mb_mask,			0x502);
#define REF_MB_MASK				GENMASK(9, 0)

ZL3073X_REG8_DEF(ref_mb_sem,			0x504);
#define REF_MB_SEM_WR				BIT(0)
#define REF_MB_SEM_RD				BIT(1)

/*
 * Register Map Page 12, DPLL Mailbox
 */
ZL3073X_REG16_DEF(dpll_mb_mask,			0x602);

ZL3073X_REG8_DEF(dpll_mb_sem,			0x604);
#define DPLL_MB_SEM_WR				BIT(0)
#define DPLL_MB_SEM_RD				BIT(1)

/*
 * Register Map Page 13, Synth Mailbox
 */
ZL3073X_REG16_DEF(synth_mb_mask,		0x682);

ZL3073X_REG8_DEF(synth_mb_sem,			0x684);
#define SYNTH_MB_SEM_WR				BIT(0)
#define SYNTH_MB_SEM_RD				BIT(1)

ZL3073X_REG16_DEF(synth_freq_base,		0x686);
ZL3073X_REG32_DEF(synth_freq_mult,		0x688);
ZL3073X_REG16_DEF(synth_freq_m,			0x68c);
ZL3073X_REG16_DEF(synth_freq_n,			0x68e);

/*
 * Register Map Page 14, Output Mailbox
 */
ZL3073X_REG16_DEF(output_mb_mask,		0x702);
ZL3073X_REG8_DEF(output_mb_sem,			0x704);
#define OUTPUT_MB_SEM_WR			BIT(0)
#define OUTPUT_MB_SEM_RD			BIT(1)

/*
 * Regmap ranges
 */
#define ZL3073x_PAGE_SIZE	128
#define ZL3073x_NUM_PAGES	16
#define ZL3073x_PAGE_SEL	0x7F

/*
 * Regmap range configuration
 *
 * The device uses 7-bit addressing and has 16 register pages with
 * range 0x00-0x7f. The register 0x7f in each page acts as page
 * selector where bits 0-3 contains currently selected page.
 */
static const struct regmap_range_cfg zl3073x_regmap_ranges[] = {
	{
		.range_min	= 0,
		.range_max	= ZL3073x_NUM_PAGES * ZL3073x_PAGE_SIZE,
		.selector_reg	= ZL3073x_PAGE_SEL,
		.selector_mask	= GENMASK(3, 0),
		.selector_shift	= 0,
		.window_start	= 0,
		.window_len	= ZL3073x_PAGE_SIZE,
	},
};

/*
 * Regmap config
 */
const struct regmap_config zl3073x_regmap_config = {
	.reg_bits		= 8,
	.val_bits		= 8,
	.max_register		= ZL3073x_NUM_PAGES * ZL3073x_PAGE_SIZE,
	.ranges			= zl3073x_regmap_ranges,
	.num_ranges		= ARRAY_SIZE(zl3073x_regmap_ranges),
	.disable_locking	= true,
};

/**
 * zl3073x_get_regmap_config - return pointer to regmap config
 *
 * Return: pointer to regmap config
 */
const struct regmap_config *zl3073x_get_regmap_config(void)
{
	return &zl3073x_regmap_config;
}
EXPORT_SYMBOL_NS_GPL(zl3073x_get_regmap_config, "ZL3073X");

/**
 * zl3073x_read_reg - Read value from device register
 * @zldev: pointer to zl3073x device
 * @reg: register to be read
 * @len: number of bytes to read
 * @value: pointer to place to store value read from the register
 *
 * Caller has to hold the device lock that can be obtained
 * by zl3073x_lock().
 *
 * Return: 0 on success or <0 on error
 */
int zl3073x_read_reg(struct zl3073x_dev *zldev, unsigned int reg,
		     unsigned int len, void *value)
{
	u8 buf[6];
	int rc;

	lockdep_assert_held(&zldev->lock);

	rc = regmap_bulk_read(zldev->regmap, reg, buf, len);
	if (rc)
		return rc;

	switch (len) {
	case 1:
		*(u8 *)value = buf[0];
		break;
	case 2:
		*(u16 *)value = get_unaligned_be16(buf);
		break;
	case 4:
		*(u32 *)value = get_unaligned_be32(buf);
		break;
	case 6:
		*(u64 *)value = get_unaligned_be48(buf);
		break;
	default:
		WARN(true, "Unsupported register size: %u\n", len);
		break;
	}

	return rc;
}
EXPORT_SYMBOL_GPL(zl3073x_read_reg);

/**
 * zl3073x_write_reg - Write value to device register
 * @zldev: pointer to zl3073x device
 * @reg: register to be written
 * @len: number of bytes to write
 * @value: pointer to value to write to the register
 *
 * Caller has to hold the device lock that can be obtained
 * by zl3073x_lock().
 *
 * Return: 0 on success, <0 on error
 */
int zl3073x_write_reg(struct zl3073x_dev *zldev, unsigned int reg,
		      unsigned int len, const void *value)
{
	u8 buf[6];

	lockdep_assert_held(&zldev->lock);

	switch (len) {
	case 1:
		buf[0] = *(u8 *)value;
		break;
	case 2:
		put_unaligned_be16(*(u16 *)value, buf);
		break;
	case 4:
		put_unaligned_be32(*(u32 *)value, buf);
		break;
	case 6:
		put_unaligned_be48(*(u64 *)value, buf);
		break;
	default:
		WARN(true, "Unsupported register size: %u\n", len);
		break;
	}

	return regmap_bulk_write(zldev->regmap, reg, buf, len);
}
EXPORT_SYMBOL_GPL(zl3073x_write_reg);

/**
 * ZL3073X_MB_OP - perform an operation on mailbox of certain type
 * @_zldev: pointer to device structure
 * @_type: type of mailbox (dpll, ref or output)
 * @_index: object index of given type
 * @_op: operation to perform
 *
 * Performs the requested operation on mailbox of certain type and
 * returns 0 in case of success or negative value otherwise.
 */
#define ZL3073X_MB_OP(_zldev, _type, _index, _op)			\
({									\
	struct zl3073x_dev *__zldev = (_zldev);				\
	u16 __mask = BIT(_index);					\
	u8 __op = (_op);						\
	int __rc;							\
	do {								\
		/* Select requested index in mask register */		\
		__rc = zl3073x_write_##_type##_mb_mask(__zldev, __mask);\
		if (__rc)						\
			break;						\
		/* Select requested command */				\
		__rc = zl3073x_write_##_type##_mb_sem(__zldev, __op);	\
		if (__rc)						\
			break;						\
		/* Wait for the command to actually finish */		\
		__rc = zl3073x_wait_clear_bits(__zldev, _type##_mb_sem,	\
					       __op);			\
	} while (0);							\
	__rc;								\
})

/**
 * zl3073x_mb_dpll_read - read given DPLL configuration to mailbox
 * @zldev: pointer to device structure
 * @index: DPLL index
 *
 * Reads configuration of given DPLL into DPLL mailbox.
 *
 * Return: 0 on success, <0 on error
 */
int zl3073x_mb_dpll_read(struct zl3073x_dev *zldev, u8 index)
{
	return ZL3073X_MB_OP(zldev, dpll, index, DPLL_MB_SEM_RD);
}
EXPORT_SYMBOL_GPL(zl3073x_mb_dpll_read);

/**
 * zl3073x_mb_dpll_write - write given DPLL configuration from mailbox
 * @zldev: pointer to device structure
 * @index: DPLL index
 *
 * Writes (commits) configuration of given DPLL from DPLL mailbox.
 *
 * Return: 0 on success, <0 on error
 */
int zl3073x_mb_dpll_write(struct zl3073x_dev *zldev, u8 index)
{
	return ZL3073X_MB_OP(zldev, dpll, index, DPLL_MB_SEM_WR);
}
EXPORT_SYMBOL_GPL(zl3073x_mb_dpll_write);

/**
 * zl3073x_mb_output_read - read given output configuration to mailbox
 * @zldev: pointer to device structure
 * @index: output index
 *
 * Reads configuration of given output into output mailbox.
 *
 * Return: 0 on success, <0 on error
 */
int zl3073x_mb_output_read(struct zl3073x_dev *zldev, u8 index)
{
	return ZL3073X_MB_OP(zldev, output, index, OUTPUT_MB_SEM_RD);
}
EXPORT_SYMBOL_GPL(zl3073x_mb_output_read);

/**
 * zl3073x_mb_output_write - write given output configuration from mailbox
 * @zldev: pointer to device structure
 * @index: DPLL index
 *
 * Writes (commits) configuration of given output from output mailbox.
 *
 * Return: 0 on success, <0 on error
 */
int zl3073x_mb_output_write(struct zl3073x_dev *zldev, u8 index)
{
	return ZL3073X_MB_OP(zldev, output, index, OUTPUT_MB_SEM_WR);
}
EXPORT_SYMBOL_GPL(zl3073x_mb_output_write);

/**
 * zl3073x_mb_ref_read - read given reference configuration to mailbox
 * @zldev: pointer to device structure
 * @index: reference index
 *
 * Reads configuration of given reference into reference mailbox.
 *
 * Return: 0 on success, <0 on error
 */
int zl3073x_mb_ref_read(struct zl3073x_dev *zldev, u8 index)
{
	return ZL3073X_MB_OP(zldev, ref, index, REF_MB_SEM_RD);
}
EXPORT_SYMBOL_GPL(zl3073x_mb_ref_read);

/**
 * zl3073x_mb_ref_write - write given reference configuration from mailbox
 * @zldev: pointer to device structure
 * @index: reference index
 *
 * Writes (commits) configuration of given reference from reference mailbox.
 *
 * Return: 0 on success, <0 on error
 */
int zl3073x_mb_ref_write(struct zl3073x_dev *zldev, u8 index)
{
	return ZL3073X_MB_OP(zldev, ref, index, REF_MB_SEM_WR);
}
EXPORT_SYMBOL_GPL(zl3073x_mb_ref_write);

/**
 * zl3073x_mb_synth_read - read given synth configuration to mailbox
 * @zldev: pointer to device structure
 * @index: synth index
 *
 * Reads configuration of given synth into synth mailbox.
 *
 * Return: 0 on success, <0 on error
 */
int zl3073x_mb_synth_read(struct zl3073x_dev *zldev, u8 index)
{
	return ZL3073X_MB_OP(zldev, synth, index, SYNTH_MB_SEM_RD);
}
EXPORT_SYMBOL_GPL(zl3073x_mb_synth_read);

/**
 * zl3073x_mb_synth_write - write given synth configuration from mailbox
 * @zldev: pointer to device structure
 * @index: synth index
 *
 * Writes (commits) configuration of given synth from reference mailbox.
 *
 * Return: 0 on success, <0 on error
 */
int zl3073x_mb_synth_write(struct zl3073x_dev *zldev, u8 index)
{
	return ZL3073X_MB_OP(zldev, synth, index, SYNTH_MB_SEM_WR);
}
EXPORT_SYMBOL_GPL(zl3073x_mb_synth_write);

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

	guard(zl3073x)(zldev);

	rc = zl3073x_read_id(zldev, &id);
	if (rc)
		return rc;

	snprintf(buf, sizeof(buf), "%X", id);
	rc = devlink_info_version_fixed_put(req,
					    DEVLINK_INFO_VERSION_GENERIC_ASIC_ID,
					    buf);
	if (rc)
		return rc;

	rc = zl3073x_read_revision(zldev, &revision);
	if (rc)
		return rc;

	snprintf(buf, sizeof(buf), "%X", revision);
	rc = devlink_info_version_fixed_put(req,
					    DEVLINK_INFO_VERSION_GENERIC_ASIC_REV,
					    buf);
	if (rc)
		return rc;

	rc = zl3073x_read_fw_ver(zldev, &fw_ver);
	if (rc)
		return rc;

	snprintf(buf, sizeof(buf), "%u", fw_ver);
	rc = devlink_info_version_fixed_put(req,
					    DEVLINK_INFO_VERSION_GENERIC_FW,
					    buf);
	if (rc)
		return rc;

	rc = zl3073x_read_custom_config_ver(zldev, &cfg_ver);
	if (rc)
		return rc;

	/* No custom config version */
	if (cfg_ver == U32_MAX)
		return rc;

	snprintf(buf, sizeof(buf), "%lu.%lu.%lu.%lu",
		 FIELD_GET(GENMASK(31, 24), cfg_ver),
		 FIELD_GET(GENMASK(23, 16), cfg_ver),
		 FIELD_GET(GENMASK(15, 8), cfg_ver),
		 FIELD_GET(GENMASK(7, 0), cfg_ver));

	rc = devlink_info_version_running_put(req, "cfg.custom_ver", buf);

	return rc;
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
 * Return: pointer to zl3073x device structure
 */
struct zl3073x_dev *zl3073x_devm_alloc(struct device *dev)
{
	struct devlink *devlink;

	devlink = devlink_alloc(&zl3073x_devlink_ops,
				sizeof(struct zl3073x_dev), dev);
	if (!devlink)
		return NULL;

	if (devm_add_action_or_reset(dev, zl3073x_devlink_free, devlink))
		return NULL;

	return devlink_priv(devlink);
}
EXPORT_SYMBOL_NS_GPL(zl3073x_devm_alloc, "ZL3073X");

static void zl3073x_devlink_unregister(void *ptr)
{
	devlink_unregister(ptr);
}

/**
 * zl3073x_dev_init - initialize zl3073x device
 * @zldev: pointer to zl3073x device
 *
 * Common initialization of zl3073x device structure.
 *
 * Returns: 0 on success, <0 on error
 */
int zl3073x_dev_init(struct zl3073x_dev *zldev)
{
	u16 id, revision, fw_ver;
	struct devlink *devlink;
	u32 cfg_ver;
	int rc;

	rc = devm_mutex_init(zldev->dev, &zldev->lock);
	if (rc) {
		dev_err_probe(zldev->dev, rc, "Failed to initialize mutex\n");
		return rc;
	}

	/* Take device lock */
	scoped_guard(zl3073x, zldev) {
		rc = zl3073x_read_id(zldev, &id);
		if (rc)
			return rc;
		rc = zl3073x_read_revision(zldev, &revision);
		if (rc)
			return rc;
		rc = zl3073x_read_fw_ver(zldev, &fw_ver);
		if (rc)
			return rc;
		rc = zl3073x_read_custom_config_ver(zldev, &cfg_ver);
		if (rc)
			return rc;
	}

	dev_info(zldev->dev, "ChipID(%X), ChipRev(%X), FwVer(%u)\n",
		 id, revision, fw_ver);
	dev_info(zldev->dev, "Custom config version: %lu.%lu.%lu.%lu\n",
		 FIELD_GET(GENMASK(31, 24), cfg_ver),
		 FIELD_GET(GENMASK(23, 16), cfg_ver),
		 FIELD_GET(GENMASK(15, 8), cfg_ver),
		 FIELD_GET(GENMASK(7, 0), cfg_ver));

	devlink = priv_to_devlink(zldev);
	devlink_register(devlink);

	rc = devm_add_action_or_reset(zldev->dev, zl3073x_devlink_unregister,
				      devlink);
	if (rc)
		return rc;

	return 0;
}
EXPORT_SYMBOL_NS_GPL(zl3073x_dev_init, "ZL3073X");

MODULE_AUTHOR("Ivan Vecera <ivecera@redhat.com>");
MODULE_DESCRIPTION("Microchip ZL3073x core driver");
MODULE_LICENSE("GPL");
