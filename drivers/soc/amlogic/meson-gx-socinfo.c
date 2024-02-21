/*
 * Copyright (c) 2017 BayLibre, SAS
 * Author: Neil Armstrong <narmstrong@baylibre.com>
 *
 * SPDX-License-Identifier: GPL-2.0+
 */

#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/sys_soc.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>

#include "meson-gx-socinfo-internal.h"

static int __init meson_gx_socinfo_init(void)
{
	struct soc_device_attribute *soc_dev_attr;
	struct soc_device *soc_dev;
	struct device_node *np;
	struct regmap *regmap;
	union meson_cpu_id socinfo;
	struct device *dev;
	int ret;

	/* look up for chipid node */
	np = of_find_compatible_node(NULL, NULL, "amlogic,meson-gx-ao-secure");
	if (!np)
		return -ENODEV;

	/* check if interface is enabled */
	if (!of_device_is_available(np)) {
		of_node_put(np);
		return -ENODEV;
	}

	/* check if chip-id is available */
	if (!of_property_read_bool(np, "amlogic,has-chip-id")) {
		of_node_put(np);
		return -ENODEV;
	}

	/* node should be a syscon */
	regmap = syscon_node_to_regmap(np);
	of_node_put(np);
	if (IS_ERR(regmap)) {
		pr_err("%s: failed to get regmap\n", __func__);
		return -ENODEV;
	}

	ret = regmap_read(regmap, AO_SEC_SOCINFO_OFFSET, &socinfo.raw);
	if (ret < 0)
		return ret;

	if (!socinfo.raw) {
		pr_err("%s: invalid chipid value\n", __func__);
		return -EINVAL;
	}

	soc_dev_attr = kzalloc(sizeof(*soc_dev_attr), GFP_KERNEL);
	if (!soc_dev_attr)
		return -ENODEV;

	soc_dev_attr->family = "Amlogic Meson";
	soc_dev_attr->revision = kasprintf(GFP_KERNEL, "%x:%x - %x:%x",
					   socinfo.v1.major_id,
					   socinfo.v1.chip_rev,
					   socinfo.v1.pack_id,
					   (socinfo.v1.reserved<<4) + socinfo.v1.layout_ver);
	soc_dev_attr->soc_id = kasprintf(GFP_KERNEL, "%s (%s)",
					 socinfo_v1_to_soc_id(socinfo),
					 socinfo_v1_to_package_id(socinfo));

	soc_dev = soc_device_register(soc_dev_attr);
	if (IS_ERR(soc_dev)) {
		kfree(soc_dev_attr->revision);
		kfree_const(soc_dev_attr->soc_id);
		kfree(soc_dev_attr);
		return PTR_ERR(soc_dev);
	}
	dev = soc_device_to_device(soc_dev);

	dev_info(dev, "Amlogic Meson %s Revision %x:%x (%x:%x) Detected\n",
			soc_dev_attr->soc_id,
			socinfo.v1.major_id,
			socinfo.v1.chip_rev,
			socinfo.v1.pack_id,
			(socinfo.v1.reserved<<4) + socinfo.v1.layout_ver);

	return 0;
}
device_initcall(meson_gx_socinfo_init);
