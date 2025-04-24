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

static int
zl3073x_write_reg(struct zl3073x_dev *zldev, unsigned int reg, const void *val)
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

	len = ZL_REG_SIZE(reg);
	switch (len) {
	case 1:
		buf[0] = *(u8 *)val;
		break;
	case 2:
		put_unaligned_be16(*(u16 *)val, buf);
		break;
	case 4:
		put_unaligned_be32(*(u32 *)val, buf);
		break;
	case 6:
		put_unaligned_be48(*(u64 *)val, buf);
		break;
	default:
		dev_err(zldev->dev, "Invalid reg-width %u for reg 0x%04lx\n",
			len, ZL_REG_ADDR(reg));
		return -EINVAL;
	}

	/* Map the register address to virtual range */
	reg = ZL_REG_ADDR(reg) + ZL_RANGE_OFFSET;

	rc = regmap_bulk_write(zldev->regmap, reg, buf, len);
	if (rc) {
		dev_err(zldev->dev, "Failed to write reg 0x%04x: %pe\n", reg,
			ERR_PTR(rc));
		return rc;
	}

	return rc;
}

static int
zl3073x_wait_reg_zero_bits(struct zl3073x_dev *zldev, unsigned int reg, u8 mask)
{
	/* Register polling sleep & timeout */
#define ZL_POLL_SLEEP_US   10
#define ZL_POLL_TIMEOUT_US 2000000
	unsigned int val;

	/* Only 8bit registers are supported */
	BUILD_BUG_ON(ZL_REG_SIZE(reg) != 1);

	/* Map the register address to virtual range for polling */
	reg = ZL_REG_ADDR(reg) + ZL_RANGE_OFFSET;

	return regmap_read_poll_timeout(zldev->regmap, reg, val, !(val & mask),
					ZL_POLL_SLEEP_US, ZL_POLL_TIMEOUT_US);
}

static int
zl3073x_mb_cmd_do(struct zl3073x_dev *zldev, unsigned int cmd_reg, u8 cmd,
		  unsigned int mask_reg, u16 mask)
{
	int rc;

	rc = zl3073x_write_reg(zldev, mask_reg, &mask);
	if (rc)
		return rc;

	rc = zl3073x_write_reg(zldev, cmd_reg, &cmd);
	if (rc)
		return rc;

	/* Wait for the command to finish */
	return zl3073x_wait_reg_zero_bits(zldev, cmd_reg, cmd);
}

/**
 * zl3073x_mb_dpll_read - read given DPLL configuration to mailbox
 * @zldev: pointer to device structure
 * @index: DPLL index
 * @fields: mask of the mailbox fields to be filled
 * @mb: DPLL mailbox
 *
 * Reads selected configuration of given reference into output mailbox.
 *
 * Return: 0 on success, <0 on error
 */
int zl3073x_mb_dpll_read(struct zl3073x_dev *zldev, u8 index, u32 fields,
			 struct zl3073x_mb_dpll *mb)
{
	int i, rc;

	rc = zl3073x_mb_cmd_do(zldev, ZL_REG_DPLL_MB_SEM, ZL_DPLL_MB_SEM_RD,
			       ZL_REG_DPLL_MB_MASK, BIT(index));
	if (rc)
		return rc;

	for (i = 0; i < ARRAY_SIZE(mb->ref_prio); i++) {
		if (fields & BIT(i)) {
			rc = zl3073x_read_reg(zldev, ZL_REG_DPLL_REF_PRIO(i),
					      &mb->ref_prio[i]);
			if (rc)
				break;
		}
	}

	return rc;
}
EXPORT_SYMBOL_NS_GPL(zl3073x_mb_dpll_read, "ZL3073X");

/**
 * zl3073x_mb_dpll_write - write given DPLL configuration from mailbox
 * @zldev: pointer to device structure
 * @index: DPLL index
 * @fields: mask of the mailbox fields to be written
 * @mb: DPLL channel mailbox
 *
 * Writes selected fields from the mailbox into device.
  *
 * Return: 0 on success, <0 on error
 */
int zl3073x_mb_dpll_write(struct zl3073x_dev *zldev, u8 index, u32 fields,
			  struct zl3073x_mb_dpll *mb)
{
	int i, rc;

	for (i = 0; i < ARRAY_SIZE(mb->ref_prio); i++) {
		if (fields & BIT(i)) {
			rc = zl3073x_write_reg(zldev, ZL_REG_DPLL_REF_PRIO(i),
					       &mb->ref_prio[i]);
			if (rc)
				break;
		}
	}

	return zl3073x_mb_cmd_do(zldev, ZL_REG_DPLL_MB_SEM, ZL_DPLL_MB_SEM_WR,
				 ZL_REG_DPLL_MB_MASK, BIT(index));
}
EXPORT_SYMBOL_NS_GPL(zl3073x_mb_dpll_write, "ZL3073X");

/**
 * zl3073x_mb_output_read - read given output configuration to mailbox
 * @zldev: pointer to device structure
 * @index: output index
 * @fields: mask of the mailbox fields to be filled
 * @mb: output mailbox
 *
 * Reads selected configuration of given reference into output mailbox.
 *
 * Return: 0 on success, <0 on error
 */
int zl3073x_mb_output_read(struct zl3073x_dev *zldev, u8 index, u32 fields,
			   struct zl3073x_mb_output *mb)
{
	int rc;

	rc = zl3073x_mb_cmd_do(zldev, ZL_REG_OUTPUT_MB_SEM, ZL_OUTPUT_MB_SEM_RD,
			       ZL_REG_OUTPUT_MB_MASK, BIT(index));

	if (!rc && (fields & ZL3073X_MB_OUTPUT_MODE))
		rc = zl3073x_read_reg(zldev, ZL_REG_OUTPUT_MODE, &mb->mode);

	if (!rc && (fields & ZL3073X_MB_OUTPUT_DIV))
		rc = zl3073x_read_reg(zldev, ZL_REG_OUTPUT_DIV, &mb->div);

	if (!rc && (fields & ZL3073X_MB_OUTPUT_WIDTH))
		rc = zl3073x_read_reg(zldev, ZL_REG_OUTPUT_WIDTH, &mb->width);

	if (!rc && (fields & ZL3073X_MB_OUTPUT_ESYNC_PERIOD))
		rc = zl3073x_read_reg(zldev, ZL_REG_OUTPUT_ESYNC_PERIOD,
				      &mb->esync_period);

	if (!rc && (fields & ZL3073X_MB_OUTPUT_ESYNC_WIDTH))
		rc = zl3073x_read_reg(zldev, ZL_REG_OUTPUT_ESYNC_WIDTH,
				      &mb->esync_width);

	if (!rc && (fields & ZL3073X_MB_OUTPUT_PHASE_COMP))
		rc = zl3073x_read_reg(zldev, ZL_REG_OUTPUT_PHASE_COMP,
				      &mb->phase_comp);

	return rc;
}
EXPORT_SYMBOL_NS_GPL(zl3073x_mb_output_read, "ZL3073X");

/**
 * zl3073x_mb_output_write - write given output configuration from mailbox
 * @zldev: pointer to device structure
 * @index: output index
 * @fields: mask of the mailbox fields to be written
 * @mb: output mailbox
 *
 * Writes selected fields from the mailbox into device.
 *
 * Return: 0 on success, <0 on error
 */
int zl3073x_mb_output_write(struct zl3073x_dev *zldev, u8 index, u32 fields,
			    const struct zl3073x_mb_output *mb)
{
	int rc = 0;

	if (fields & ZL3073X_MB_OUTPUT_MODE)
		rc = zl3073x_write_reg(zldev, ZL_REG_OUTPUT_MODE, &mb->mode);

	if (!rc && (fields & ZL3073X_MB_OUTPUT_DIV))
		rc = zl3073x_write_reg(zldev, ZL_REG_OUTPUT_DIV, &mb->div);

	if (!rc && (fields & ZL3073X_MB_OUTPUT_WIDTH))
		rc = zl3073x_write_reg(zldev, ZL_REG_OUTPUT_WIDTH, &mb->width);

	if (!rc && (fields & ZL3073X_MB_OUTPUT_ESYNC_PERIOD))
		rc = zl3073x_write_reg(zldev, ZL_REG_OUTPUT_ESYNC_PERIOD,
				       &mb->esync_period);

	if (!rc && (fields & ZL3073X_MB_OUTPUT_ESYNC_WIDTH))
		rc = zl3073x_write_reg(zldev, ZL_REG_OUTPUT_ESYNC_WIDTH,
				       &mb->esync_width);

	if (!rc && (fields & ZL3073X_MB_OUTPUT_PHASE_COMP))
		rc = zl3073x_write_reg(zldev, ZL_REG_OUTPUT_PHASE_COMP,
				       &mb->phase_comp);
	if (rc)
		return rc;

	return zl3073x_mb_cmd_do(zldev,
				 ZL_REG_OUTPUT_MB_SEM, ZL_OUTPUT_MB_SEM_WR,
				 ZL_REG_OUTPUT_MB_MASK, BIT(index));
}
EXPORT_SYMBOL_NS_GPL(zl3073x_mb_output_write, "ZL3073X");

/**
 * zl3073x_mb_ref_read - read given reference configuration to mailbox
 * @zldev: pointer to device structure
 * @index: reference index
 * @fields: mask of the mailbox fields to be filled
 * @mb: reference mailbox
 *
 * Reads selected configuration of given reference into ref mailbox.
 *
 * Return: 0 on success, <0 on error
 */
int zl3073x_mb_ref_read(struct zl3073x_dev *zldev, u8 index, u32 fields,
			struct zl3073x_mb_ref *mb)
{
	int rc;

	rc = zl3073x_mb_cmd_do(zldev, ZL_REG_REF_MB_SEM, ZL_REF_MB_SEM_RD,
			       ZL_REG_REF_MB_MASK, BIT(index));

	if (!rc && (fields & ZL3073X_MB_REF_FREQ_BASE))
		rc = zl3073x_read_reg(zldev, ZL_REG_REF_FREQ_BASE,
				      &mb->freq_base);

	if (!rc && (fields & ZL3073X_MB_REF_FREQ_MULT))
		rc = zl3073x_read_reg(zldev, ZL_REG_REF_FREQ_MULT,
				      &mb->freq_mult);

	if (!rc && (fields & ZL3073X_MB_REF_RATIO_M))
		rc = zl3073x_read_reg(zldev, ZL_REG_REF_RATIO_M, &mb->ratio_m);

	if (!rc && (fields & ZL3073X_MB_REF_RATIO_N))
		rc = zl3073x_read_reg(zldev, ZL_REG_REF_RATIO_N, &mb->ratio_n);

	if (!rc && (fields & ZL3073X_MB_REF_CONFIG))
		rc = zl3073x_read_reg(zldev, ZL_REG_REF_CONFIG, &mb->config);

	if (!rc && (fields & ZL3073X_MB_REF_PHASE_OFFSET_COMP))
		rc = zl3073x_read_reg(zldev, ZL_REG_REF_PHASE_OFFSET_COMP,
				      &mb->phase_offset_comp);

	if (!rc && (fields & ZL3073X_MB_REF_SYNC_CTRL))
		rc = zl3073x_read_reg(zldev, ZL_REG_REF_SYNC_CTRL,
				      &mb->sync_ctrl);

	if (!rc && (fields & ZL3073X_MB_REF_ESYNC_DIV))
		rc = zl3073x_read_reg(zldev, ZL_REG_REF_ESYNC_DIV,
				      &mb->esync_div);

	return rc;
}
EXPORT_SYMBOL_NS_GPL(zl3073x_mb_ref_read, "ZL3073X");

/**
 * zl3073x_mb_ref_write - write given reference configuration from mailbox
 * @zldev: pointer to device structure
 * @index: reference index
 * @fields: mask of the mailbox fields to be written
 * @mb: reference mailbox
 *
 * Writes selected fields from the mailbox into device.
 *
 * Return: 0 on success, <0 on error
 */
int zl3073x_mb_ref_write(struct zl3073x_dev *zldev, u8 index, u32 fields,
			 const struct zl3073x_mb_ref *mb)
{
	int rc = 0;

	if (fields & ZL3073X_MB_REF_FREQ_BASE)
		rc = zl3073x_write_reg(zldev, ZL_REG_REF_FREQ_BASE,
				       &mb->freq_base);

	if (!rc && (fields & ZL3073X_MB_REF_FREQ_MULT))
		rc = zl3073x_write_reg(zldev, ZL_REG_REF_FREQ_MULT,
				       &mb->freq_mult);

	if (!rc && (fields & ZL3073X_MB_REF_RATIO_M))
		rc = zl3073x_write_reg(zldev, ZL_REG_REF_RATIO_M,
				       &mb->ratio_m);

	if (!rc && (fields & ZL3073X_MB_REF_RATIO_N))
		rc = zl3073x_write_reg(zldev, ZL_REG_REF_RATIO_N,
				       &mb->ratio_n);

	if (!rc && (fields & ZL3073X_MB_REF_CONFIG))
		rc = zl3073x_write_reg(zldev, ZL_REG_REF_CONFIG, &mb->config);

	if (!rc && (fields & ZL3073X_MB_REF_PHASE_OFFSET_COMP))
		rc = zl3073x_write_reg(zldev, ZL_REG_REF_PHASE_OFFSET_COMP,
				       &mb->phase_offset_comp);

	if (!rc && (fields & ZL3073X_MB_REF_SYNC_CTRL))
		rc = zl3073x_write_reg(zldev, ZL_REG_REF_SYNC_CTRL,
				       &mb->sync_ctrl);

	if (!rc && (fields & ZL3073X_MB_REF_ESYNC_DIV))
		rc = zl3073x_write_reg(zldev, ZL_REG_REF_ESYNC_DIV,
				       &mb->esync_div);

	if (rc)
		return rc;

	return zl3073x_mb_cmd_do(zldev, ZL_REG_REF_MB_SEM, ZL_REF_MB_SEM_WR,
				 ZL_REG_REF_MB_MASK, BIT(index));
}
EXPORT_SYMBOL_NS_GPL(zl3073x_mb_ref_write, "ZL3073X");

/**
 * zl3073x_mb_synth_read - read given synth configuration to mailbox
 * @zldev: pointer to device structure
 * @index: synth index
 * @fields: mask of the mailbox fields to be filled
 * @mb: synth mailbox
 *
 * Reads selected configuration of given reference into synth mailbox.
 *
 * Return: 0 on success, <0 on error
 */
int zl3073x_mb_synth_read(struct zl3073x_dev *zldev, u8 index, u32 fields,
			  struct zl3073x_mb_synth *mb)
{
	int rc;

	rc = zl3073x_mb_cmd_do(zldev, ZL_REG_SYNTH_MB_SEM, ZL_SYNTH_MB_SEM_RD,
			       ZL_REG_SYNTH_MB_MASK, BIT(index));

	if (!rc && (fields & ZL3073X_MB_SYNTH_FREQ_BASE))
		rc = zl3073x_read_reg(zldev, ZL_REG_SYNTH_FREQ_BASE,
				      &mb->freq_base);

	if (!rc && (fields & ZL3073X_MB_SYNTH_FREQ_MULT))
		rc = zl3073x_read_reg(zldev, ZL_REG_SYNTH_FREQ_MULT,
				      &mb->freq_mult);

	if (!rc && (fields & ZL3073X_MB_SYNTH_FREQ_M))
		rc = zl3073x_read_reg(zldev, ZL_REG_SYNTH_FREQ_M, &mb->freq_m);

	if (!rc && (fields & ZL3073X_MB_SYNTH_FREQ_N))
		rc = zl3073x_read_reg(zldev, ZL_REG_SYNTH_FREQ_N, &mb->freq_n);

	return rc;
}
EXPORT_SYMBOL_NS_GPL(zl3073x_mb_synth_read, "ZL3073X");

/**
 * zl3073x_mb_synth_write - write given synth configuration from mailbox
 * @zldev: pointer to device structure
 * @index: synth index
 * @fields: mask of the mailbox fields to be written
 * @mb: synth mailbox
 *
 * Writes selected fields from the mailbox into device.
 *
 * Return: 0 on success, <0 on error
 */
int zl3073x_mb_synth_write(struct zl3073x_dev *zldev, u8 index, u32 fields,
			   struct zl3073x_mb_synth *mb)
{
	int rc = 0;

	if (fields & ZL3073X_MB_SYNTH_FREQ_BASE)
		rc = zl3073x_write_reg(zldev, ZL_REG_SYNTH_FREQ_BASE,
				       &mb->freq_base);

	if (!rc && (fields & ZL3073X_MB_SYNTH_FREQ_MULT))
		rc = zl3073x_write_reg(zldev, ZL_REG_SYNTH_FREQ_MULT,
				       &mb->freq_mult);

	if (!rc && (fields & ZL3073X_MB_SYNTH_FREQ_M))
		rc = zl3073x_write_reg(zldev, ZL_REG_SYNTH_FREQ_M, &mb->freq_m);

	if (!rc && (fields & ZL3073X_MB_SYNTH_FREQ_N))
		rc = zl3073x_write_reg(zldev, ZL_REG_SYNTH_FREQ_N, &mb->freq_n);

	if (rc)
		return rc;

	return zl3073x_mb_cmd_do(zldev, ZL_REG_SYNTH_MB_SEM, ZL_SYNTH_MB_SEM_WR,
				 ZL_REG_SYNTH_MB_MASK, BIT(index));
}
EXPORT_SYMBOL_NS_GPL(zl3073x_mb_synth_write, "ZL3073X");

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
	struct zl3073x_mb_ref ref;
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

	/* Read reference configuration into mailbox */
	rc = zl3073x_mb_ref_read(zldev, index, ZL3073X_MB_REF_CONFIG, &ref);
	if (rc)
		return rc;

	input->enabled = FIELD_GET(ZL_REF_CONFIG_ENABLE, ref.config);
	input->diff = FIELD_GET(ZL_REF_CONFIG_DIFF_EN, ref.config);

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
	struct zl3073x_mb_output mb;
	u8 output_ctrl;
	int rc;

	output = &zldev->output[index];

	/* Read output control register */
	rc = zl3073x_read_reg(zldev, ZL_REG_OUTPUT_CTRL(index), &output_ctrl);
	if (rc)
		return rc;

	/* Store info about output enablement and synthesizer the output
	 * is connected to.
	 */
	output->enabled = FIELD_GET(ZL_OUTPUT_CTRL_EN, output_ctrl);
	output->synth = FIELD_GET(ZL_OUTPUT_CTRL_SYNTH_SEL, output_ctrl);

	dev_dbg(zldev->dev, "OUTPUT%u is %s, connected to SYNTH%u\n",
		index, output->enabled ? "enabled" : "disabled", output->synth);

	/* Read output config mailbox */
	rc = zl3073x_mb_output_read(zldev, index, ZL3073X_MB_OUTPUT_MODE, &mb);
	if (rc)
		return rc;

	/* Extract and store output signal format */
	output->signal_format = FIELD_GET(ZL_OUTPUT_MODE_SIGNAL_FORMAT,
					  mb.mode);

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
	struct zl3073x_mb_synth mb;
	u8 synth_ctrl;
	int rc;

	/* Read synth control register */
	rc = zl3073x_read_reg(zldev, ZL_REG_SYNTH_CTRL(index), &synth_ctrl);
	if (rc)
		return rc;

	/* Extract and store DPLL channel the synth is driven by */
	zldev->synth[index].dpll = FIELD_GET(ZL_SYNTH_CTRL_DPLL_SEL,
					     synth_ctrl);

	dev_dbg(zldev->dev, "SYNTH%u is connected to DPLL%u\n", index,
		zldev->synth[index].dpll);

	/* Read synth configuration into mailbox */
	rc = zl3073x_mb_synth_read(zldev, index,
				   ZL3073X_MB_SYNTH_FREQ_BASE	|
				   ZL3073X_MB_SYNTH_FREQ_MULT	|
				   ZL3073X_MB_SYNTH_FREQ_M	|
				   ZL3073X_MB_SYNTH_FREQ_N, &mb);
	if (rc)
		return rc;

	/* The output frequency is determined by the following formula:
	 * base * multiplier * numerator / denominator
	 */

	/* Check denominator for zero to avoid div by 0 */
	if (!mb.freq_n) {
		dev_err(zldev->dev,
			"Zero divisor for SYNTH%u retrieved from device\n",
			index);
		return -EINVAL;
	}

	/* Compute and store synth frequency */
	zldev->synth[index].freq = mul_u64_u32_div(mul_u32_u32(mb.freq_base,
							       mb.freq_mult),
						   mb.freq_m, mb.freq_n);

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

	/* Use chip ID and given dev ID as clock ID */
	zldev->clock_id = ((u64)id << 8) | dev_id;

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
