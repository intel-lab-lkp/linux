// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Junhui Liu <junhui.liu@pigmoral.tech>
 */

#include <linux/bits.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/remoteproc.h>
#include <linux/reset.h>
#include <linux/regmap.h>

#include "remoteproc_internal.h"

#define CV1800B_SYS_C906L_CTRL_REG	0x04
#define CV1800B_SYS_C906L_CTRL_EN	BIT(13)

#define CV1800B_SYS_C906L_BOOTADDR_REG	0x20

/**
 * struct cv1800b_c906l - C906L remoteproc structure
 * @dev: private pointer to the device
 * @reset: reset control handle
 * @rproc: the remote processor handle
 * @syscon: regmap for accessing security system registers
 */
struct cv1800b_c906l {
	struct device *dev;
	struct reset_control *reset;
	struct rproc *rproc;
	struct regmap *syscon;
};

static int cv1800b_c906l_mem_alloc(struct rproc *rproc,
				   struct rproc_mem_entry *mem)
{
	void __iomem *va;

	va = ioremap_wc(mem->dma, mem->len);
	if (!va)
		return -ENOMEM;

	/* Update memory entry va */
	mem->va = (void *)va;

	return 0;
}

static int cv1800b_c906l_mem_release(struct rproc *rproc,
				     struct rproc_mem_entry *mem)
{
	iounmap((void __iomem *)mem->va);
	return 0;
}

static int cv1800b_c906l_add_carveout(struct rproc *rproc)
{
	struct device *dev = rproc->dev.parent;
	struct device_node *np = dev->of_node;
	struct of_phandle_iterator it;
	struct rproc_mem_entry *mem;
	struct reserved_mem *rmem;
	int i = 0;

	/* Register associated reserved memory regions */
	of_phandle_iterator_init(&it, np, "memory-region", NULL, 0);
	while (of_phandle_iterator_next(&it) == 0) {
		rmem = of_reserved_mem_lookup(it.node);
		if (!rmem) {
			of_node_put(it.node);
			return -EINVAL;
		}

		if (!strcmp(it.node->name, "vdev0buffer")) {
			mem = rproc_of_resm_mem_entry_init(&rproc->dev, i,
							   rmem->size,
							   rmem->base,
							   it.node->name);
		} else {
			mem = rproc_mem_entry_init(dev, NULL, (dma_addr_t)rmem->base,
						   rmem->size, rmem->base,
						   cv1800b_c906l_mem_alloc,
						   cv1800b_c906l_mem_release,
						   it.node->name);
		}

		if (!mem) {
			of_node_put(it.node);
			return -ENOMEM;
		}

		rproc_add_carveout(rproc, mem);
		i++;
	}

	return 0;
}

static int cv1800b_c906l_prepare(struct rproc *rproc)
{
	struct cv1800b_c906l *priv = rproc->priv;
	int ret;

	ret = cv1800b_c906l_add_carveout(rproc);
	if (ret)
		return ret;

	/*
	 * This control bit must be set to enable the C906L remote processor.
	 * Note that once the remote processor is running, merely clearing
	 * this bit will not stop its execution.
	 */
	return regmap_update_bits(priv->syscon, CV1800B_SYS_C906L_CTRL_REG,
				  CV1800B_SYS_C906L_CTRL_EN,
				  CV1800B_SYS_C906L_CTRL_EN);
}

static int cv1800b_c906l_start(struct rproc *rproc)
{
	struct cv1800b_c906l *priv = rproc->priv;
	u32 bootaddr[2];
	int ret;

	bootaddr[0] = lower_32_bits(rproc->bootaddr);
	bootaddr[1] = upper_32_bits(rproc->bootaddr);

	ret = regmap_bulk_write(priv->syscon, CV1800B_SYS_C906L_BOOTADDR_REG,
				bootaddr, ARRAY_SIZE(bootaddr));
	if (ret)
		return ret;

	return reset_control_deassert(priv->reset);
}

static int cv1800b_c906l_stop(struct rproc *rproc)
{
	struct cv1800b_c906l *priv = rproc->priv;

	return reset_control_assert(priv->reset);
}

static int cv1800b_c906l_parse_fw(struct rproc *rproc,
				  const struct firmware *fw)
{
	int ret;

	ret = rproc_elf_load_rsc_table(rproc, fw);
	if (ret == -EINVAL) {
		dev_info(&rproc->dev, "No resource table in elf\n");
		ret = 0;
	}

	return ret;
}

static const struct rproc_ops cv1800b_c906l_ops = {
	.prepare = cv1800b_c906l_prepare,
	.start = cv1800b_c906l_start,
	.stop = cv1800b_c906l_stop,
	.load = rproc_elf_load_segments,
	.parse_fw = cv1800b_c906l_parse_fw,
	.find_loaded_rsc_table = rproc_elf_find_loaded_rsc_table,
	.sanity_check = rproc_elf_sanity_check,
	.get_boot_addr = rproc_elf_get_boot_addr,
};

static int cv1800b_c906l_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct cv1800b_c906l *priv;
	struct rproc *rproc;
	const char *fw_name;
	int ret;

	ret = rproc_of_parse_firmware(dev, 0, &fw_name);
	if (ret)
		return dev_err_probe(dev, ret, "No firmware filename given\n");

	rproc = devm_rproc_alloc(dev, dev_name(dev), &cv1800b_c906l_ops,
				 fw_name, sizeof(*priv));
	if (!rproc)
		return dev_err_probe(dev, -ENOMEM,
				     "unable to allocate remoteproc\n");

	rproc->has_iommu = false;

	priv = rproc->priv;
	priv->dev = dev;
	priv->rproc = rproc;

	priv->syscon = syscon_regmap_lookup_by_phandle(np, "sophgo,syscon");
	if (IS_ERR(priv->syscon))
		return PTR_ERR(priv->syscon);

	priv->reset = devm_reset_control_get_exclusive(dev, NULL);
	if (IS_ERR(priv->reset))
		return dev_err_probe(dev, PTR_ERR(priv->reset),
				     "failed to get reset control handle\n");

	platform_set_drvdata(pdev, rproc);

	ret = devm_rproc_add(dev, rproc);
	if (ret)
		return dev_err_probe(dev, ret, "rproc_add failed\n");

	return 0;
}

static void cv1800b_c906l_remove(struct platform_device *pdev)
{
	struct rproc *rproc = platform_get_drvdata(pdev);

	rproc_del(rproc);
}

static const struct of_device_id cv1800b_c906l_of_match[] = {
	{ .compatible = "sophgo,cv1800b-c906l" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, cv1800b_c906l_of_match);

static struct platform_driver cv1800b_c906l_driver = {
	.probe = cv1800b_c906l_probe,
	.remove = cv1800b_c906l_remove,
	.driver = {
		.name = "cv1800b-c906l",
		.of_match_table = cv1800b_c906l_of_match,
	},
};

module_platform_driver(cv1800b_c906l_driver);

MODULE_AUTHOR("Junhui Liu <junhui.liu@pigmoral.tech>");
MODULE_DESCRIPTION("Sophgo CV1800B C906L remote processor control driver");
MODULE_LICENSE("GPL");
