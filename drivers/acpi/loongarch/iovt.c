// SPDX-License-Identifier: GPL-2.0-only

#include <linux/acpi.h>
#include <linux/pci.h>
#include "init.h"

struct iovt_fwnode {
	struct list_head list;
	struct fwnode_handle *fwnode;
	int flag;
	int segment;
	int devid;
	int nid;
};

/* Root pointer to the mapped IOVT table */
static LIST_HEAD(iovt_fwnode_list);
static DEFINE_SPINLOCK(iovt_fwnode_lock);

#ifdef CONFIG_PCI
static void __init iovt_enable_acs(struct acpi_iovt_iommu *iommu)
{
	static bool acs_enabled __initdata;

	if (acs_enabled)
		return;

	/* IOMMU V1 only supports PCI device management */
	if ((iommu->header.type == ACPI_IOVT_IOMMU_V1) ||
		(iommu->flags & (ACPI_IOVT_PCI_DEVICE | ACPI_IOVT_MAGAGE_BY_SEGMENT))) {
		pci_request_acs();
		acs_enabled = true;
	}
}
#else
static inline void iovt_enable_acs(struct acpi_iovt_iommu *iommu) { }
#endif

static int __init iovt_get_pci_iommu_fwnode(struct iovt_fwnode *np, u16 segment, u16 bdf)
{
	struct pci_dev *pdev;
	struct fwnode_handle *fwnode;

	pdev = pci_get_domain_bus_and_slot(segment, PCI_BUS_NUM(bdf), bdf & 0xff);
	if (!pdev) {
		pr_err("Could not find PCI IOMMU\n");
		return -ENODEV;
	}

	fwnode = dev_fwnode(&pdev->dev);
	if (!fwnode) {
		/*
		 * PCI devices aren't necessarily described by ACPI. Create a
		 * fwnode so the IOMMU subsystem can identify this device.
		 */
		fwnode = acpi_alloc_fwnode_static();
		if (!fwnode) {
			pci_dev_put(pdev);
			return -ENOMEM;
		}
		set_primary_fwnode(&pdev->dev, fwnode);
	}

	np->fwnode = dev_fwnode(&pdev->dev);
	if (np->flag & ACPI_IOVT_PXM_VALID)
		set_dev_node(&pdev->dev, np->nid);
	pci_dev_put(pdev);
	return 0;
}

static int __init iovt_add_iommu(struct acpi_iovt_iommu *iommu)
{
	struct iovt_fwnode *np;
	struct fwnode_handle *fwnode;
	int ret;

	np = kzalloc_obj(struct iovt_fwnode, GFP_ATOMIC);
	if (WARN_ON(!np))
		return -ENOMEM;

	INIT_LIST_HEAD(&np->list);
	np->flag = iommu->flags;
	np->segment = iommu->segment;
	if (np->flag & ACPI_IOVT_PXM_VALID)
		np->nid = pxm_to_node(iommu->proximity_domain);

	if (np->flag & ACPI_IOVT_PCI_DEVICE) {
		np->devid = iommu->device_id;
		ret = iovt_get_pci_iommu_fwnode(np, np->segment, np->devid);
		if (ret) {
			kfree(np);
			return ret;
		}

	} else {
		fwnode = acpi_alloc_fwnode_static();
		if (!fwnode) {
			kfree(np);
			return -ENOMEM;
		}

		np->fwnode = fwnode;
	}

	spin_lock(&iovt_fwnode_lock);
	list_add_tail(&np->list, &iovt_fwnode_list);
	spin_unlock(&iovt_fwnode_lock);
	return 0;
}

static void __init iovt_init_devices(struct acpi_table_header *header)
{
	struct acpi_iovt_iommu *iommu;
	struct acpi_table_iovt *iovt;
	int i;

	iovt = (struct acpi_table_iovt *)header;

	/* Get the first IOVT node */
	iommu = ACPI_ADD_PTR(struct acpi_iovt_iommu, iovt, iovt->iommu_offset);
	for (i = 0; i < iovt->iommu_count; i++) {
		iovt_add_iommu(iommu);
		iommu = ACPI_ADD_PTR(struct acpi_iovt_iommu, iommu, iommu->header.length);
	}
}

void __init acpi_iovt_init(void)
{
	acpi_status status;
	struct acpi_table_header *hdr;
	struct acpi_table_iovt *iovt;
	struct acpi_iovt_iommu *iommu;
	int i;

	status = acpi_get_table(ACPI_SIG_IOVT, 0, &hdr);
	if (ACPI_FAILURE(status)) {
		if (status != AE_NOT_FOUND) {
			const char *msg = acpi_format_exception(status);

			pr_err("Failed to get table, %s\n", msg);
		}

		return;
	}

	iovt = (struct acpi_table_iovt *)&hdr;
	iommu = ACPI_ADD_PTR(struct acpi_iovt_iommu, iovt, iovt->iommu_offset);
	for (i = 0; i < iovt->iommu_count; i++) {
		iovt_enable_acs(iommu);

		iommu = ACPI_ADD_PTR(struct acpi_iovt_iommu, iommu, iommu->header.length);
	}

	acpi_put_table(hdr);
}

void __init acpi_iovt_late_init(void)
{
	acpi_status status;
	struct acpi_table_header *hdr;

	status = acpi_get_table(ACPI_SIG_IOVT, 0, &hdr);
	if (ACPI_FAILURE(status)) {
		if (status != AE_NOT_FOUND) {
			const char *msg = acpi_format_exception(status);

			pr_err("Failed to get table, %s\n", msg);
		}

		return;
	}

	iovt_init_devices(hdr);
	acpi_put_table(hdr);
}
