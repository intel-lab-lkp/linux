// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023 MediaTek Inc.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/pm_runtime.h>
#include <linux/nvmem-consumer.h>
#include <linux/device.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/sys_soc.h>
#include <linux/slab.h>

#define MTK_SOCINFO_ENTRY(_soc_name, _segment_name, _marketing_name, _cell_data1, _cell_data2) {\
	.soc_name = _soc_name,	\
	.segment_name = _segment_name,	\
	.marketing_name = _marketing_name,	\
	.cell_data = {_cell_data1, _cell_data2}	\
}
#define CELL_NOT_USED (0xFFFFFFFF)
#define MAX_CELLS (2)

struct mtk_socinfo {
	struct device *dev;
	struct name_data *name_data;
	struct socinfo_data *socinfo_data;
};

struct socinfo_data {
	char *soc_name;
	char *segment_name;
	char *marketing_name;
	u32 cell_data[MAX_CELLS];
};

const char *soc_manufacturer = "MediaTek";
struct soc_device *soc_dev;
char *cell_names[MAX_CELLS] = {"socinfo-data1", "socinfo-data2"};

static struct socinfo_data socinfo_data_table[] = {
	MTK_SOCINFO_ENTRY("MT8173", "MT8173V/AC", "MT8173", 0x6CA20004, 0x10000000),
	MTK_SOCINFO_ENTRY("MT8183", "MT8183V/AZA", "Kompanio 500", 0x00010043, 0x00000840),
	MTK_SOCINFO_ENTRY("MT8186", "MT8186GV/AZA", "Kompanio 520", 0x81861001, CELL_NOT_USED),
	MTK_SOCINFO_ENTRY("MT8186T", "MT8186TV/AZA", "Kompanio 528", 0x81862001, CELL_NOT_USED),
	MTK_SOCINFO_ENTRY("MT8188", "MT8188GV/AZA", "Kompanio 830", 0x81880000, 0x00000010),
	MTK_SOCINFO_ENTRY("MT8188", "MT8188GV/HZA", "Kompanio 830", 0x81880000, 0x00000011),
	MTK_SOCINFO_ENTRY("MT8192", "MT8192V/AZA", "Kompanio 820", 0x00001100, 0x00040080),
	MTK_SOCINFO_ENTRY("MT8192T", "MT8192V/ATZA", "Kompanio 828", 0x00000100, 0x000400C0),
	MTK_SOCINFO_ENTRY("MT8195", "MT8195GV/EZA", "Kompanio 1200", 0x81950300, CELL_NOT_USED),
	MTK_SOCINFO_ENTRY("MT8195", "MT8195GV/EHZA", "Kompanio 1200", 0x81950304, CELL_NOT_USED),
	MTK_SOCINFO_ENTRY("MT8195", "MT8195TV/EZA", "Kompanio 1380", 0x81950400, CELL_NOT_USED),
	MTK_SOCINFO_ENTRY("MT8195", "MT8195TV/EHZA", "Kompanio 1380", 0x81950404, CELL_NOT_USED),
};

static int mtk_socinfo_create_socinfo_node(struct mtk_socinfo *mtk_socinfop)
{
	struct soc_device_attribute *attrs;
	static char machine[30] = {0};

	attrs = devm_kzalloc(mtk_socinfop->dev, sizeof(*attrs), GFP_KERNEL);
	if (!attrs)
		return -ENOMEM;

	snprintf(machine, 30, "%s (%s)", mtk_socinfop->socinfo_data->marketing_name,
		mtk_socinfop->socinfo_data->soc_name);
	attrs->family = soc_manufacturer;
	attrs->machine = machine;

	soc_dev = soc_device_register(attrs);
	if (IS_ERR(soc_dev))
		return PTR_ERR(soc_dev);

	dev_info(mtk_socinfop->dev, "%s SoC detected.\n", attrs->machine);
	return 0;
}

static int mtk_socinfo_get_socinfo_data(struct mtk_socinfo *mtk_socinfop)
{
	unsigned int i = 0, j = 0;
	unsigned int num_cell_data = 0;
	u32 *cell_datap[MAX_CELLS] = { NULL };
	size_t efuse_bytes;
	struct nvmem_cell *cell;
	bool match_socinfo = true;
	int match_socinfo_index = -1;

	for (i = 0; i < MAX_CELLS; i++) {
		cell = nvmem_cell_get(mtk_socinfop->dev, cell_names[i]);
		if (IS_ERR_OR_NULL(cell))
			break;
		cell_datap[i] = (u32 *)nvmem_cell_read(cell, &efuse_bytes);
		nvmem_cell_put(cell);
		num_cell_data++;
	}

	if (!num_cell_data)
		return -ENOENT;

	for (i = 0; i < ARRAY_SIZE(socinfo_data_table); i++) {
		match_socinfo = true;
		for (j = 0; j < num_cell_data; j++) {
			if (*(cell_datap[j]) != socinfo_data_table[i].cell_data[j])
				match_socinfo = false;
		}
		if (num_cell_data > 0 && match_socinfo) {
			mtk_socinfop->socinfo_data = &(socinfo_data_table[i]);
			match_socinfo_index = i;
			break;
		}
	}

	return match_socinfo_index >= 0 ? match_socinfo_index : -ENOENT;
}

static const struct of_device_id mtk_socinfo_id_table[] = {
	{ .compatible = "mediatek,socinfo" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, mtk_socinfo_id_table);

static int mtk_socinfo_probe(struct platform_device *pdev)
{
	struct mtk_socinfo *mtk_socinfop;
	int ret = 0;

	mtk_socinfop = devm_kzalloc(&pdev->dev, sizeof(*mtk_socinfop), GFP_KERNEL);
	if (!mtk_socinfop)
		return -ENOMEM;

	mtk_socinfop->dev = &pdev->dev;

	ret = mtk_socinfo_get_socinfo_data(mtk_socinfop);
	if (ret < 0)
		return dev_err_probe(mtk_socinfop->dev, ret, "Failed to get socinfo data\n");

	ret = mtk_socinfo_create_socinfo_node(mtk_socinfop);
	if (ret != 0)
		return dev_err_probe(mtk_socinfop->dev, -EINVAL, "Failed to create socinfo node\n");

	return 0;
}

static int mtk_socinfo_remove(struct platform_device *pdev)
{
	if (soc_dev)
		soc_device_unregister(soc_dev);
	return 0;
}

static struct platform_driver mtk_socinfo = {
	.probe          = mtk_socinfo_probe,
	.remove         = mtk_socinfo_remove,
	.driver         = {
		.name   = "mtk_socinfo",
		.owner  = THIS_MODULE,
		.of_match_table = mtk_socinfo_id_table,
	},
};
module_platform_driver(mtk_socinfo);
MODULE_AUTHOR("William-TW LIN <william-tw.lin@mediatek.com>");
MODULE_DESCRIPTION("Mediatek socinfo driver");
MODULE_LICENSE("GPL");
