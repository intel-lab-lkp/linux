// SPDX-License-Identifier: GPL-2.0-only

#include <linux/acpi.h>
#include <linux/pci.h>
#include "init.h"

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
