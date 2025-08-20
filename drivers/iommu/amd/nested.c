// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2023 Advanced Micro Devices, Inc.
 */

#define pr_fmt(fmt)     "AMD-Vi: " fmt
#define dev_fmt(fmt)    pr_fmt(fmt)

#include <linux/iommu.h>
#include <uapi/linux/iommufd.h>

#include "amd_iommu.h"
#include "amd_iommu_types.h"

const struct iommu_domain_ops nested_domain_ops;

static int udata_to_iommu_hwpt_amd_v2(const struct iommu_user_data *user_data,
				       struct iommu_hwpt_amd_v2 *hwpt)
{
	if (!user_data)
		return -EINVAL;

	if (user_data->type != IOMMU_HWPT_DATA_AMD_V2)
		return -EOPNOTSUPP;

	return iommu_copy_struct_from_user(hwpt, user_data,
					   IOMMU_HWPT_DATA_AMD_V2,
					   dte);
}

struct iommu_domain *
amd_iommu_domain_alloc_nested(struct device *dev, struct iommu_domain *parent,
			      u32 flags, const struct iommu_user_data *user_data)
{
	int ret;
	struct iommu_hwpt_amd_v2 hwpt;
	struct protection_domain *pdom;

	if (parent->ops != amd_iommu_ops.default_domain_ops)
		return ERR_PTR(-EINVAL);

	ret = udata_to_iommu_hwpt_amd_v2(user_data, &hwpt);
	if (ret)
		return ERR_PTR(ret);

	pdom = kzalloc(sizeof(*pdom), GFP_KERNEL);
	if (IS_ERR(pdom))
		return ERR_PTR(-ENOMEM);

	pdom->id = amd_iommu_pdom_id_alloc();
	if (!pdom->id)
		goto out_err;

	pr_debug("%s: Allocating nested domain with parent domid=%#x\n",
		 __func__, to_pdomain(parent)->id);

	spin_lock_init(&pdom->lock);
	INIT_LIST_HEAD(&pdom->dev_list);
	INIT_LIST_HEAD(&pdom->dev_data_list);
	xa_init(&pdom->iommu_array);

	pdom->pd_mode = PD_MODE_V2;
	pdom->iop.pgtbl.cfg.amd.nid = NUMA_NO_NODE;
	pdom->parent = to_pdomain(parent);
	pdom->domain.ops = &nested_domain_ops;
	pdom->domain.type = IOMMU_DOMAIN_NESTED;
	pdom->domain.geometry.aperture_start = 0;
	pdom->domain.geometry.aperture_end = ((1ULL << PM_LEVEL_SHIFT(amd_iommu_gpt_level)) - 1);
	pdom->domain.geometry.force_aperture = true;
	pdom->domain.pgsize_bitmap = pdom->iop.pgtbl.cfg.pgsize_bitmap;
	memcpy(&pdom->guest_hwpt, &hwpt, sizeof(struct iommu_hwpt_amd_v2));

	return &pdom->domain;
out_err:
	kfree(pdom);
	return ERR_PTR(-EINVAL);
}

static inline u64 hwpt_to_gcr3_trp(u64 *dte)
{
	u64 gcr3;

	gcr3  = (FIELD_GET(DTE_GCR3_14_12, dte[0]) << 12);
	gcr3 |= (FIELD_GET(DTE_GCR3_30_15, dte[1]) << 15);
	gcr3 |= (FIELD_GET(DTE_GCR3_51_31, dte[1]) << 31);
	return gcr3;
}

static int nested_gcr3_update(struct protection_domain *pdom, struct device *dev)
{
	struct iommu_dev_data *dev_data = dev_iommu_priv_get(dev);
	struct iommu_hwpt_amd_v2 *hwpt = &pdom->guest_hwpt;
	struct pci_dev *pdev = to_pci_dev(dev);

	if (!pdev || !hwpt)
		return -EINVAL;

	/* Note: Currently only support GCR3TRPMode with nested translation */
	if (!check_feature2(FEATURE_GCR3TRPMODE))
		return -EOPNOTSUPP;

	if (FIELD_GET(DTE_GPT_LEVEL_MASK, hwpt->dte[2]) == GUEST_PGTABLE_5_LEVEL)
		pdom->guest_paging_mode = PAGE_MODE_5_LEVEL;
	else
		pdom->guest_paging_mode = PAGE_MODE_4_LEVEL;

	dev_data->ppr = FIELD_GET(DTE_FLAG_PPR, hwpt->dte[0]);
	dev_data->gcr3_info.glx = FIELD_GET(DTE_FLAG_GLX, hwpt->dte[0]);
	dev_data->gcr3_info.giov = FIELD_GET(DTE_FLAG_GIOV, hwpt->dte[0]);
	dev_data->gcr3_info.trp_gpa = hwpt_to_gcr3_trp(hwpt->dte);
	/* Due to possible aliasing issue use nested domain ID */
	dev_data->gcr3_info.domid = pdom->id;
	pr_debug("%s: devid=%#x, domid=%#x, trp_gpa=%#llx, glx=%#x\n", __func__,
		 pci_dev_id(pdev),
		 dev_data->gcr3_info.domid,
		 dev_data->gcr3_info.trp_gpa,
		 dev_data->gcr3_info.glx);

	return 0;
}

static int amd_iommu_nested_attach_device(struct iommu_domain *dom, struct device *dev)
{
	struct iommu_dev_data *dev_data = dev_iommu_priv_get(dev);
	struct protection_domain *pdom = to_pdomain(dom);
	struct pci_dev *pdev;
	int ret;

	if (dev_data->domain == pdom)
		return 0;

	ret = nested_gcr3_update(pdom, dev);
	if (ret)
		return ret;

	if (dev_data->domain)
		amd_iommu_detach_device(dev);

	ret = __amd_iommu_attach_device(dev, pdom);
	if (ret)
		return ret;

	pdev = dev_is_pci(dev_data->dev) ? to_pci_dev(dev_data->dev) : NULL;
	if (pdev)
		amd_iommu_pdev_enable_cap_ats(pdev);

	return ret;
}

const struct iommu_domain_ops nested_domain_ops = {
	.attach_dev = amd_iommu_nested_attach_device,
	.free = amd_iommu_domain_free,
};
