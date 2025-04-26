// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2024, NVIDIA CORPORATION & AFFILIATES
 */
#include "iommufd_private.h"

void iommufd_viommu_destroy(struct iommufd_object *obj)
{
	struct iommufd_viommu *viommu =
		container_of(obj, struct iommufd_viommu, obj);

	if (viommu->ops && viommu->ops->destroy)
		viommu->ops->destroy(viommu);
	refcount_dec(&viommu->hwpt->common.obj.users);
	xa_destroy(&viommu->vdevs);
}

int iommufd_viommu_alloc_ioctl(struct iommufd_ucmd *ucmd)
{
	struct iommu_viommu_alloc *cmd = ucmd->cmd;
	const struct iommu_user_data user_data = {
		.type = cmd->type,
		.uptr = u64_to_user_ptr(cmd->data_uptr),
		.len = cmd->data_len,
	};
	struct iommufd_hwpt_paging *hwpt_paging;
	struct iommufd_viommu *viommu;
	struct iommufd_device *idev;
	const struct iommu_ops *ops;
	int rc;

	if (cmd->flags || cmd->type == IOMMU_VIOMMU_TYPE_DEFAULT)
		return -EOPNOTSUPP;

	idev = iommufd_get_device(ucmd, cmd->dev_id);
	if (IS_ERR(idev))
		return PTR_ERR(idev);

	ops = dev_iommu_ops(idev->dev);
	if (!ops->viommu_alloc) {
		rc = -EOPNOTSUPP;
		goto out_put_idev;
	}

	hwpt_paging = iommufd_get_hwpt_paging(ucmd, cmd->hwpt_id);
	if (IS_ERR(hwpt_paging)) {
		rc = PTR_ERR(hwpt_paging);
		goto out_put_idev;
	}

	if (!hwpt_paging->nest_parent) {
		rc = -EINVAL;
		goto out_put_hwpt;
	}

	viommu = ops->viommu_alloc(idev->dev, hwpt_paging->common.domain,
				   ucmd->ictx, cmd->type,
				   user_data.len ? &user_data : NULL);
	if (IS_ERR(viommu)) {
		rc = PTR_ERR(viommu);
		goto out_put_hwpt;
	}

	xa_init(&viommu->vdevs);
	viommu->type = cmd->type;
	viommu->ictx = ucmd->ictx;
	viommu->hwpt = hwpt_paging;
	refcount_inc(&viommu->hwpt->common.obj.users);
	INIT_LIST_HEAD(&viommu->veventqs);
	init_rwsem(&viommu->veventqs_rwsem);
	/*
	 * It is the most likely case that a physical IOMMU is unpluggable. A
	 * pluggable IOMMU instance (if exists) is responsible for refcounting
	 * on its own.
	 */
	viommu->iommu_dev = __iommu_get_iommu_dev(idev->dev);

	cmd->out_viommu_id = viommu->obj.id;
	rc = iommufd_ucmd_respond(ucmd, sizeof(*cmd));
	if (rc)
		goto out_abort;
	iommufd_object_finalize(ucmd->ictx, &viommu->obj);
	goto out_put_hwpt;

out_abort:
	iommufd_object_abort_and_destroy(ucmd->ictx, &viommu->obj);
out_put_hwpt:
	iommufd_put_object(ucmd->ictx, &hwpt_paging->common.obj);
out_put_idev:
	iommufd_put_object(ucmd->ictx, &idev->obj);
	return rc;
}

void iommufd_vdevice_destroy(struct iommufd_object *obj)
{
	struct iommufd_vdevice *vdev =
		container_of(obj, struct iommufd_vdevice, obj);
	struct iommufd_viommu *viommu = vdev->viommu;

	if (viommu->ops && viommu->ops->vdevice_destroy)
		viommu->ops->vdevice_destroy(vdev);

	/* xa_cmpxchg is okay to fail if alloc failed xa_cmpxchg previously */
	xa_cmpxchg(&viommu->vdevs, vdev->id, vdev, NULL, GFP_KERNEL);
	refcount_dec(&viommu->obj.users);
	put_device(vdev->dev);
}

int iommufd_vdevice_alloc_ioctl(struct iommufd_ucmd *ucmd)
{
	struct iommu_vdevice_alloc *cmd = ucmd->cmd;
	struct iommufd_vdevice *vdev, *curr;
	struct iommufd_viommu *viommu;
	struct iommufd_device *idev;
	u64 virt_id = cmd->virt_id;
	int rc = 0;

	/* virt_id indexes an xarray */
	if (virt_id > ULONG_MAX)
		return -EINVAL;

	viommu = iommufd_get_viommu(ucmd, cmd->viommu_id);
	if (IS_ERR(viommu))
		return PTR_ERR(viommu);

	idev = iommufd_get_device(ucmd, cmd->dev_id);
	if (IS_ERR(idev)) {
		rc = PTR_ERR(idev);
		goto out_put_viommu;
	}

	if (viommu->iommu_dev != __iommu_get_iommu_dev(idev->dev)) {
		rc = -EINVAL;
		goto out_put_idev;
	}

	if (viommu->ops && viommu->ops->vdevice_alloc)
		vdev = viommu->ops->vdevice_alloc(viommu, idev->dev, virt_id);
	else
		vdev = iommufd_object_alloc(ucmd->ictx, vdev,
					    IOMMUFD_OBJ_VDEVICE);
	if (IS_ERR(vdev)) {
		rc = PTR_ERR(vdev);
		goto out_put_idev;
	}

	vdev->id = virt_id;
	vdev->dev = idev->dev;
	get_device(idev->dev);
	vdev->viommu = viommu;
	refcount_inc(&viommu->obj.users);

	curr = xa_cmpxchg(&viommu->vdevs, virt_id, NULL, vdev, GFP_KERNEL);
	if (curr) {
		rc = xa_err(curr) ?: -EEXIST;
		goto out_abort;
	}

	cmd->out_vdevice_id = vdev->obj.id;
	rc = iommufd_ucmd_respond(ucmd, sizeof(*cmd));
	if (rc)
		goto out_abort;
	iommufd_object_finalize(ucmd->ictx, &vdev->obj);
	goto out_put_idev;

out_abort:
	iommufd_object_abort_and_destroy(ucmd->ictx, &vdev->obj);
out_put_idev:
	iommufd_put_object(ucmd->ictx, &idev->obj);
out_put_viommu:
	iommufd_put_object(ucmd->ictx, &viommu->obj);
	return rc;
}

void iommufd_vcmdq_destroy(struct iommufd_object *obj)
{
	struct iommufd_vcmdq *vcmdq =
		container_of(obj, struct iommufd_vcmdq, obj);
	struct iommufd_viommu *viommu = vcmdq->viommu;

	if (viommu->ops->vcmdq_destroy)
		viommu->ops->vcmdq_destroy(vcmdq);
	iopt_unpin_pages(&viommu->hwpt->ioas->iopt, vcmdq->addr, vcmdq->length);
	refcount_dec(&viommu->obj.users);
}

int iommufd_vcmdq_alloc_ioctl(struct iommufd_ucmd *ucmd)
{
	struct iommu_vcmdq_alloc *cmd = ucmd->cmd;
	struct iommufd_viommu *viommu;
	struct iommufd_vcmdq *vcmdq;
	struct page **pages;
	int max_npages, i;
	dma_addr_t end;
	int rc;

	if (cmd->flags || cmd->type == IOMMU_VCMDQ_TYPE_DEFAULT)
		return -EOPNOTSUPP;
	if (!cmd->addr || !cmd->length)
		return -EINVAL;
	if (check_add_overflow(cmd->addr, cmd->length - 1, &end))
		return -EOVERFLOW;

	max_npages = DIV_ROUND_UP(cmd->length, PAGE_SIZE);
	pages = kcalloc(max_npages, sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return -ENOMEM;

	viommu = iommufd_get_viommu(ucmd, cmd->viommu_id);
	if (IS_ERR(viommu)) {
		rc = PTR_ERR(viommu);
		goto out_free;
	}

	if (!viommu->ops || !viommu->ops->vcmdq_alloc) {
		rc = -EOPNOTSUPP;
		goto out_put_viommu;
	}

	/* Quick test on the base address */
	if (!iommu_iova_to_phys(viommu->hwpt->common.domain, cmd->addr)) {
		rc = -ENXIO;
		goto out_put_viommu;
	}

	/* The underlying physical pages must be pinned in the IOAS */
	rc = iopt_pin_pages(&viommu->hwpt->ioas->iopt, cmd->addr, cmd->length,
			    pages, 0);
	if (rc)
		goto out_put_viommu;

	/* Validate if the underlying physical pages are contiguous */
	for (i = 1; i < max_npages && pages[i]; i++) {
		if (page_to_pfn(pages[i]) == page_to_pfn(pages[i - 1]) + 1)
			continue;
		rc = -EFAULT;
		goto out_unpin;
	}

	vcmdq = viommu->ops->vcmdq_alloc(viommu, cmd->type, cmd->index,
					 cmd->addr, cmd->length);
	if (IS_ERR(vcmdq)) {
		rc = PTR_ERR(vcmdq);
		goto out_unpin;
	}

	vcmdq->viommu = viommu;
	refcount_inc(&viommu->obj.users);
	vcmdq->addr = cmd->addr;
	vcmdq->ictx = ucmd->ictx;
	vcmdq->length = cmd->length;
	cmd->out_vcmdq_id = vcmdq->obj.id;
	rc = iommufd_ucmd_respond(ucmd, sizeof(*cmd));
	if (rc)
		iommufd_object_abort_and_destroy(ucmd->ictx, &vcmdq->obj);
	else
		iommufd_object_finalize(ucmd->ictx, &vcmdq->obj);
	goto out_put_viommu;

out_unpin:
	iopt_unpin_pages(&viommu->hwpt->ioas->iopt, cmd->addr, cmd->length);
out_put_viommu:
	iommufd_put_object(ucmd->ictx, &viommu->obj);
out_free:
	kfree(pages);
	return rc;
}
