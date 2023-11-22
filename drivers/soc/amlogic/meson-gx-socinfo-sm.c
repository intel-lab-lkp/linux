// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2023 JetHome
 * Author: Viacheslav Bocharov <adeep@lexina.in>
 *
 */

#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/sys_soc.h>
#include <linux/bitfield.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>

#include <linux/firmware/meson/meson_sm.h>

#include "meson-gx-socinfo-internal.h"

static char *socinfo_get_cpuid(struct device *dev, struct meson_sm_firmware *fw,
			       unsigned int *socinfo)
{
	char *buf;
	uint8_t *id_buf;
	int chip_id_version;
	int ret;

	id_buf = devm_kzalloc(dev, SM_CHIP_ID_LENGTH, GFP_KERNEL);
	if (!id_buf)
		return NULL;

	ret = meson_sm_call_read(fw, id_buf, SM_CHIP_ID_LENGTH, SM_GET_CHIP_ID,
				 2, 0, 0, 0, 0);
	if (ret < 0) {
		kfree(id_buf);
		return NULL;
	}

	chip_id_version = *((unsigned int *)id_buf);

	if (chip_id_version != 2) {
		uint8_t tmp;
		/**
		 * Legacy 12-byte chip ID read out, transform data
		 * to expected order format
		 */

		memmove(&id_buf[SM_CHIP_ID_OFFSET + 4], &id_buf[SM_CHIP_ID_OFFSET], 12);
		for (int i = 0; i < 6; i++) {
			tmp = id_buf[i + SM_CHIP_ID_OFFSET + 4];
			id_buf[i + SM_CHIP_ID_OFFSET + 4] = id_buf[15 - i + SM_CHIP_ID_OFFSET];
			id_buf[15 - i + SM_CHIP_ID_OFFSET] = tmp;
		}
		*(uint32_t *)(id_buf + SM_CHIP_ID_OFFSET) =
					((*socinfo & 0xff000000)	|	// Family ID
					((*socinfo << 8) & 0xff0000)	|	// Chip Revision
					((*socinfo >> 8) & 0xff00))	|	// Package ID
					((*socinfo) & 0xff);			// Misc
	} else {
		*socinfo = id_buf[SM_CHIP_ID_OFFSET] << 24 |	// Family ID
		   id_buf[SM_CHIP_ID_OFFSET + 2] << 16 |	// Chip revision
		   id_buf[SM_CHIP_ID_OFFSET + 1] << 8 |		// Package ID
		   id_buf[SM_CHIP_ID_OFFSET + 3];		// Misc
	}

	buf = kasprintf(GFP_KERNEL, "%16phN\n", &id_buf[SM_CHIP_ID_OFFSET]);
	kfree(id_buf);

	return buf;
}

static int meson_gx_socinfo_sm_probe(struct platform_device *pdev)
{
	struct soc_device_attribute *soc_dev_attr;
	struct soc_device *soc_dev;
	struct device_node *sm_np;
	struct meson_sm_firmware *fw;
	struct regmap *regmap;
	unsigned int socinfo;
	struct device *dev;
	int ret;

	/* check if chip-id is available */
	if (!of_property_read_bool(pdev->dev.of_node, "amlogic,has-chip-id"))
		return -ENODEV;

	/* node should be a syscon */
	regmap = syscon_node_to_regmap(pdev->dev.of_node);
	if (IS_ERR(regmap)) {
		dev_err(&pdev->dev, "failed to get regmap\n");
		return -ENODEV;
	}

	sm_np = of_parse_phandle(pdev->dev.of_node, "secure-monitor", 0);
	if (!sm_np) {
		dev_err(&pdev->dev, "no secure-monitor node found\n");
		return -ENODEV;
	}

	fw = meson_sm_get(sm_np);
	of_node_put(sm_np);
	if (!fw)
		return -EPROBE_DEFER;

	dev_err(&pdev->dev, "secure-monitor node found\n");

	ret = regmap_read(regmap, AO_SEC_SOCINFO_OFFSET, &socinfo);
	if (ret < 0)
		return ret;

	if (!socinfo) {
		dev_err(&pdev->dev, "invalid regmap chipid value\n");
		return -EINVAL;
	}

	soc_dev_attr = devm_kzalloc(&pdev->dev, sizeof(*soc_dev_attr),
				    GFP_KERNEL);
	if (!soc_dev_attr)
		return -ENOMEM;

	soc_dev_attr->serial_number = socinfo_get_cpuid(&pdev->dev, fw, &socinfo);

	meson_gx_socinfo_prepare_soc_driver_attr(soc_dev_attr, socinfo);

	soc_dev = soc_device_register(soc_dev_attr);
	if (IS_ERR(soc_dev)) {
		kfree(soc_dev_attr->revision);
		kfree_const(soc_dev_attr->soc_id);
		kfree(soc_dev_attr);
		return PTR_ERR(soc_dev);
	}

	dev = soc_device_to_device(soc_dev);
	platform_set_drvdata(pdev, soc_dev);

	dev_info(dev, "Amlogic Meson %s Revision %x:%x (%x:%x) Detected at SM driver %x\n",
			soc_dev_attr->soc_id,
			socinfo_to_major(socinfo),
			socinfo_to_minor(socinfo),
			socinfo_to_pack(socinfo),
			socinfo_to_misc(socinfo), socinfo);

	return PTR_ERR_OR_ZERO(dev);
}


static int meson_gx_socinfo_sm_remove(struct platform_device *pdev)
{
	struct soc_device *soc_dev = platform_get_drvdata(pdev);

	soc_device_unregister(soc_dev);
	return 0;
}

static const struct of_device_id meson_gx_socinfo_match[] = {
	{ .compatible = "amlogic,meson-gx-ao-secure", },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, meson_gx_socinfo_match);

static struct platform_driver meson_gx_socinfo_driver = {
	.probe = meson_gx_socinfo_sm_probe,
	.remove	= meson_gx_socinfo_sm_remove,
	.driver = {
		.name = "meson-gx-socinfo-sm",
		.of_match_table = meson_gx_socinfo_match,
	},
};


module_platform_driver(meson_gx_socinfo_driver);

MODULE_AUTHOR("Viacheslav Bocharov <adeep@lexina.in>");
MODULE_DESCRIPTION("Amlogic Meson GX SOC SM driver");
MODULE_LICENSE("GPL");
