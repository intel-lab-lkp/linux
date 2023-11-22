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
#include <linux/bitfield.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>

#include "meson-gx-socinfo-internal.h"

static const char *socinfo_to_package_id(u32 socinfo)
{
	unsigned int pack = socinfo_to_pack(socinfo);
	unsigned int major = socinfo_to_major(socinfo);
	int i;

	for (i = 0 ; i < ARRAY_SIZE(soc_packages) ; ++i) {
		if (soc_packages[i].major_id == major &&
		    soc_packages[i].pack_id ==
				(pack & soc_packages[i].pack_mask))
			return soc_packages[i].name;
	}

	return "Unknown";
}

static const char *socinfo_to_soc_id(u32 socinfo)
{
	unsigned int id = socinfo_to_major(socinfo);
	int i;

	for (i = 0 ; i < ARRAY_SIZE(soc_ids) ; ++i) {
		if (soc_ids[i].id == id)
			return soc_ids[i].name;
	}

	return "Unknown";
}

int meson_gx_socinfo_prepare_soc_driver_attr(struct soc_device_attribute *soc_dev_attr,
					     unsigned int socattr)
{
	soc_dev_attr->family = "Amlogic Meson";
	soc_dev_attr->revision = kasprintf(GFP_KERNEL, "%x:%x - %x:%x",
					   socinfo_to_major(socattr),
					   socinfo_to_minor(socattr),
					   socinfo_to_pack(socattr),
					   socinfo_to_misc(socattr));
	soc_dev_attr->soc_id = kasprintf(GFP_KERNEL, "%s (%s)",
					 socinfo_to_soc_id(socattr),
					 socinfo_to_package_id(socattr));
	return 0;
}
EXPORT_SYMBOL(meson_gx_socinfo_prepare_soc_driver_attr);

static int __init meson_gx_socinfo_init(void)
{
	struct soc_device_attribute *soc_dev_attr;
	struct soc_device *soc_dev;
	struct device_node *np;
	struct regmap *regmap;
	unsigned int socinfo;
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

	ret = regmap_read(regmap, AO_SEC_SOCINFO_OFFSET, &socinfo);
	if (ret < 0)
		return ret;

	if (!socinfo) {
		pr_err("%s: invalid chipid value\n", __func__);
		return -EINVAL;
	}

	soc_dev_attr = kzalloc(sizeof(*soc_dev_attr), GFP_KERNEL);
	if (!soc_dev_attr)
		return -ENODEV;

	meson_gx_socinfo_prepare_soc_driver_attr(soc_dev_attr, socinfo);

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
			socinfo_to_major(socinfo),
			socinfo_to_minor(socinfo),
			socinfo_to_pack(socinfo),
			socinfo_to_misc(socinfo));

	return 0;
}
device_initcall(meson_gx_socinfo_init);
