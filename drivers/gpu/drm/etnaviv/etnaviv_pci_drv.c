// SPDX-License-Identifier: GPL-2.0

#include <linux/component.h>
#include <linux/pci.h>

#include "etnaviv_drv.h"
#include "etnaviv_pci_drv.h"

enum etnaviv_pci_gpu_chip_id {
	GC_CORE_UNKNOWN = 0,
	JM9100 = 1,
	JD9230P = 2,
	GP102 = 3,
	GC1000_IN_LS7A1000 = 4,
	GC1000_IN_LS2K1000 = 5,
	GC_CORE_PCI_LAST,
};

struct etnaviv_pci_gpu_data {
	enum etnaviv_pci_gpu_chip_id chip_id;
	u32 num_core;
	u32 num_vram;
	u32 vram_bars[2];
	u32 mmio_bar;
	struct {
		u32 id;
		u32 offset;
		u32 size;
		char compatible[20];
	} cores[ETNA_MAX_PIPES];

	bool has_dedicated_vram;
	char market_name[24];
};

static const struct etnaviv_pci_gpu_data
gc_core_plaform_data[GC_CORE_PCI_LAST] = {
	{
		.chip_id = GC_CORE_UNKNOWN,
	},
	{
		.chip_id = JM9100,
		.num_core = 1,
		.num_vram = 2,
		.vram_bars = {0, 2},
		.mmio_bar = 1,
		.cores = {{0, 0x00900000, 0x00010000, "etnaviv-gpu,3d"},},
		.has_dedicated_vram = true,
		.market_name = "JingJia Micro JM9100",
	},
	{
		.chip_id = JD9230P,
		.num_core = 2,
		.num_vram = 2,
		.vram_bars = {0, 2},
		.mmio_bar = 1,
		.cores = {{0, 0x00900000, 0x00010000, "etnaviv-gpu,3d"},
			  {1, 0x00910000, 0x00010000, "etnaviv-gpu,3d"},},
		.has_dedicated_vram = true,
		.market_name = "JingJia Micro JD9230P",
	},
	{
		.chip_id = GP102,
		.num_core = 2,
		.num_vram = 1,
		.vram_bars = {0,},
		.mmio_bar = 2,
		.cores = {{0, 0x00040000, 0x00010000, "etnaviv-gpu,3d"},
			  {0, 0x000C0000, 0x00010000, "etnaviv-gpu,2d"},},
		.has_dedicated_vram = true,
		.market_name = "LingJiu GP102",
	},
	{
		.chip_id = GC1000_IN_LS7A1000,
		.num_core = 1,
		.num_vram = 1,
		.vram_bars = {2,},
		.mmio_bar = 0,
		.cores = {{0, 0, 0x00010000, "etnaviv-gpu,3d"}, {}, {}, {}},
		.has_dedicated_vram = true,
		.market_name = "GC1000 in LS7A1000",
	},
	{
		.chip_id = GC1000_IN_LS2K1000,
		.num_core = 1,
		.num_vram = 0,
		.mmio_bar = 0,
		.cores = {{0, 0, 0x00010000, "etnaviv-gpu,3d"}, {}, {}, {}},
		.has_dedicated_vram = false,
		.market_name = "GC1000 in LS2K1000",
	},
};

static const struct etnaviv_pci_gpu_data *
etnaviv_pci_get_platform_data(const struct pci_device_id *entity)
{
	enum etnaviv_pci_gpu_chip_id chip_id = entity->driver_data;
	static const struct etnaviv_pci_gpu_data *pdata;

	pdata = &gc_core_plaform_data[chip_id];
	if (!pdata || pdata->chip_id == GC_CORE_UNKNOWN)
		return NULL;

	return pdata;
}

extern const struct component_master_ops etnaviv_master_ops;

static int etnaviv_pci_probe(struct pci_dev *pdev,
			     const struct pci_device_id *ent)
{
	const struct etnaviv_pci_gpu_data *pdata;
	struct device *dev = &pdev->dev;
	struct component_match *matches = NULL;
	unsigned int i;
	unsigned int num_core;
	int ret;

	ret = pcim_enable_device(pdev);
	if (ret) {
		dev_err(dev, "failed to enable\n");
		return ret;
	}

	pci_set_master(pdev);

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	pdata = etnaviv_pci_get_platform_data(ent);
	if (!pdata)
		return -ENODEV;

	num_core = pdata->num_core;

	dev_info(dev, "%s has %u GPU cores\n", pdata->market_name, num_core);

	/*
	 * Create a virtual platform device for the sub-component,
	 * a sub-component is refer to a single vivante GPU core.
	 * But it can also be extended to stand for a display controller
	 * or any other IP core attached via the same PCIe master.
	 */
	for (i = 0; i < num_core; i++) {
		struct platform_device *virtual_child;
		resource_size_t start, offset, size;
		struct resource res;

		start = pci_resource_start(pdev, pdata->mmio_bar);
		offset = pdata->cores[i].offset;
		size = pdata->cores[i].size;

		memset(&res, 0, sizeof(res));
		res.flags = IORESOURCE_MEM;
		res.name = "reg";
		res.start = start + offset;
		res.end = start + offset + size - 1;

		ret = etnaviv_create_platform_device(dev,
						     pdata->cores[i].compatible,
						     pdata->cores[i].id,
						     &res,
						     (void *)pdata,
						     &virtual_child);
		if (ret)
			return ret;

		component_match_add(dev, &matches, component_compare_dev,
				    &virtual_child->dev);
	}

	ret = component_master_add_with_match(dev, &etnaviv_master_ops, matches);

	return ret;
}

static int platform_device_remove_callback(struct device *dev, void *data)
{
	struct platform_device *pdev = to_platform_device(dev);

	etnaviv_destroy_platform_device(&pdev);

	return 0;
}

static void etnaviv_pci_remove(struct pci_dev *pdev)
{
	struct device *dev = &pdev->dev;

	component_master_del(dev, &etnaviv_master_ops);

	device_for_each_child(dev, NULL, platform_device_remove_callback);

	pci_clear_master(pdev);
}

static const struct pci_device_id etnaviv_pci_id_lists[] = {
	{0x0731, 0x9100, PCI_ANY_ID, PCI_ANY_ID, 0, 0, JM9100},
	{0x0731, 0x9230, PCI_ANY_ID, PCI_ANY_ID, 0, 0, JD9230P},
	{0x0709, 0x0001, PCI_ANY_ID, PCI_ANY_ID, 0, 0, GP102},
	{0x0014, 0x7A15, PCI_ANY_ID, PCI_ANY_ID, 0, 0, GC1000_IN_LS7A1000},
	{0x0014, 0x7A05, PCI_ANY_ID, PCI_ANY_ID, 0, 0, GC1000_IN_LS2K1000},
	{ }
};

static struct pci_driver etnaviv_pci_driver = {
	.name = "etnaviv",
	.id_table = etnaviv_pci_id_lists,
	.probe = etnaviv_pci_probe,
	.remove = etnaviv_pci_remove,
};

int etnaviv_register_pci_driver(void)
{
	return pci_register_driver(&etnaviv_pci_driver);
}

void etnaviv_unregister_pci_driver(void)
{
	pci_unregister_driver(&etnaviv_pci_driver);
}

MODULE_DEVICE_TABLE(pci, etnaviv_pci_id_lists);
