// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2023-2025 NXP
 */

#include <linux/device.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/nvmem-provider.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>

#define S32G_OCOTP_BANK_OFFSET	512u
#define S32G_OCOTP_BANK_SIZE	32u
#define S32G_OCOTP_WORD_SIZE	4u

struct s32g_fuse {
	u8 bank;
	u8 words_mask;
};

struct s32g_fuse_map {
	const struct s32g_fuse *map;
	size_t n_entries;
};

struct s32g_ocotp_priv {
	struct device *dev;
	void __iomem *base;
	const struct s32g_fuse_map *fuse;
};

static const struct s32g_fuse s32g_map[] = {
	{ .bank = 0,  .words_mask = GENMASK(6, 2) },
	{ .bank = 1,  .words_mask = GENMASK(7, 5) },
	{ .bank = 2,  .words_mask = GENMASK(1, 0) },
	{ .bank = 2,  .words_mask = GENMASK(4, 2) },
	{ .bank = 4,  .words_mask = BIT(6) },
	{ .bank = 5,  .words_mask = BIT(1) },
	{ .bank = 5,  .words_mask = BIT(2) },
	{ .bank = 6,  .words_mask = BIT(7) },
	{ .bank = 7,  .words_mask = GENMASK(1, 0) },
	{ .bank = 11, .words_mask = GENMASK(5, 0) },
	{ .bank = 11, .words_mask = GENMASK(7, 6) },
	{ .bank = 12, .words_mask = GENMASK(2, 0) },
	{ .bank = 12, .words_mask = BIT(7) },
	{ .bank = 13, .words_mask = GENMASK(4, 2) },
	{ .bank = 14, .words_mask = BIT(1) | BIT(4) | BIT(5) },
	{ .bank = 15, .words_mask = GENMASK(7, 5) },
};

static const struct s32g_fuse_map s32g_fuse_map = {
	.map = s32g_map,
	.n_entries = ARRAY_SIZE(s32g_map),
};

static const struct of_device_id ocotp_of_match[] = {
	{ .compatible = "nxp,s32g2-ocotp", .data = &s32g_fuse_map},
	{ /* sentinel */ }
};

static u32 get_bank_index(unsigned int offset)
{
	return (offset - S32G_OCOTP_BANK_OFFSET) / S32G_OCOTP_BANK_SIZE;
}

static u32 get_word_index(unsigned int offset)
{
	return offset % S32G_OCOTP_BANK_SIZE / S32G_OCOTP_WORD_SIZE;
}

static bool is_valid_word(struct s32g_ocotp_priv *s32g_data,
			  unsigned int offset, int bytes)
{
	const struct s32g_fuse_map *fuse = s32g_data->fuse;
	u32 bank, word;
	size_t i;

	if (offset < S32G_OCOTP_BANK_OFFSET)
		return false;

	if (bytes != S32G_OCOTP_WORD_SIZE)
		return false;

	bank = get_bank_index(offset);
	word = get_word_index(offset);
	if (bank >= fuse->n_entries)
		return false;

	for (i = 0; i < fuse->n_entries; i++) {
		if (fuse->map[i].bank == bank &&
		    fuse->map[i].words_mask & BIT(word))
			return true;
	}
	return false;
}

static int s32g_ocotp_read(void *context, unsigned int offset,
			    void *val, size_t bytes)
{
	struct s32g_ocotp_priv *s32g_data = context;

	if (!is_valid_word(s32g_data, offset, bytes))
		return -EINVAL;

	/* Read from Fuse OCOTP Shadow registers */
	*(u32 *)val = ioread32(s32g_data->base + offset);

	return 0;
}

static struct nvmem_config s32g_ocotp_nvmem_config = {
	.name = "s32g-ocotp",
	.add_legacy_fixed_of_cells = true,
	.read_only = true,
	.word_size = S32G_OCOTP_WORD_SIZE,
	.reg_read = s32g_ocotp_read,
};

static int s32g_ocotp_probe(struct platform_device *pdev)
{
	const struct of_device_id *of_matched_dt_id;
	struct s32g_ocotp_priv *s32g_data;
	struct device *dev = &pdev->dev;
	struct nvmem_device *nvmem;
	struct resource *res;

	of_matched_dt_id = of_match_device(ocotp_of_match, dev);
	if (!of_matched_dt_id) {
		dev_err(dev, "Unable to find driver data.\n");
		return -ENODEV;
	}

	s32g_data = devm_kzalloc(dev, sizeof(*s32g_data), GFP_KERNEL);
	if (!s32g_data)
		return -ENOMEM;

	s32g_data->fuse = of_device_get_match_data(dev);
	if (!s32g_data->fuse) {
		dev_err(dev, "Cannot find platform device data.\n");
		return -ENODEV;
	}

	s32g_data->base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(s32g_data->base)) {
		dev_err(dev, "Cannot map OCOTP device.\n");
		return PTR_ERR(s32g_data->base);
	}

	s32g_data->dev = dev;
	s32g_ocotp_nvmem_config.dev = dev;
	s32g_ocotp_nvmem_config.priv = s32g_data;
	s32g_ocotp_nvmem_config.size = resource_size(res);

	nvmem = devm_nvmem_register(dev, &s32g_ocotp_nvmem_config);

	return PTR_ERR_OR_ZERO(nvmem);
}

static struct platform_driver s32g_ocotp_driver = {
	.probe = s32g_ocotp_probe,
	.driver = {
		.name = "s32g-ocotp",
		.of_match_table = ocotp_of_match,
	},
};
module_platform_driver(s32g_ocotp_driver);
MODULE_AUTHOR("NXP");
MODULE_DESCRIPTION("S32G OCOTP driver");
MODULE_LICENSE("GPL");
