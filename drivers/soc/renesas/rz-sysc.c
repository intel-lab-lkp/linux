// SPDX-License-Identifier: GPL-2.0
/*
 * RZ System controller driver
 *
 * Copyright (C) 2024 Renesas Electronics Corp.
 */

#include <linux/auxiliary_bus.h>
#include <linux/bitfield.h>
#include <linux/cleanup.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/sys_soc.h>

#include "rz-sysc.h"

/**
 * struct rz_sysc - RZ SYSC private data structure
 * @base: SYSC base address
 * @dev: SYSC device pointer
 */
struct rz_sysc {
	void __iomem *base;
	struct device *dev;
};

static const struct rz_aof_unit *
rz_aof_lookup(const struct rz_sysc_soc_id_init_data *soc_data, u32 base)
{
	unsigned int i;

	for (i = 0; i < soc_data->num_aof_units; i++)
		if (soc_data->aof_units[i].base == base)
			return &soc_data->aof_units[i];

	return NULL;
}

static int rz_aof_parse_bus(struct rz_sysc *sysc,
			    const struct rz_sysc_soc_id_init_data *soc_data,
			    struct device_node *bus_np)
{
	const struct rz_aof_unit *unit;
	struct device_node *child;
	struct resource res;
	const __be32 *ranges;
	int naddr, nsize, cell_sz, len, nentries, i;
	u32 reg_val = 0;

	child = NULL;
	for_each_child_of_node(bus_np, child) {
		if (of_address_to_resource(child, 0, &res) == 0)
			break;
	}
	if (!child) {
		dev_warn(sysc->dev, "%pOF: no addressable child found\n", bus_np);
		return -EINVAL;
	}

	if (!of_device_is_available(child)) {
		dev_dbg(sysc->dev, "%pOF: %pOF is disabled, skipping AOF setup\n",
			bus_np, child);
		of_node_put(child);
		return 0;
	}

	unit = rz_aof_lookup(soc_data, (u32)res.start);
	of_node_put(child);
	if (!unit) {
		dev_warn(sysc->dev, "%pOF: no AOF unit for base %pa\n", bus_np, &res.start);
		return -ENODEV;
	}

	naddr = of_n_addr_cells(bus_np);
	nsize = of_n_size_cells(bus_np);
	cell_sz = (naddr * 2 + nsize) * sizeof(u32);

	ranges = of_get_property(bus_np, "dma-ranges", &len);
	if (!ranges || !cell_sz || len % cell_sz) {
		dev_err(sysc->dev, "%pOF: malformed dma-ranges\n", bus_np);
		return -EINVAL;
	}
	nentries = len / cell_sz;

	for (i = 0; i < nentries; i++) {
		const __be32 *p = ranges + i * (naddr * 2 + nsize);
		u64 child_addr = of_read_number(p, naddr);
		u64 parent_addr = of_read_number(p + naddr, naddr);
		u64 size = of_read_number(p + 2 * naddr, nsize);
		unsigned int nseg = DIV_ROUND_UP_ULL(size, SZ_1G);
		unsigned int sel0 = (child_addr >> 30) & 0x3;
		u64 tgt0 = parent_addr >> 30;
		unsigned int s;

		if (child_addr & (SZ_1G - 1) || parent_addr & (SZ_1G - 1)) {
			dev_err(sysc->dev, "%pOF: entry %d not 1GiB aligned\n", bus_np, i);
			return -EINVAL;
		}

		for (s = 0; s < nseg; s++) {
			unsigned int sel = sel0 + s;
			u64 tgt = tgt0 + s;

			if (sel > 3) {
				dev_err(sysc->dev, "%pOF: entry %d overflows the 4 fields\n",
					bus_np, i);
				return -EINVAL;
			}
			if (unit->addr_bits == 35 && (tgt & BIT(5))) {
				dev_err(sysc->dev,
					"%pOF: target 0x%llx needs bit[5], unit is 35-bit only\n",
					bus_np, tgt << 30);
				return -EINVAL;
			}
			reg_val &= ~(0x3fu << (sel * 8));
			reg_val |= (tgt & 0x3f) << (sel * 8);
		}
	}

	writel(reg_val, sysc->base + SYS_AOF_OFFSET(soc_data->aof_base, unit->index));

	return 0;
}

static void rz_sysc_setup_aof(struct rz_sysc *sysc,
			      const struct rz_sysc_soc_id_init_data *soc_data)
{
	struct device_node *np;

	if (!soc_data->aof_units || !soc_data->num_aof_units)
		return;

	for_each_node_with_property(np, "dma-ranges") {
		if (!of_device_is_compatible(np, "simple-bus"))
			continue;

		if (rz_aof_parse_bus(sysc, soc_data, np))
			dev_warn(sysc->dev, "AOF setup failed for %pOF\n", np);
	}
}

static int rz_sysc_soc_init(struct rz_sysc *sysc, const struct of_device_id *match)
{
	const struct rz_sysc_init_data *sysc_data = match->data;
	const struct rz_sysc_soc_id_init_data *soc_data = sysc_data->soc_id_init_data;
	struct soc_device_attribute *soc_dev_attr;
	const char *soc_id_start, *soc_id_end;
	u32 val, revision, specific_id;
	struct soc_device *soc_dev;
	char soc_id[32] = {0};
	size_t size;

	soc_id_start = strchr(match->compatible, ',') + 1;
	soc_id_end = strchr(match->compatible, '-');
	size = soc_id_end - soc_id_start + 1;
	if (size > 32)
		size = sizeof(soc_id);
	strscpy(soc_id, soc_id_start, size);

	soc_dev_attr = devm_kzalloc(sysc->dev, sizeof(*soc_dev_attr), GFP_KERNEL);
	if (!soc_dev_attr)
		return -ENOMEM;

	soc_dev_attr->family = devm_kstrdup(sysc->dev, soc_data->family, GFP_KERNEL);
	if (!soc_dev_attr->family)
		return -ENOMEM;

	soc_dev_attr->soc_id = devm_kstrdup(sysc->dev, soc_id, GFP_KERNEL);
	if (!soc_dev_attr->soc_id)
		return -ENOMEM;

	val = readl(sysc->base + soc_data->devid_offset);
	revision = field_get(soc_data->revision_mask, val);
	specific_id = field_get(soc_data->specific_id_mask, val);
	soc_dev_attr->revision = devm_kasprintf(sysc->dev, GFP_KERNEL, "%u", revision);
	if (!soc_dev_attr->revision)
		return -ENOMEM;

	if (soc_data->id && specific_id != soc_data->id) {
		dev_warn(sysc->dev, "SoC mismatch (product = 0x%x)\n", specific_id);
		return -ENODEV;
	}

	/* Try to call SoC-specific device identification */
	if (soc_data->print_id) {
		soc_data->print_id(sysc->dev, sysc->base, soc_dev_attr);
	} else {
		dev_info(sysc->dev, "Detected Renesas %s %s Rev %s\n",
			 soc_dev_attr->family, soc_dev_attr->soc_id, soc_dev_attr->revision);
	}

	soc_dev = soc_device_register(soc_dev_attr);
	if (IS_ERR(soc_dev))
		return PTR_ERR(soc_dev);

	return 0;
}

static const struct of_device_id rz_sysc_match[] = {
#ifdef CONFIG_SYSC_R9A08G045
	{ .compatible = "renesas,r9a08g045-sysc", .data = &rzg3s_sysc_init_data },
#endif
#ifdef CONFIG_SYSC_R9A08G046
	{ .compatible = "renesas,r9a08g046-sysc", .data = &rzg3l_sysc_init_data },
#endif
#ifdef CONFIG_SYS_R9A09G047
	{ .compatible = "renesas,r9a09g047-sys", .data = &rzg3e_sys_init_data },
#endif
#ifdef CONFIG_SYS_R9A09G056
	{ .compatible = "renesas,r9a09g056-sys", .data = &rzv2n_sys_init_data },
#endif
#ifdef CONFIG_SYS_R9A09G057
	{ .compatible = "renesas,r9a09g057-sys", .data = &rzv2h_sys_init_data },
#endif
	{ }
};
MODULE_DEVICE_TABLE(of, rz_sysc_match);

static int rz_sysc_probe(struct platform_device *pdev)
{
	const struct rz_sysc_init_data *data;
	const struct of_device_id *match;
	struct device *dev = &pdev->dev;
	struct regmap *regmap;
	struct rz_sysc *sysc;
	int ret;

	struct regmap_config *regmap_cfg __free(kfree) = kzalloc_obj(*regmap_cfg);
	if (!regmap_cfg)
		return -ENOMEM;

	match = of_match_node(rz_sysc_match, dev->of_node);
	if (!match)
		return -ENODEV;

	data = match->data;

	sysc = devm_kzalloc(dev, sizeof(*sysc), GFP_KERNEL);
	if (!sysc)
		return -ENOMEM;

	sysc->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(sysc->base))
		return PTR_ERR(sysc->base);

	sysc->dev = dev;
	ret = rz_sysc_soc_init(sysc, match);
	if (ret)
		return ret;

	rz_sysc_setup_aof(sysc, data->soc_id_init_data);

	regmap_cfg->name = "rz_sysc_regs";
	regmap_cfg->reg_bits = 32;
	regmap_cfg->reg_stride = 4;
	regmap_cfg->val_bits = 32;
	regmap_cfg->fast_io = true;
	regmap_cfg->max_register = data->max_register;
	regmap_cfg->readable_reg = data->readable_reg;
	regmap_cfg->writeable_reg = data->writeable_reg;

	regmap = devm_regmap_init_mmio(dev, sysc->base, regmap_cfg);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	if (data->soc_id_init_data->pwrrdy_pwrseq &&
	    !devm_auxiliary_device_create(dev, "pwrseq-pwrrdy", regmap))
		return -ENODEV;

	return of_syscon_register_regmap(dev->of_node, regmap);
}

static struct platform_driver rz_sysc_driver = {
	.driver = {
		.name = "renesas-rz-sysc",
		.suppress_bind_attrs = true,
		.of_match_table = rz_sysc_match
	},
	.probe = rz_sysc_probe
};

static int __init rz_sysc_init(void)
{
	return platform_driver_register(&rz_sysc_driver);
}
subsys_initcall(rz_sysc_init);

MODULE_DESCRIPTION("Renesas RZ System Controller Driver");
MODULE_AUTHOR("Claudiu Beznea <claudiu.beznea.uj@bp.renesas.com>");
MODULE_LICENSE("GPL");
