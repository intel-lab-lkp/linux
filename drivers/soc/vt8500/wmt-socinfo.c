// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright 2025 Alexey Charkov <alchark@gmail.com>
 * Based on aspeed-socinfo.c
 */

#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/sys_soc.h>

static struct {
	const char *name;
	const unsigned long id;
} const chip_id_table[] = {
	/* VIA */
	{ "VT8420", 0x3300 },
	{ "VT8430", 0x3357 },
	{ "VT8500", 0x3400 },

	/* WonderMedia */
	{ "WM8425", 0x3429 },
	{ "WM8435", 0x3437 },
	{ "WM8440", 0x3451 },
	{ "WM8505", 0x3426 },
	{ "WM8650", 0x3465 },
	{ "WM8750", 0x3445 },
	{ "WM8850", 0x3481 },
	{ "WM8880", 0x3498 },
};

static const char *sccid_to_name(unsigned long sccid)
{
	unsigned long id = sccid >> 16;
	unsigned int i;

	for (i = 0 ; i < ARRAY_SIZE(chip_id_table) ; ++i) {
		if (chip_id_table[i].id == id)
			return chip_id_table[i].name;
	}

	return "Unknown";
}

static const char *sccid_to_rev(unsigned long sccid)
{
	char letter, digit;

	letter = (sccid >> 8) & 0xf;
	letter = (letter - 1) + 'A';

	digit = sccid & 0xff;
	digit = (digit - 1) + '0';

	return kasprintf(GFP_KERNEL, "%c%c", letter, digit);
}

static int __init wmt_socinfo_init(void)
{
	struct soc_device_attribute *attrs;
	struct soc_device *soc_dev;
	struct device_node *np;
	void __iomem *reg;
	unsigned long sccid;
	const char *machine = NULL;

	np = of_find_compatible_node(NULL, NULL, "via,scc-id");
	if (!of_device_is_available(np)) {
		of_node_put(np);
		return -ENODEV;
	}

	reg = of_iomap(np, 0);
	if (!reg) {
		of_node_put(np);
		return -ENODEV;
	}
	sccid = readl(reg);
	iounmap(reg);

	attrs = kzalloc(sizeof(*attrs), GFP_KERNEL);
	if (!attrs)
		return -ENODEV;

	/*
	 * Machine: VIA APC Rock
	 * Family: WM8850
	 * Revision: A2
	 * SoC ID: raw silicon revision id (0x34810103)
	 */

	np = of_find_node_by_path("/");
	of_property_read_string(np, "model", &machine);
	if (machine)
		attrs->machine = kstrdup(machine, GFP_KERNEL);
	of_node_put(np);

	attrs->family = sccid_to_name(sccid);
	attrs->revision = sccid_to_rev(sccid);
	attrs->soc_id = kasprintf(GFP_KERNEL, "%08lx", sccid);

	soc_dev = soc_device_register(attrs);
	if (IS_ERR(soc_dev)) {
		kfree(attrs->machine);
		kfree(attrs->soc_id);
		kfree(attrs->revision);
		kfree(attrs);
		return PTR_ERR(soc_dev);
	}

	pr_info("VIA/WonderMedia %s rev %s (%s)\n",
			attrs->family,
			attrs->revision,
			attrs->soc_id);

	return 0;
}
early_initcall(wmt_socinfo_init);
