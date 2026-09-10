// SPDX-License-Identifier: GPL-2.0-only
/*
 * TDISP devices can have separate T=0 and T=1 DMA streams that terminate in
 * different IOMMU environments. The Linux IOMMU driver manages the T=1 stream
 * while the T=0 stream uses the physical path.
 *
 *   [device: T=0] ===> [hypervisor] ===> [memory]
 *       |
 *       +-- (guest association) --> [confidential IOMMU: blocking DMA]
 */
#include <linux/pci.h>
#include <linux/pci-ats.h>

#include "iommu-priv.h"

/**
 * iommu_tdisp_enter_t0 - Prepare @dev while TDISP is T=0
 * @dev: device entering TDISP T=0
 *
 * The IOMMU core invokes this helper for a device on a confidential IOMMU
 * while TDISP is T=0. The device remains associated with its IOMMU while the
 * core owns DMA operations required in this state. Enable ATS here when the
 * device requires it, since the IOMMU driver does not operate ATS while in
 * BLOCKED.
 */
void iommu_tdisp_enter_t0(struct device *dev)
{
	struct pci_dev *pdev;
	int ret;

	if (!dev_is_pci(dev))
		return;

	pdev = to_pci_dev(dev);
	if (!pci_ats_required(pdev))
		return;

	ret = pci_enable_ats(pdev, PCI_ATS_MIN_STU);
	if (ret)
		dev_warn(dev, "cannot enable ATS while TDISP is T=0\n");
}

/**
 * iommu_tdisp_exit_t0 - Undo iommu_tdisp_enter_t0()
 * @dev: device leaving TDISP T=0
 */
void iommu_tdisp_exit_t0(struct device *dev)
{
	struct pci_dev *pdev;

	if (!dev_is_pci(dev))
		return;

	pdev = to_pci_dev(dev);
	if (pci_ats_required(pdev) && pdev->ats_enabled)
		pci_disable_ats(pdev);
}
