// SPDX-License-Identifier: GPL-2.0
/*
 * AMD MicroBlaze Remote Processor driver
 *
 * Copyright (C) 2026 Advanced Micro Devices, Inc.
 *
 * This driver supports a MicroBlaze remote processor managed by Linux
 * through the remoteproc framework.
 *
 * The executable firmware memory is described in the MicroBlaze-local
 * address space and translated to the Linux-visible system physical
 * address with standard devicetree address translation.
 *
 */

#include <linux/dma-mapping.h>
#include <linux/gpio/consumer.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/remoteproc.h>

#include "remoteproc_internal.h"

struct mb_rproc {
	struct device *dev;
	struct gpio_desc *reset;
};

static int mb_rproc_mem_region_map(struct rproc *rproc,
				   struct rproc_mem_entry *mem)
{
	void __iomem *va;

	va = ioremap_wc(mem->dma, mem->len);
	if (!va)
		return -ENOMEM;

	mem->va = (__force void *)va;
	mem->is_iomem = true;

	return 0;
}

static int mb_rproc_mem_region_unmap(struct rproc *rproc,
				     struct rproc_mem_entry *mem)
{
	iounmap((void __iomem *)mem->va);

	return 0;
}

static int mb_rproc_prepare(struct rproc *rproc)
{
	struct mb_rproc *mb = rproc->priv;
	struct rproc_mem_entry *mem;
	struct resource res;
	u64 da, size;
	int ret;

	ret = of_property_read_reg(mb->dev->of_node, 0, &da, &size);
	if (ret) {
		dev_err(mb->dev, "failed to parse executable memory reg\n");
		return ret;
	}

	if (!size || size > U32_MAX) {
		dev_err(mb->dev, "invalid executable memory size\n");
		return -EINVAL;
	}

	ret = of_address_to_resource(mb->dev->of_node, 0, &res);
	if (ret) {
		dev_err(mb->dev, "failed to translate executable memory reg\n");
		return ret;
	}

	mem = rproc_mem_entry_init(mb->dev, NULL, (dma_addr_t)res.start,
				   (size_t)size, da,
				   mb_rproc_mem_region_map,
				   mb_rproc_mem_region_unmap,
				   dev_name(mb->dev));
	if (!mem)
		return -ENOMEM;

	rproc_add_carveout(rproc, mem);
	rproc_coredump_add_segment(rproc, da, (size_t)size);

	return 0;
}

static int mb_rproc_start(struct rproc *rproc)
{
	struct mb_rproc *mb = rproc->priv;

	/* reset-gpios is declared active-low, so logical 0 releases reset */
	gpiod_set_value_cansleep(mb->reset, 0);

	return 0;
}

static int mb_rproc_stop(struct rproc *rproc)
{
	struct mb_rproc *mb = rproc->priv;

	/* reset-gpios is declared active-low, so logical 1 asserts reset */
	gpiod_set_value_cansleep(mb->reset, 1);

	return 0;
}

static int mb_rproc_parse_fw(struct rproc *rproc, const struct firmware *fw)
{
	int ret;

	ret = rproc_elf_load_rsc_table(rproc, fw);
	if (ret == -EINVAL) {
		dev_dbg(&rproc->dev, "no resource table found\n");
		return 0;
	}

	return ret;
}

static const struct rproc_ops mb_rproc_ops = {
	.prepare	= mb_rproc_prepare,
	.start		= mb_rproc_start,
	.stop		= mb_rproc_stop,
	.load		= rproc_elf_load_segments,
	.sanity_check	= rproc_elf_sanity_check,
	.get_boot_addr	= rproc_elf_get_boot_addr,
	.parse_fw	= mb_rproc_parse_fw,
};

static int mb_rproc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mb_rproc *mb;
	struct rproc *rproc;
	const char *fw_name = NULL;
	int ret;

	ret = rproc_of_parse_firmware(dev, 0, &fw_name);
	if (ret < 0 && ret != -EINVAL)
		return dev_err_probe(dev, ret,
				     "failed to parse firmware-name property\n");

	rproc = devm_rproc_alloc(dev, dev_name(dev), &mb_rproc_ops, fw_name,
				 sizeof(*mb));
	if (!rproc)
		return -ENOMEM;

	mb = rproc->priv;
	mb->dev = dev;

	/*
	 * Keep the MicroBlaze in reset until remoteproc has finished loading
	 * firmware into the executable memory window described by reg and
	 * translated through the parent bus ranges property.
	 */
	mb->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(mb->reset))
		return dev_err_probe(dev, PTR_ERR(mb->reset),
				     "failed to get reset gpio\n");

	rproc->auto_boot = false;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (ret)
		return dev_err_probe(dev, ret, "failed to set DMA mask\n");

	platform_set_drvdata(pdev, rproc);

	ret = devm_rproc_add(dev, rproc);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register rproc\n");

	dev_dbg(dev, "MicroBlaze remoteproc registered\n");

	return 0;
}

static const struct of_device_id mb_rproc_of_match[] = {
	{ .compatible = "amd,microblaze" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, mb_rproc_of_match);

static struct platform_driver mb_rproc_driver = {
	.probe = mb_rproc_probe,
	.driver = {
		.name = "amd-microblaze-rproc",
		.of_match_table = mb_rproc_of_match,
	},
};
module_platform_driver(mb_rproc_driver);

MODULE_DESCRIPTION("AMD MicroBlaze Remote Processor driver");
MODULE_AUTHOR("Ben Levinsky");
MODULE_LICENSE("GPL");
