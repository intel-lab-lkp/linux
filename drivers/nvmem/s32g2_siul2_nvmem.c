// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2021-2025 NXP
 */

#include <linux/mfd/nxp-siul2.h>
#include <linux/module.h>
#include <linux/nvmem-provider.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

/* SoC revision */
#define NVRAM_CELL_SIZE			4
#define SIUL2_MIDR1_OFF			(0x00000000)
#define SIUL2_MIDR2_OFF			(0x00000004)

#define SIUL20_CELL(c)			(c)
#define SIUL21_CELL(c)			(100u + (c))
#define SOC_MAJOR_CELL_OFFSET		SIUL20_CELL(0)
#define SOC_MINOR_CELL_OFFSET		SIUL20_CELL(1)
#define PCIE_DEV_ID_CELL_OFFSET		SIUL20_CELL(2)
#define SERDES_PRESENCE_CELL_OFFSET	SIUL21_CELL(0)

/* SIUL20_MIDR1 masks */
#define SIUL20_MIDR1_MINOR_MASK		(0xF << 0)
#define SIUL20_MIDR1_MAJOR_SHIFT	(4)
#define SIUL20_MIDR1_MAJOR_MASK		(0xF << SIUL20_MIDR1_MAJOR_SHIFT)
#define SIUL20_MIDR1_PART_NO_SHIFT	(16)
#define SIUL20_MIDR1_PART_NO_MASK	GENMASK(25, 16)

/* SIUL21_MIDR2 masks */
#define SIUL21_MIDR2_SERDES_MASK	BIT(15)

#define SIUL2_QUIRK_MIDR1_DECREMENT_VAL	BIT(1)

struct s32g2_nvmem_drvdata {
	u32 quirks;
};

struct s32g2_siul2_nvmem_data {
	struct device *dev;
	struct nvmem_device *nvmem;
	struct regmap **regmaps;
	struct s32g2_nvmem_drvdata drvdata;
	u8 num_siul2;
};

static int needs_minor_decrement(const struct s32g2_nvmem_drvdata *data)
{
	return data->quirks & SIUL2_QUIRK_MIDR1_DECREMENT_VAL;
}

/* 3 digit part number */
static int get_part_no(struct s32g2_siul2_nvmem_data *priv, u32 *part)
{
	int ret;

	ret = regmap_read(priv->regmaps[0], SIUL2_MIDR1_OFF, part);
	if (ret)
		dev_err(priv->dev, "Failed to read SIUL2 PART_NO!\n");

	*part &= SIUL20_MIDR1_PART_NO_MASK;
	*part >>= SIUL20_MIDR1_PART_NO_SHIFT;

	return ret;
}

static u32 get_variant_bits(u32 value)
{
	/*
	 * Mapping between G3 variant ID and the PCIe Device ID,
	 * as described in the "S32G3 Reference Manual",
	 * chapter SerDes Subsystem, section "Device and revision IDs",
	 * where: index = last 2 digits of the variant
	 *        value = last hex digit of the PCIe Device ID"
	 */
	static const u32 s32g3_variants[] = {
		[78] = 0x6,
		[79] = 0x4,
		[98] = 0x2,
		[99] = 0x0,
	};

	/* PCIe variant bits with respect to PCIe Device ID update
	 * applies only to S32G3 platforms.
	 */
	if (value / 100 != 3)
		return 0;

	value %= 100;

	if (value < ARRAY_SIZE(s32g3_variants))
		return s32g3_variants[value];

	return 0;
}

static int s32g2_siul2_nvmem_read(void *context, unsigned int offset,
				  void *val, size_t bytes)
{
	u32 major, minor, part_no, serdes, midr1, midr2;
	struct s32g2_siul2_nvmem_data *priv = context;
	int ret;

	if (bytes != NVRAM_CELL_SIZE)
		return -EOPNOTSUPP;

	switch (offset) {
	/* SIUL20 cells */
	case SOC_MAJOR_CELL_OFFSET:
		ret = regmap_read(priv->regmaps[0], SIUL2_MIDR1_OFF, &midr1);
		if (ret)
			return ret;
		major = (midr1 & SIUL20_MIDR1_MAJOR_MASK) >> SIUL20_MIDR1_MAJOR_SHIFT;

		/* Bytes format: 0.0.0.MAJOR */
		*(u32 *)val = major + 1;

		return 0;

	case SOC_MINOR_CELL_OFFSET:
		ret = regmap_read(priv->regmaps[0], SIUL2_MIDR1_OFF, &midr1);
		if (ret)
			return ret;

		minor = midr1 & SIUL20_MIDR1_MINOR_MASK;

		if (minor > 0 && needs_minor_decrement(&priv->drvdata))
			minor--;

		/* Bytes format: 0.0.0.MINOR */
		*(u32 *)val = minor;

		return 0;

	case PCIE_DEV_ID_CELL_OFFSET:
		ret = get_part_no(priv, &part_no);
		if (ret)
			return ret;

		/* Bytes format: 0.0.0.PCIE_VARIANT */
		*(u32 *)val = get_variant_bits(part_no);

		return 0;

	/* SIUL21 cells */
	case SERDES_PRESENCE_CELL_OFFSET:
		ret = regmap_read(priv->regmaps[1], SIUL2_MIDR2_OFF, &midr2);
		if (ret)
			return ret;

		serdes = !!(midr2 & SIUL21_MIDR2_SERDES_MASK);
		*(u32 *)val = serdes;
		return 0;

	default:
		return -EOPNOTSUPP;
	}
}

static int s32g2_siul2_nvmem_probe(struct platform_device *pdev)
{
	struct nxp_siul2_mfd *mfd = dev_get_drvdata(pdev->dev.parent);
	struct s32g2_siul2_nvmem_data *priv;
	struct nvmem_config econfig = {
		.name = "s32g2-siul2_nvmem",
		.add_legacy_fixed_of_cells = false,
		.owner = THIS_MODULE,
		.word_size = 4,
		.size = 4,
		.read_only = true,
	};
	int i, ret;
	u32 part;

	if (!mfd)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "Invalid SIUL2 NVMEM parent!\n");

	priv = devm_kzalloc(&pdev->dev, sizeof(struct s32g2_siul2_nvmem_data),
			    GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->num_siul2 = mfd->num_siul2;
	priv->regmaps = devm_kmalloc_array(&pdev->dev, priv->num_siul2,
					   sizeof(*priv->regmaps), GFP_KERNEL);
	if (!priv->regmaps)
		return -ENOMEM;

	for (i = 0; i < priv->num_siul2; i++)
		priv->regmaps[i] = mfd->siul2[i].regmaps[SIUL2_MIDR];

	priv->dev = &pdev->dev;
	econfig.reg_read = s32g2_siul2_nvmem_read;
	econfig.dev = pdev->dev.parent;
	econfig.priv = priv;

	ret = get_part_no(priv, &part);
	if (ret)
		return ret;

	/* S32G2 SoCs have a special case. */
	if (part / 100 == 2)
		priv->drvdata.quirks |= SIUL2_QUIRK_MIDR1_DECREMENT_VAL;

	priv->nvmem = devm_nvmem_register(pdev->dev.parent, &econfig);
	if (IS_ERR(priv->nvmem))
		return dev_err_probe(&pdev->dev, PTR_ERR(priv->nvmem),
				     "Failed to probe SIUL2 NVMEM!\n");

	dev_info(&pdev->dev, "Initialized S32G%u SIUL2 nvmem driver\n",
		 part / 100);

	return 0;
}

static struct platform_driver s32g2_siul2_nvmem_driver = {
	.probe = s32g2_siul2_nvmem_probe,
	.driver = {
		.name = "s32g2-siul2-nvmem",
	},
};

module_platform_driver(s32g2_siul2_nvmem_driver);

MODULE_AUTHOR("Catalin Udma <catalin-dan.udma@nxp.com>");
MODULE_DESCRIPTION("S32G2 SIUL2 NVMEM driver");
MODULE_LICENSE("GPL");
