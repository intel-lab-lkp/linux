// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SIUL2(System Integration Unit Lite) MFD driver
 *
 * Copyright 2024 NXP
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/mfd/core.h>
#include <linux/mfd/nxp-siul2.h>

#define S32G_NUM_SIUL2 2

#define S32_REG_RANGE(start, end, name, access)		\
	{						\
		.reg_name = (name),			\
		.reg_start_offset = (start),		\
		.reg_end_offset = (end),		\
		.reg_access = (access),			\
		.valid = true,				\
	}

#define S32_INVALID_REG_RANGE		\
	{				\
		.reg_name = NULL,	\
		.reg_access = NULL,	\
		.valid = false,		\
	}

static const struct mfd_cell nxp_siul2_devs[] = {
	{
		.name = "s32g-siul2-pinctrl",
	}
};

/**
 * struct nxp_siul2_reg_range_info: a register range in SIUL2
 * @reg_start_offset: the first valid register offset
 * @reg_end_offset: the last valid register offset
 * @reg_access: the read/write access tables if not NULL
 * @valid: whether the register range is valid or not
 */
struct nxp_siul2_reg_range_info {
	const char *reg_name;
	unsigned int reg_start_offset;
	unsigned int reg_end_offset;
	const struct regmap_access_table *reg_access;
	bool valid;
};

static const struct regmap_range s32g2_siul2_0_imcr_reg_ranges[] = {
	/* IMCR0 - IMCR1 */
	regmap_reg_range(0, 4),
	/* IMCR3 - IMCR61 */
	regmap_reg_range(0xC, 0xF4),
	/* IMCR68 - IMCR83 */
	regmap_reg_range(0x110, 0x14C)
};

static const struct regmap_access_table s32g2_siul2_0_imcr = {
	.yes_ranges = s32g2_siul2_0_imcr_reg_ranges,
	.n_yes_ranges = ARRAY_SIZE(s32g2_siul2_0_imcr_reg_ranges)
};

static const struct regmap_range s32g2_siul2_0_pgpd_reg_ranges[] = {
	/* PGPD*0 - PGPD*5 */
	regmap_reg_range(0, 0xA),
	/* PGPD*6 - PGPD*6 */
	regmap_reg_range(0xE, 0xE),
};

static const struct regmap_access_table s32g2_siul2_0_pgpd = {
	.yes_ranges = s32g2_siul2_0_pgpd_reg_ranges,
	.n_yes_ranges = ARRAY_SIZE(s32g2_siul2_0_pgpd_reg_ranges)
};

static const struct regmap_range s32g2_siul2_1_irq_reg_ranges[] = {
	/* DISR0 */
	regmap_reg_range(0x10, 0x10),
	/* DIRER0 */
	regmap_reg_range(0x18, 0x18),
	/* DIRSR0 */
	regmap_reg_range(0x20, 0x20),
	/* IREER0 */
	regmap_reg_range(0x28, 0x28),
	/* IFEER0 */
	regmap_reg_range(0x30, 0x30),
};

static const struct regmap_access_table s32g2_siul2_1_irq = {
	.yes_ranges = s32g2_siul2_1_irq_reg_ranges,
	.n_yes_ranges = ARRAY_SIZE(s32g2_siul2_1_irq_reg_ranges),
};

static const struct regmap_range s32g2_siul2_1_irq_volatile_reg_range[] = {
	/* DISR0 */
	regmap_reg_range(0x10, 0x10)
};

static const struct regmap_access_table s32g2_siul2_1_irq_volatile = {
	.yes_ranges = s32g2_siul2_1_irq_volatile_reg_range,
	.n_yes_ranges = ARRAY_SIZE(s32g2_siul2_1_irq_volatile_reg_range),
};

static const struct regmap_range s32g2_siul2_1_mscr_reg_ranges[] = {
	/* MSCR112 - MSCR122 */
	regmap_reg_range(0, 0x28),
	/* MSCR144 - MSCR190 */
	regmap_reg_range(0x80, 0x138)
};

static const struct regmap_access_table s32g2_siul2_1_mscr = {
	.yes_ranges = s32g2_siul2_1_mscr_reg_ranges,
	.n_yes_ranges = ARRAY_SIZE(s32g2_siul2_1_mscr_reg_ranges),
};

static const struct regmap_range s32g2_siul2_1_imcr_reg_ranges[] = {
	/* IMCR119 - IMCR121 */
	regmap_reg_range(0, 8),
	/* IMCR128 - IMCR129 */
	regmap_reg_range(0x24, 0x28),
	/* IMCR143 - IMCR151 */
	regmap_reg_range(0x60, 0x80),
	/* IMCR153 - IMCR161 */
	regmap_reg_range(0x88, 0xA8),
	/* IMCR205 - IMCR212 */
	regmap_reg_range(0x158, 0x174),
	/* IMCR224 - IMCR225 */
	regmap_reg_range(0x1A4, 0x1A8),
	/* IMCR233 - IMCR248 */
	regmap_reg_range(0x1C8, 0x204),
	/* IMCR273 - IMCR274 */
	regmap_reg_range(0x268, 0x26C),
	/* IMCR278 - IMCR281 */
	regmap_reg_range(0x27C, 0x288),
	/* IMCR283 - IMCR286 */
	regmap_reg_range(0x290, 0x29C),
	/* IMCR288 - IMCR294 */
	regmap_reg_range(0x2A4, 0x2BC),
	/* IMCR296 - IMCR302 */
	regmap_reg_range(0x2C4, 0x2DC),
	/* IMCR304 - IMCR310 */
	regmap_reg_range(0x2E4, 0x2FC),
	/* IMCR312 - IMCR314 */
	regmap_reg_range(0x304, 0x30C),
	/* IMCR316 */
	regmap_reg_range(0x314, 0x314),
	/* IMCR 318 */
	regmap_reg_range(0x31C, 0x31C),
	/* IMCR322 - IMCR340 */
	regmap_reg_range(0x32C, 0x374),
	/* IMCR343 - IMCR360 */
	regmap_reg_range(0x380, 0x3C4),
	/* IMCR363 - IMCR380 */
	regmap_reg_range(0x3D0, 0x414),
	/* IMCR383 - IMCR393 */
	regmap_reg_range(0x420, 0x448),
	/* IMCR398 - IMCR433 */
	regmap_reg_range(0x45C, 0x4E8),
	/* IMCR467 - IMCR470 */
	regmap_reg_range(0x570, 0x57C),
	/* IMCR473 - IMCR475 */
	regmap_reg_range(0x588, 0x590),
	/* IMCR478 - IMCR480*/
	regmap_reg_range(0x59C, 0x5A4),
	/* IMCR483 - IMCR485 */
	regmap_reg_range(0x5B0, 0x5B8),
	/* IMCR488 - IMCR490 */
	regmap_reg_range(0x5C4, 0x5CC),
	/* IMCR493 - IMCR495 */
	regmap_reg_range(0x5D8, 0x5E0),
};

static const struct regmap_access_table s32g2_siul2_1_imcr = {
	.yes_ranges = s32g2_siul2_1_imcr_reg_ranges,
	.n_yes_ranges = ARRAY_SIZE(s32g2_siul2_1_imcr_reg_ranges)
};

static const struct regmap_range s32g2_siul2_1_pgpd_reg_ranges[] = {
	/* PGPD*7 */
	regmap_reg_range(0xC, 0xC),
	/* PGPD*9 */
	regmap_reg_range(0x10, 0x10),
	/* PDPG*10 - PGPD*11 */
	regmap_reg_range(0x14, 0x16),
};

static const struct regmap_access_table s32g2_siul2_1_pgpd = {
	.yes_ranges = s32g2_siul2_1_pgpd_reg_ranges,
	.n_yes_ranges = ARRAY_SIZE(s32g2_siul2_1_pgpd_reg_ranges)
};

static const struct nxp_siul2_reg_range_info
s32g2_reg_ranges[S32G_NUM_SIUL2][SIUL2_NUM_REG_TYPES] = {
	/* SIUL2_0 */
	{
		[SIUL2_MPIDR] = S32_REG_RANGE(4, 8, "SIUL2_0_MPIDR", NULL),
		/* Interrupts are to be controlled from SIUL2_1 */
		[SIUL2_IRQ] = S32_INVALID_REG_RANGE,
		[SIUL2_MSCR] = S32_REG_RANGE(0x240, 0x3D4, "SIUL2_0_MSCR",
					     NULL),
		[SIUL2_IMCR] = S32_REG_RANGE(0xA40, 0xB8C, "SIUL2_0_IMCR",
					     &s32g2_siul2_0_imcr),
		[SIUL2_PGPDO] = S32_REG_RANGE(0x1700, 0x170E,
					      "SIUL2_0_PGPDO",
					      &s32g2_siul2_0_pgpd),
		[SIUL2_PGPDI] = S32_REG_RANGE(0x1740, 0x174E,
					      "SIUL2_0_PGPDI",
					      &s32g2_siul2_0_pgpd),
	},
	/* SIUL2_1 */
	{
		[SIUL2_MPIDR] = S32_REG_RANGE(4, 8, "SIUL2_1_MPIDR", NULL),
		[SIUL2_IRQ] = S32_REG_RANGE(0x10, 0xC0, "SIUL2_1_IRQ",
					    &s32g2_siul2_1_irq),
		[SIUL2_MSCR] = S32_REG_RANGE(0x400, 0x538, "SIUL2_1_MSCR",
					     &s32g2_siul2_1_mscr),
		[SIUL2_IMCR] = S32_REG_RANGE(0xC1C, 0x11FC, "SIUL2_1_IMCR",
					     &s32g2_siul2_1_imcr),
		[SIUL2_PGPDO] = S32_REG_RANGE(0x1700, 0x1716,
					      "SIUL2_1_PGPDO",
					      &s32g2_siul2_1_pgpd),
		[SIUL2_PGPDI] = S32_REG_RANGE(0x1740, 0x1756,
					      "SIUL2_1_PGPDI",
					      &s32g2_siul2_1_pgpd),
	},
};

static const struct regmap_config nxp_siul2_regmap_irq_conf = {
	.val_bits = 32,
	.val_format_endian = REGMAP_ENDIAN_LITTLE,
	.reg_bits = 32,
	.reg_stride = 4,
	.cache_type = REGCACHE_FLAT,
	.use_raw_spinlock = true,
	.volatile_table = &s32g2_siul2_1_irq_volatile,
};

static const struct regmap_config nxp_siul2_regmap_generic_conf = {
	.val_bits = 32,
	.val_format_endian = REGMAP_ENDIAN_LITTLE,
	.reg_bits = 32,
	.reg_stride = 4,
	.cache_type = REGCACHE_FLAT,
	.use_raw_spinlock = true,
};

static const struct regmap_config nxp_siul2_regmap_pgpdo_conf = {
	.val_bits = 16,
	.val_format_endian = REGMAP_ENDIAN_LITTLE,
	.reg_bits = 32,
	.reg_stride = 2,
	.cache_type = REGCACHE_FLAT,
	.use_raw_spinlock = true,
};

static const struct regmap_config nxp_siul2_regmap_pgpdi_conf = {
	.val_bits = 16,
	.val_format_endian = REGMAP_ENDIAN_LITTLE,
	.reg_bits = 32,
	.reg_stride = 2,
	.cache_type = REGCACHE_NONE,
	.use_raw_spinlock = true,
};

static int nxp_siul2_init_regmap(struct platform_device *pdev,
				 void __iomem *base, int siul)
{
	struct regmap_config regmap_configs[SIUL2_NUM_REG_TYPES] = {
		[SIUL2_MPIDR]	= nxp_siul2_regmap_generic_conf,
		[SIUL2_IRQ]	= nxp_siul2_regmap_irq_conf,
		[SIUL2_MSCR]	= nxp_siul2_regmap_generic_conf,
		[SIUL2_IMCR]	= nxp_siul2_regmap_generic_conf,
		[SIUL2_PGPDO]	= nxp_siul2_regmap_pgpdo_conf,
		[SIUL2_PGPDI]	= nxp_siul2_regmap_pgpdi_conf,
	};
	const struct nxp_siul2_reg_range_info *tmp_range;
	struct regmap_config *tmp_conf;
	struct nxp_siul2_info *info;
	struct nxp_siul2_mfd *priv;
	void __iomem *reg_start;
	int i, ret;

	priv = platform_get_drvdata(pdev);
	info = &priv->siul2[siul];

	for (i = 0; i < SIUL2_NUM_REG_TYPES; i++) {
		if (!s32g2_reg_ranges[siul][i].valid)
			continue;

		tmp_range = &s32g2_reg_ranges[siul][i];
		tmp_conf = &regmap_configs[i];
		tmp_conf->name = tmp_range->reg_name;
		tmp_conf->max_register =
			tmp_range->reg_end_offset - tmp_range->reg_start_offset;

		if (tmp_conf->cache_type != REGCACHE_NONE)
			tmp_conf->num_reg_defaults_raw =
				tmp_conf->max_register / tmp_conf->reg_stride;

		if (tmp_range->reg_access) {
			tmp_conf->wr_table = tmp_range->reg_access;
			tmp_conf->rd_table = tmp_range->reg_access;
		}

		reg_start = base + tmp_range->reg_start_offset;
		info->regmaps[i] = devm_regmap_init_mmio(&pdev->dev, reg_start,
							 tmp_conf);
		if (IS_ERR(info->regmaps[i])) {
			dev_err(&pdev->dev, "regmap %d init failed: %d\n", i,
				ret);
			return PTR_ERR(info->regmaps[i]);
		}
	}

	return 0;
}

static int nxp_siul2_parse_dtb(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct of_phandle_args pinspec;
	struct nxp_siul2_mfd *priv;
	void __iomem *base;
	char reg_name[16];
	int i, ret;

	priv = platform_get_drvdata(pdev);

	for (i = 0; i < priv->num_siul2; i++) {
		ret = snprintf(reg_name, ARRAY_SIZE(reg_name), "siul2%d", i);
		if (ret < 0 || ret >= ARRAY_SIZE(reg_name))
			return ret;

		base = devm_platform_ioremap_resource_byname(pdev, reg_name);
		if (IS_ERR(base)) {
			dev_err(&pdev->dev, "Failed to get MEM resource: %s\n",
				reg_name);
			return PTR_ERR(base);
		}

		ret = nxp_siul2_init_regmap(pdev, base, i);
		if (ret)
			return ret;

		ret = of_parse_phandle_with_fixed_args(np, "gpio-ranges", 3,
						       i, &pinspec);
		if (ret)
			return ret;

		of_node_put(pinspec.np);

		if (pinspec.args_count != 3) {
			dev_err(&pdev->dev, "Invalid pinspec count: %d\n",
				pinspec.args_count);
			return -EINVAL;
		}

		priv->siul2[i].gpio_base = pinspec.args[1];
		priv->siul2[i].gpio_num = pinspec.args[2];
	}

	return 0;
}

static int nxp_siul2_probe(struct platform_device *pdev)
{
	struct nxp_siul2_mfd *priv;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->num_siul2 = S32G_NUM_SIUL2;
	priv->siul2 = devm_kcalloc(&pdev->dev, priv->num_siul2,
				   sizeof(*priv->siul2), GFP_KERNEL);
	if (!priv->siul2)
		return -ENOMEM;

	platform_set_drvdata(pdev, priv);
	ret = nxp_siul2_parse_dtb(pdev);
	if (ret)
		return ret;

	return devm_mfd_add_devices(&pdev->dev, PLATFORM_DEVID_AUTO,
				    nxp_siul2_devs, ARRAY_SIZE(nxp_siul2_devs),
				    NULL, 0, NULL);
}

static const struct of_device_id nxp_siul2_dt_ids[] = {
	{ .compatible = "nxp,s32g2-siul2" },
	{ .compatible = "nxp,s32g3-siul2" },
	{ },
};
MODULE_DEVICE_TABLE(of, nxp_siul2_dt_ids);

static struct platform_driver nxp_siul2_mfd_driver = {
	.driver = {
		.name		= "nxp-siul2-mfd",
		.of_match_table	= nxp_siul2_dt_ids,
	},
	.probe = nxp_siul2_probe,
};

module_platform_driver(nxp_siul2_mfd_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("NXP SIUL2 MFD driver");
MODULE_AUTHOR("Andrei Stefanescu <andrei.stefanescu@oss.nxp.com>");
