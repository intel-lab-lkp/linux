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

const struct iommu_domain_ops nested_domain_ops = {
	.free = amd_iommu_domain_free,
};

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
