// SPDX-License-Identifier: GPL-2.0+
/*
 * Virtual NVMe controller implementation
 * Copyright (c) 2019 - Maxim Levitsky
 */
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/mdev.h>
#include <linux/nvme.h>
#include "priv.h"

bool nvmet_mdev_vctrl_is_dead(struct nvmet_mdev_vctrl *vctrl)
{
	return (vctrl->mmio.csts & (NVME_CSTS_CFS | NVME_CSTS_SHST_MASK)) != 0;
}

/* Create a new virtual controller */
int nvmet_mdev_vctrl_create(struct nvmet_mdev_vctrl *vctrl,
			   struct mdev_device *mdev)
{
	int ret;

	/* Basic init */
	vctrl->mdev = mdev;
	vctrl->viommu.vctrl = vctrl;

	kref_init(&vctrl->ref);
	mutex_init(&vctrl->lock);

	/* default feature values */
	vctrl->arb_burst_shift = 3;
	vctrl->mmio.shadow_db_supported = vctrl->nvmet_ctrl->shadow_db;

	ret = nvmet_mdev_pci_create(vctrl);
	if (ret)
		return ret;

	ret = nvmet_mdev_mmio_create(vctrl);
	if (ret)
		goto free_pci;

	nvmet_mdev_irqs_setup(vctrl);

	ret = nvmet_mdev_io_create(vctrl);
	if (ret)
		goto free_mmio;

	_INFO(vctrl, "device created\n");
	return 0;

free_mmio:
	nvmet_mdev_mmio_free(vctrl);
free_pci:
	nvmet_mdev_pci_free(vctrl);
	return ret;
}

/* Try to destroy an vctrl */
void nvmet_mdev_vctrl_destroy(struct nvmet_mdev_vctrl *vctrl)
{
	mutex_unlock(&vctrl->lock);

	mutex_lock(&vctrl->lock); /* only for lockdep checks */
	nvmet_mdev_io_free(vctrl);
	__nvmet_mdev_vctrl_reset(vctrl, true);

	nvmet_mdev_pci_free(vctrl);
	nvmet_mdev_mmio_free(vctrl);

	mutex_unlock(&vctrl->lock);

	_INFO(vctrl, "device is destroyed\n");
}

/* Called when new mediated device is first opened by a user */
int nvmet_mdev_vctrl_open(struct vfio_device *vfio_dev)
{
	struct nvmet_mdev_vctrl *vctrl = vfio_dev_to_nvmet_mdev_vctrl(vfio_dev);

	_INFO(vctrl, "device is opened\n");

	mutex_lock(&vctrl->lock);
	nvmet_mdev_viommu_init(&vctrl->viommu, vfio_dev);
	nvmet_mdev_mmio_open(vctrl);
	mutex_unlock(&vctrl->lock);
	return 0;
}

/* Called when new mediated device is closed (last close of the user) */
void nvmet_mdev_vctrl_release(struct vfio_device *vfio_dev)
{
	struct nvmet_mdev_vctrl *vctrl = vfio_dev_to_nvmet_mdev_vctrl(vfio_dev);

	mutex_lock(&vctrl->lock);
	nvmet_mdev_io_pause(vctrl);
	/*
	 * Remove the guest DMA mappings - new user that will open the
	 * device might be a different guest
	 */
	nvmet_mdev_viommu_reset(&vctrl->viommu);

	/* Reset the controller to a clean state for a new user */
	__nvmet_mdev_vctrl_reset(vctrl, false);
	nvmet_mdev_irqs_reset(vctrl);
	mutex_unlock(&vctrl->lock);

	_INFO(vctrl, "device is released\n");
}

/* Called each time the controller is reset (CC.EN <= 0 or VM level reset) */
void __nvmet_mdev_vctrl_reset(struct nvmet_mdev_vctrl *vctrl, bool pci_reset)
{
	lockdep_assert_held(&vctrl->lock);

	if ((vctrl->mmio.csts & NVME_CSTS_RDY) &&
	    !(vctrl->mmio.csts & NVME_CSTS_SHST_MASK)) {
		_DBG(vctrl, "unsafe reset (CSTS.RDY==1)\n");
		nvmet_mdev_io_pause(vctrl);
		nvmet_mdev_vctrl_disable(vctrl);
	}
	nvmet_mdev_mmio_reset(vctrl, pci_reset);
}

/* setups initial admin queues and doorbells */
bool nvmet_mdev_vctrl_enable(struct nvmet_mdev_vctrl *vctrl, dma_addr_t cqiova,
			     dma_addr_t sqiova, u32 sizes)
{
	struct nvmet_ctrl *ctrl = vctrl->nvmet_ctrl;
	u16 cqentries, sqentries;
	int ret;

	nvmet_mdev_assert_io_not_running(vctrl);

	lockdep_assert_held(&vctrl->lock);

	sqentries = (sizes & 0xFFFF) + 1;
	cqentries = (sizes >> 16) + 1;

	if (cqentries > 4096 || cqentries < 2)
		return false;
	if (sqentries > 4096 || sqentries < 2)
		return false;

	ret = nvmet_mdev_mmio_enable_dbs(vctrl);
	if (ret)
		return false;

	ret = nvmet_mdev_vcq_init(vctrl, 0, cqiova, cqentries, 0);
	if (ret)
		goto disable_dbs;

	ret = nvmet_mdev_vsq_init(vctrl, 0, sqiova, sqentries, 0);
	if (ret)
		goto delete_vcq;

	nvmet_update_cc(ctrl, vctrl->mmio.cc);
	if (ctrl->csts != NVME_CSTS_RDY)
		goto delete_vsq;

	if (!vctrl->mmio.shadow_db_supported) {
		/* start polling right away to support admin queue */
		vctrl->io_idle = false;
		nvmet_mdev_io_resume(vctrl);
	}

	return true;

delete_vsq:
	nvmet_mdev_vsq_delete(vctrl, 0);
delete_vcq:
	nvmet_mdev_vcq_delete(vctrl, 0);
disable_dbs:
	nvmet_mdev_mmio_disable_dbs(vctrl);
	return false;
}

/* destroy all io/admin queues on the controller */
void nvmet_mdev_vctrl_disable(struct nvmet_mdev_vctrl *vctrl)
{
	u16 sqid, cqid;

	nvmet_mdev_assert_io_not_running(vctrl);

	lockdep_assert_held(&vctrl->lock);

	sqid = 1;
	for_each_set_bit_from(sqid, vctrl->vsq_en, NVMET_MDEV_MAX_NR_QUEUES)
		nvmet_mdev_vsq_delete(vctrl, sqid);

	cqid = 1;
	for_each_set_bit_from(cqid, vctrl->vcq_en, NVMET_MDEV_MAX_NR_QUEUES)
		nvmet_mdev_vcq_delete(vctrl, cqid);

	nvmet_mdev_vsq_delete(vctrl, 0);
	nvmet_mdev_vcq_delete(vctrl, 0);

	nvmet_mdev_mmio_disable_dbs(vctrl);
	vctrl->io_idle = true;
}

/* External reset */
void nvmet_mdev_vctrl_reset(struct nvmet_mdev_vctrl *vctrl)
{
	mutex_lock(&vctrl->lock);
	_INFO(vctrl, "reset\n");
	__nvmet_mdev_vctrl_reset(vctrl, true);
	mutex_unlock(&vctrl->lock);
}

/* Add IO region */
void nvmet_mdev_vctrl_add_region(struct nvmet_mdev_vctrl *vctrl,
				 unsigned int index, unsigned int size,
				 region_access_fn access_fn)
{
	struct nvmet_mdev_io_region *region = &vctrl->regions[index];

	region->size = size;
	region->rw = access_fn;
	region->mmap_ops = NULL;
}

/* Enable mmap window on an IO region */
void nvmet_mdev_vctrl_region_set_mmap(struct nvmet_mdev_vctrl *vctrl,
				      unsigned int index,
				      unsigned int offset,
				      unsigned int size,
				      const struct vm_operations_struct *ops)
{
	struct nvmet_mdev_io_region *region = &vctrl->regions[index];

	region->mmap_area_start = offset;
	region->mmap_area_size = size;
	region->mmap_ops = ops;
}

/* Disable mmap window on an IO region */
void nvmet_mdev_vctrl_region_disable_mmap(struct nvmet_mdev_vctrl *vctrl,
					  unsigned int index)
{
	struct nvmet_mdev_io_region *region = &vctrl->regions[index];

	region->mmap_area_start = 0;
	region->mmap_area_size = 0;
	region->mmap_ops = NULL;
}

/* remove a user memory mapping */
int nvmet_mdev_vctrl_viommu_unmap(struct nvmet_mdev_vctrl *vctrl,
				  dma_addr_t iova, u64 size)
{
	bool paused;
	int ret;

	paused = nvmet_mdev_io_pause(vctrl);
	ret = nvmet_mdev_viommu_remove(&vctrl->viommu, iova, size);
	if (paused)
		nvmet_mdev_io_resume(vctrl);
	return ret;
}
