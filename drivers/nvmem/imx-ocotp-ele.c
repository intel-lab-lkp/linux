// SPDX-License-Identifier: GPL-2.0-only
/*
 * i.MX9 OCOTP fusebox driver
 *
 * Copyright 2023 NXP
 */

#include <dt-bindings/nvmem/fsl,imx93-ocotp.h>
#include <dt-bindings/nvmem/fsl,imx95-ocotp.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/nvmem-provider.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

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
	const struct ocotp_access_gates *access_gates;
	u32 reg_off;
	char *name;
	u32 size;
	u32 num_entry;
	u32 flag;
	nvmem_reg_read_t reg_read;
	struct ocotp_map_entry entry[];
};

#define OCOTP_MAX_NUM_GATE_WORDS 4
#define IMX93_OCOTP_NUM_GATES 17
#define IMX95_OCOTP_NUM_GATES 36

struct ocotp_access_gates {
	u32 num_words;
	u32 words[OCOTP_MAX_NUM_GATE_WORDS];
	u32 num_gates;
	struct access_gate {
		u32 word;
		u32 mask;
	} gates[];
};

struct imx_ocotp_priv {
	struct device *dev;
	void __iomem *base;
	struct nvmem_config config;
	struct mutex lock;
	u32 value[OCOTP_MAX_NUM_GATE_WORDS];
	const struct ocotp_devtype_data *data;
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

static int imx_ocotp_cell_pp(void *context, const char *id, int index,
			     unsigned int offset, void *data, size_t bytes)
{
	u8 *buf = data;
	int i;

	/* Deal with some post processing of nvmem cell data */
	if (id && !strcmp(id, "mac-address"))
		for (i = 0; i < bytes / 2; i++)
			swap(buf[i], buf[bytes - i - 1]);

	return 0;
}

static void imx_ocotp_fixup_dt_cell_info(struct nvmem_device *nvmem,
					 struct nvmem_cell_info *cell)
{
	cell->read_post_process = imx_ocotp_cell_pp;
}

static int imx_ele_ocotp_check_access(struct platform_device *pdev, u32 id)
{
	struct imx_ocotp_priv *priv = platform_get_drvdata(pdev);
	const struct ocotp_access_gates *access_gates = priv->data->access_gates;
	u32 word, mask;

	if (id >= access_gates->num_gates) {
		dev_err(&pdev->dev, "Index %d too large\n", id);
		return -EACCES;
	}

	word = access_gates->gates[id].word;
	mask = access_gates->gates[id].mask;

	dev_dbg(&pdev->dev, "id:%d word:%d mask:0x%08x\n", id, word, mask);
	/* true means not allow access */
	if (priv->value[word] & mask)
		return -EACCES;

	return 0;
}

static int imx_ele_ocotp_grant_access(struct platform_device *pdev, struct device_node *parent)
{
	struct device_node *child;
	struct device *dev = &pdev->dev;

	for_each_available_child_of_node(parent, child) {
		struct of_phandle_iterator it;
		int err;
		u32 id;

		of_for_each_phandle(&it, err, child, "access-controllers",
				    "#access-controller-cells", 0) {
			struct of_phandle_args provider_args;
			struct device_node *provider = it.node;

			if (err) {
				dev_err(dev, "Unable to get access-controllers property for node %s\n, err: %d",
					child->full_name, err);
				of_node_put(provider);
				return err;
			}

			/* Only support one cell */
			if (of_phandle_iterator_args(&it, provider_args.args, 1) != 1) {
				dev_err(dev, "wrong args count\n");
				return -EINVAL;
			}

			id = provider_args.args[0];

			dev_dbg(dev, "Checking node: %s gate: %d\n", child->full_name, id);

			if (imx_ele_ocotp_check_access(pdev, id)) {
				of_detach_node(child);
				dev_err(dev, "%s: Not granted, device driver will not be probed\n",
					child->full_name);
			}
		}

		imx_ele_ocotp_grant_access(pdev, child);
	}

	return 0;
}

static int imx_ele_ocotp_access_control(struct platform_device *pdev)
{
	struct imx_ocotp_priv *priv = platform_get_drvdata(pdev);
	struct device_node *soc __free(device_node) = of_find_node_by_path("/soc");
	const struct ocotp_access_gates *access_gates = priv->data->access_gates;
	void __iomem *reg = priv->base + priv->data->reg_off;
	u32 off;
	int i;

	if (!priv->data->access_gates)
		return 0;

	if (!soc)
		soc = of_find_node_by_path("/soc@0");

	/* This should never happen */
	WARN_ON(!soc);

	for (i = 0; i < access_gates->num_words; i++) {
		off = access_gates->words[i] << 2;
		priv->value[i] = readl(reg + off);
		dev_dbg(&pdev->dev, "word:%d 0x%08x\n", access_gates->words[i], priv->value[i]);
	}

	return imx_ele_ocotp_grant_access(pdev, soc);
}

static int imx_ele_ocotp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct imx_ocotp_priv *priv;
	struct nvmem_device *nvmem;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->data = of_device_get_match_data(dev);

	priv->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->base))
		return PTR_ERR(priv->base);

	priv->config.dev = dev;
	priv->config.name = "ELE-OCOTP";
	priv->config.id = NVMEM_DEVID_AUTO;
	priv->config.owner = THIS_MODULE;
	priv->config.size = priv->data->size;
	priv->config.reg_read = priv->data->reg_read;
	priv->config.word_size = 1;
	priv->config.stride = 1;
	priv->config.priv = priv;
	priv->config.read_only = true;
	priv->config.add_legacy_fixed_of_cells = true;
	priv->config.fixup_dt_cell_info = imx_ocotp_fixup_dt_cell_info;
	mutex_init(&priv->lock);

	platform_set_drvdata(pdev, priv);

	nvmem = devm_nvmem_register(dev, &priv->config);
	if (IS_ERR(nvmem))
		return PTR_ERR(nvmem);


	return imx_ele_ocotp_access_control(pdev);
}

static const struct ocotp_access_gates imx93_access_gates = {
	.num_words = 3,
	.words = {19, 20, 21},
	.num_gates = IMX93_OCOTP_NUM_GATES,
	.gates = {
		[IMX93_OCOTP_NPU_GATE]		= { .word = 19, .mask = BIT(13) },
		[IMX93_OCOTP_A550_GATE]		= { .word = 19, .mask = BIT(14) },
		[IMX93_OCOTP_A551_GATE]		= { .word = 19, .mask = BIT(15) },
		[IMX93_OCOTP_M33_GATE]		= { .word = 19, .mask = BIT(24) },
		[IMX93_OCOTP_CAN1_FD_GATE]	= { .word = 19, .mask = BIT(28) },
		[IMX93_OCOTP_CAN2_FD_GATE]	= { .word = 19, .mask = BIT(29) },
		[IMX93_OCOTP_CAN1_GATE]		= { .word = 19, .mask = BIT(30) },
		[IMX93_OCOTP_CAN2_GATE]		= { .word = 19, .mask = BIT(31) },
		[IMX93_OCOTP_USB1_GATE]		= { .word = 20, .mask = BIT(3) },
		[IMX93_OCOTP_USB2_GATE]		= { .word = 20, .mask = BIT(4) },
		[IMX93_OCOTP_ENET1_GATE]	= { .word = 20, .mask = BIT(5) },
		[IMX93_OCOTP_ENET2_GATE]	= { .word = 20, .mask = BIT(6) },
		[IMX93_OCOTP_PXP_GATE]		= { .word = 20, .mask = BIT(10) },
		[IMX93_OCOTP_MIPI_CSI1_GATE]	= { .word = 20, .mask = BIT(17) },
		[IMX93_OCOTP_MIPI_DSI1_GATE]	= { .word = 20, .mask = BIT(19) },
		[IMX93_OCOTP_LVDS1_GATE]	= { .word = 20, .mask = BIT(24) },
		[IMX93_OCOTP_ADC1_GATE]		= { .word = 21, .mask = BIT(7) },
	},
};

static const struct ocotp_devtype_data imx93_ocotp_data = {
	.access_gates = &imx93_access_gates,
	.reg_off = 0x8000,
	.reg_read = imx_ocotp_reg_read,
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
};

static const struct ocotp_access_gates imx95_access_gates = {
	.num_words = 3,
	.words = {17, 18, 19},
	.num_gates = IMX95_OCOTP_NUM_GATES,
	.gates = {
		[IMX95_OCOTP_CANFD1_GATE]	= { .word = 17, .mask = BIT(20) },
		[IMX95_OCOTP_CANFD2_GATE]	= { .word = 17, .mask = BIT(21) },
		[IMX95_OCOTP_CANFD3_GATE]	= { .word = 17, .mask = BIT(22) },
		[IMX95_OCOTP_CANFD4_GATE]	= { .word = 17, .mask = BIT(23) },
		[IMX95_OCOTP_CANFD5_GATE]	= { .word = 17, .mask = BIT(24) },
		[IMX95_OCOTP_CAN1_GATE]	= { .word = 17, .mask = BIT(25) },
		[IMX95_OCOTP_CAN2_GATE]	= { .word = 17, .mask = BIT(26) },
		[IMX95_OCOTP_CAN3_GATE]	= { .word = 17, .mask = BIT(27) },
		[IMX95_OCOTP_CAN4_GATE]	= { .word = 17, .mask = BIT(28) },
		[IMX95_OCOTP_CAN5_GATE]	= { .word = 17, .mask = BIT(29) },
		[IMX95_OCOTP_NPU_GATE]		= { .word = 18, .mask = BIT(0) },
		[IMX95_OCOTP_A550_GATE]	= { .word = 18, .mask = BIT(1) },
		[IMX95_OCOTP_A551_GATE]	= { .word = 18, .mask = BIT(2) },
		[IMX95_OCOTP_A552_GATE]	= { .word = 18, .mask = BIT(3) },
		[IMX95_OCOTP_A553_GATE]	= { .word = 18, .mask = BIT(4) },
		[IMX95_OCOTP_A554_GATE]	= { .word = 18, .mask = BIT(5) },
		[IMX95_OCOTP_A555_GATE]	= { .word = 18, .mask = BIT(6) },
		[IMX95_OCOTP_M7_GATE]		= { .word = 18, .mask = BIT(9) },
		[IMX95_OCOTP_DCSS_GATE]	= { .word = 18, .mask = BIT(22) },
		[IMX95_OCOTP_LVDS1_GATE]	= { .word = 18, .mask = BIT(27) },
		[IMX95_OCOTP_ISP_GATE]		= { .word = 18, .mask = BIT(29) },
		[IMX95_OCOTP_USB1_GATE]	= { .word = 19, .mask = BIT(2) },
		[IMX95_OCOTP_USB2_GATE]	= { .word = 19, .mask = BIT(3) },
		[IMX95_OCOTP_NETC_GATE]	= { .word = 19, .mask = BIT(4) },
		[IMX95_OCOTP_PCIE1_GATE]	= { .word = 19, .mask = BIT(6) },
		[IMX95_OCOTP_PCIE2_GATE]	= { .word = 19, .mask = BIT(7) },
		[IMX95_OCOTP_ADC1_GATE]	= { .word = 19, .mask = BIT(8) },
		[IMX95_OCOTP_EARC_RX_GATE]	= { .word = 19, .mask = BIT(11) },
		[IMX95_OCOTP_GPU3D_GATE]	= { .word = 19, .mask = BIT(16) },
		[IMX95_OCOTP_VPU_GATE]		= { .word = 19, .mask = BIT(17) },
		[IMX95_OCOTP_JPEG_ENC_GATE]	= { .word = 19, .mask = BIT(18) },
		[IMX95_OCOTP_JPEG_DEC_GATE]	= { .word = 19, .mask = BIT(19) },
		[IMX95_OCOTP_MIPI_CSI1_GATE]	= { .word = 19, .mask = BIT(21) },
		[IMX95_OCOTP_MIPI_CSI2_GATE]	= { .word = 19, .mask = BIT(22) },
		[IMX95_OCOTP_MIPI_DSI1_GATE]	= { .word = 19, .mask = BIT(23) },
	}
};

static const struct ocotp_devtype_data imx95_ocotp_data = {
	.access_gates = &imx95_access_gates,
	.reg_off = 0x8000,
	.reg_read = imx_ocotp_reg_read,
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
