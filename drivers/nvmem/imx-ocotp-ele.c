// SPDX-License-Identifier: GPL-2.0-only
/*
 * i.MX9 OCOTP fusebox driver
 *
 * Copyright 2023 NXP
 */

#include <linux/device.h>
#include <linux/firmware/imx/se_api.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/nvmem-provider.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/if_ether.h>	/* ETH_ALEN */

enum fuse_type {
	FUSE_FSB = BIT(0),
	FUSE_ELE = BIT(1),
	FUSE_ECC = BIT(2),
	FUSE_INVALID = -1
};

struct ocotp_map_entry {
	u32 start; /* start word */
	u32 num; /* num words */
	enum fuse_type type;
};

struct ocotp_devtype_data {
	u32 reg_off;
	char *name;
	u32 size;
	u32 num_entry;
	u32 flag;
	const struct nvmem_keepout *keepout;
	unsigned int nkeepout;
	struct ocotp_map_entry entry[];
};

struct imx_ocotp_priv {
	struct device *dev;
	void __iomem *base;
	struct nvmem_config config;
	struct mutex lock;
	const struct ocotp_devtype_data *data;
	void *se_data;
	struct platform_device *se_dev;
};

static enum fuse_type imx_ocotp_fuse_type(void *context, u32 index)
{
	struct imx_ocotp_priv *priv = context;
	const struct ocotp_devtype_data *data = priv->data;
	u32 start, end;
	int i;

	for (i = 0; i < data->num_entry; i++) {
		start = data->entry[i].start;
		end = data->entry[i].start + data->entry[i].num;

		if (index >= start && index < end)
			return data->entry[i].type;
	}

	return FUSE_INVALID;
}

static int imx_ocotp_reg_read(void *context, unsigned int offset, void *val, size_t bytes)
{
	struct imx_ocotp_priv *priv = context;
	void __iomem *reg = priv->base + priv->data->reg_off;
	u32 count, index, num_bytes;
	enum fuse_type type;
	u32 *buf;
	void *p;
	int ret;
	int i;
	u8 skipbytes;

	if (offset + bytes > priv->data->size)
		bytes = priv->data->size - offset;

	index = offset >> 2;
	skipbytes = offset - (index << 2);
	num_bytes = round_up(bytes + skipbytes, 4);
	count = num_bytes >> 2;

	p = kzalloc(num_bytes, GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	mutex_lock(&priv->lock);

	buf = p;

	for (i = index; i < (index + count); i++) {
		/*
		 * All fuse registers can be read via ELE. If the SE device is
		 * available, always prefer it.
		 */
		if (priv->se_data) {
			ret = imx_se_read_fuse(priv->se_data, i, buf++);
			if (ret) {
				mutex_unlock(&priv->lock);
				return ret;
			}
			continue;
		}

		type = imx_ocotp_fuse_type(context, i);
		if (type == FUSE_INVALID || type == FUSE_ELE) {
			*buf++ = 0;
			continue;
		}

		if (type & FUSE_ECC)
			*buf++ = readl_relaxed(reg + (i << 2)) & GENMASK(15, 0);
		else
			*buf++ = readl_relaxed(reg + (i << 2));
	}

	memcpy(val, ((u8 *)p) + skipbytes, bytes);

	mutex_unlock(&priv->lock);

	kfree(p);

	return 0;
};

static int imx_ocotp_reg_write(void *context, unsigned int offset, void *val, size_t bytes)
{
	struct imx_ocotp_priv *priv = context;
	u32 word = offset >> 2;
	u32 *buf = val;
	int ret;

	/* allow only writing one complete OTP word at a time */
	if ((bytes != 4) || (offset % 4 != 0))
		return -EINVAL;

	/*
	 * The ELE API returns an error when writing an all-zero value. As
	 * OTP fuse bits can not be switched from 1 to 0 anyway, skip these
	 * values.
	 */
	if (!*buf)
		return 0;

	mutex_lock(&priv->lock);
	ret = imx_se_write_fuse(priv->se_data, word, *buf);
	mutex_unlock(&priv->lock);

	return ret;
}

static int imx_ocotp_cell_pp(void *context, const char *id, int index,
			     unsigned int offset, void *data, size_t bytes)
{
	u8 *buf = data;
	int i;

	/* Deal with some post processing of nvmem cell data */
	if (id && !strcmp(id, "mac-address")) {
		bytes = min(bytes, ETH_ALEN);
		for (i = 0; i < bytes / 2; i++)
			swap(buf[i], buf[bytes - i - 1]);
	}

	return 0;
}

static void imx_ocotp_fixup_dt_cell_info(struct nvmem_device *nvmem,
					 struct nvmem_cell_info *cell)
{
	cell->raw_len = round_up(cell->bytes, 4);
	cell->read_post_process = imx_ocotp_cell_pp;
}

static void imx_ocotp_put_se_dev(void *data)
{
	platform_device_put(data);
}

static int imx_ele_ocotp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct imx_ocotp_priv *priv;
	struct nvmem_device *nvmem;
	struct device_node *np;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->data = of_device_get_match_data(dev);

	priv->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->base))
		return PTR_ERR(priv->base);

	np = of_parse_phandle(pdev->dev.of_node, "secure-enclave", 0);
	if (!np) {
		dev_info(&pdev->dev, "missing or invalid SE handle, using readonly FSB\n");
	} else {
		priv->se_dev = of_find_device_by_node(np);
		of_node_put(np);
		if (!priv->se_dev)
			return dev_err_probe(&pdev->dev, -ENODEV, "failed to find SE device\n");

		ret = devm_add_action_or_reset(&pdev->dev, imx_ocotp_put_se_dev,
					       priv->se_dev);
		if (ret)
			return ret;

		priv->se_data = platform_get_drvdata(priv->se_dev);
		if (!priv->se_data)
			return dev_err_probe(&pdev->dev, -EPROBE_DEFER,
					     "SE device not ready\n");
	}

	priv->config.dev = dev;
	priv->config.name = "ELE-OCOTP";
	priv->config.id = NVMEM_DEVID_AUTO;
	priv->config.owner = THIS_MODULE;
	priv->config.size = priv->data->size;
	priv->config.reg_read = imx_ocotp_reg_read;
	priv->config.reg_write = imx_ocotp_reg_write;
	priv->config.word_size = 1;
	priv->config.stride = 1;
	priv->config.priv = priv;
	priv->config.add_legacy_fixed_of_cells = true;
	priv->config.fixup_dt_cell_info = imx_ocotp_fixup_dt_cell_info;

	if (priv->data->nkeepout) {
		priv->config.keepout = priv->data->keepout;
		priv->config.nkeepout = priv->data->nkeepout;
	}

	if (!priv->se_data)
		priv->config.read_only = true;

	mutex_init(&priv->lock);

	nvmem = devm_nvmem_register(dev, &priv->config);
	if (IS_ERR(nvmem))
		return PTR_ERR(nvmem);

	return 0;
}

static const struct nvmem_keepout imx93_ocotp_keepout[] = {
	{.start = 208, .end = 252},
	{.start = 256, .end = 512},
	{.start = 576, .end = 728},
	{.start = 732, .end = 752},
	{.start = 756, .end = 1248},
};

static const struct ocotp_devtype_data imx93_ocotp_data = {
	.reg_off = 0x8000,
	.size = 2048,
	.num_entry = 6,
	.entry = {
		{ 0, 52, FUSE_FSB },
		{ 63, 1, FUSE_ELE},
		{ 128, 16, FUSE_ELE },
		{ 182, 1, FUSE_ELE },
		{ 188, 1, FUSE_ELE },
		{ 312, 200, FUSE_FSB }
	},
	.keepout = imx93_ocotp_keepout,
	.nkeepout = ARRAY_SIZE(imx93_ocotp_keepout),
};

static const struct ocotp_devtype_data imx94_ocotp_data = {
	.reg_off = 0x8000,
	.size = 3296, /* 103 Banks */
	.num_entry = 10,
	.entry = {
		{ 0, 1, FUSE_FSB | FUSE_ECC },
		{ 7, 1, FUSE_FSB | FUSE_ECC },
		{ 9, 3, FUSE_FSB | FUSE_ECC },
		{ 12, 24, FUSE_FSB },
		{ 36, 2, FUSE_FSB  | FUSE_ECC },
		{ 38, 14, FUSE_FSB },
		{ 59, 1, FUSE_ELE },
		{ 525, 2, FUSE_FSB | FUSE_ECC },
		{ 528, 7, FUSE_FSB },
		{ 536, 280, FUSE_FSB },
	},
};

static const struct ocotp_devtype_data imx95_ocotp_data = {
	.reg_off = 0x8000,
	.size = 2048,
	.num_entry = 12,
	.entry = {
		{ 0, 1, FUSE_FSB | FUSE_ECC },
		{ 7, 1, FUSE_FSB | FUSE_ECC },
		{ 9, 3, FUSE_FSB | FUSE_ECC },
		{ 12, 24, FUSE_FSB },
		{ 36, 2, FUSE_FSB  | FUSE_ECC },
		{ 38, 14, FUSE_FSB },
		{ 63, 1, FUSE_ELE },
		{ 128, 16, FUSE_ELE },
		{ 188, 1, FUSE_ELE },
		{ 317, 2, FUSE_FSB | FUSE_ECC },
		{ 320, 7, FUSE_FSB },
		{ 328, 184, FUSE_FSB }
	},
};

static const struct of_device_id imx_ele_ocotp_dt_ids[] = {
	{ .compatible = "fsl,imx93-ocotp", .data = &imx93_ocotp_data, },
	{ .compatible = "fsl,imx94-ocotp", .data = &imx94_ocotp_data, },
	{ .compatible = "fsl,imx95-ocotp", .data = &imx95_ocotp_data, },
	{},
};
MODULE_DEVICE_TABLE(of, imx_ele_ocotp_dt_ids);

static struct platform_driver imx_ele_ocotp_driver = {
	.driver = {
		.name = "imx_ele_ocotp",
		.of_match_table = imx_ele_ocotp_dt_ids,
	},
	.probe = imx_ele_ocotp_probe,
};
module_platform_driver(imx_ele_ocotp_driver);

MODULE_DESCRIPTION("i.MX OCOTP/ELE driver");
MODULE_AUTHOR("Peng Fan <peng.fan@nxp.com>");
MODULE_LICENSE("GPL");
