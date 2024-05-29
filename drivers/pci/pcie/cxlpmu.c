// SPDX-License-Identifier: GPL-2.0
/*
 * Discovery of presence of CXL PMU instances and the maximum irqnum.
 * Registers a auxiliary_device to which a driver can bind after the
 * CXL bus is available and new devices can be added to ti.
 */
#include <linux/kernel.h>
#include <linux/idr.h>
#include <linux/list.h>
#include <linux/pci.h>
#include <linux/platform_device.h>

#include "portdrv.h"
#include "../../cxl/cxl.h"
#include "../../cxl/cxlpci.h"
#include "../../cxl/pmu.h"

static DEFINE_IDA(pcie_cxl_pmu_ida);
static void cpmu_adev_release(struct device *dev)
{
	struct auxiliary_device *adev = to_auxiliary_dev(dev);

	ida_free(&pcie_cxl_pmu_ida, adev->id);
}

int pcie_cxl_pmu_get_irqs(struct pci_dev *dev, u32 *max_irq,
			  struct list_head *aux_dev_list)
{
	u32 regblocks, regloc_size;
	int i, regloc, ret;
	bool found = false;

	regloc = pci_find_dvsec_capability(dev, PCI_VENDOR_ID_CXL,
					   CXL_DVSEC_REG_LOCATOR);
	if (!regloc)
		return -ENODEV;

	pci_read_config_dword(dev, regloc + PCI_DVSEC_HEADER1, &regloc_size);
	regloc_size = FIELD_GET(PCI_DVSEC_HEADER1_LENGTH_MASK, regloc_size);

	regloc += CXL_DVSEC_REG_LOCATOR_BLOCK1_OFFSET;
	regblocks = (regloc_size - CXL_DVSEC_REG_LOCATOR_BLOCK1_OFFSET) / 8;

	for (i = 0; i < regblocks; i++, regloc += 8) {
		u32 reg_lo, reg_hi;
		u8 reg_type;
		struct resource *res;
		void __iomem *base;
		u64 offset, val;
		int bar;

		pci_read_config_dword(dev, regloc, &reg_lo);
		reg_type = FIELD_GET(CXL_DVSEC_REG_LOCATOR_BLOCK_ID_MASK,
				     reg_lo);
		if (reg_type != CXL_REGLOC_RBI_PMU)
			continue;

		found = true;
		/* Now we need to map just enough to get the irq */
		bar = FIELD_GET(CXL_DVSEC_REG_LOCATOR_BIR_MASK, reg_lo);
		pci_read_config_dword(dev, regloc + 4, &reg_hi);

		offset = ((u64) reg_hi << 32) |
			(reg_lo & CXL_DVSEC_REG_LOCATOR_BLOCK_OFF_LOW_MASK);
		if (offset > pci_resource_len(dev, bar)) {
			pci_warn(dev, "CPMU BAR%d: %pr: too small\n",
				bar, &dev->resource[bar]);
			continue;
		}
		/*
		 * Map only the CPMU region because other parts are in control
		 * of the CXL port driver.
		 */
		res = request_mem_region(pci_resource_start(dev, bar) + offset,
					 CXL_PMU_REGMAP_SIZE, NULL);
		if (!res) {
			pci_err(dev, "CPMU: could not map\n");
			continue;
		}

		base = ioremap(pci_resource_start(dev, bar) + offset,
		       CXL_PMU_REGMAP_SIZE);
		if (!base) {
			pci_err(dev, "CPU: ioremap fail\n");
			release_mem_region(res->start, resource_size(res));
			continue;
		}
		if (max_irq) {
			val = readq(base + CXL_PMU_CAP_REG);
			if (FIELD_GET(CXL_PMU_CAP_INT, val))
				*max_irq = max(*max_irq,
					       (u32)FIELD_GET(CXL_PMU_CAP_MSI_N_MSK, val));
		}
		iounmap(base);
		release_mem_region(res->start, resource_size(res));

		if (aux_dev_list) {
			struct pcie_port_aux_dev *pcie_adev;
			int id;

			pcie_adev = devm_kzalloc(&dev->dev, sizeof(*pcie_adev),
						GFP_KERNEL);
			if (!pcie_adev)
				return -ENOMEM;

			/* Cleanup handled by release after devm_pcie_port_aux_dev_init() */
			id = ida_alloc(&pcie_cxl_pmu_ida, GFP_KERNEL);
			if (id < 0)
				return -ENOMEM;

			pcie_adev->adev.name = "cpmu";
			pcie_adev->adev.id = id;
			pcie_adev->adev.dev.parent = &dev->dev;
			pcie_adev->adev.dev.release = cpmu_adev_release;
			pcie_adev->addr = pci_resource_start(dev, bar) + offset;
			pcie_adev->optional = true;

			ret = devm_pcie_port_aux_dev_init(&dev->dev, pcie_adev);
			if (ret)
				return ret;

			list_add(&pcie_adev->node, aux_dev_list);
		}
	}
	if (!found)
		return -ENODEV;

	return 0;
}
