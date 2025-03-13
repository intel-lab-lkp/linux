// SPDX-License-Identifier: GPL-2.0+
/*
 * NVMe virtual controller MMIO implementation
 * Copyright (c) 2019 - Maxim Levitsky
 */
#include <linux/kernel.h>
#include <linux/highmem.h>
#include "priv.h"

#define DB_AREA_SIZE (NVMET_MDEV_MAX_NR_QUEUES * 2 * (4 << DB_STRIDE_SHIFT))
#define DB_MASK ((4 << DB_STRIDE_SHIFT) - 1)
#define MMIO_BAR_SIZE __roundup_pow_of_two(NVME_REG_DBS + DB_AREA_SIZE)

/* Put the controller into fatal error state. Only way out is reset */
static void nvmet_mdev_mmio_fatal_error(struct nvmet_mdev_vctrl *vctrl)
{
	if (vctrl->mmio.csts & NVME_CSTS_CFS)
		return;

	vctrl->mmio.csts |= NVME_CSTS_CFS;
	nvmet_mdev_io_pause(vctrl);

	if (vctrl->mmio.csts & NVME_CSTS_RDY)
		nvmet_mdev_vctrl_disable(vctrl);
}

/* This is memory fault handler for the mmap area of the doorbells */
static vm_fault_t nvmet_mdev_mmio_dbs_mmap_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	struct nvmet_mdev_vctrl *vctrl = vma->vm_private_data;

	/* DB area is just one page, starting at offset 4096 of the mmio */
	if (WARN_ON(vmf->pgoff != 1))
		return VM_FAULT_SIGBUS;

	get_page(vctrl->mmio.dbs_page);
	vmf->page = vctrl->mmio.dbs_page;
	return 0;
}

static const struct vm_operations_struct nvmet_mdev_mmio_dbs_vm_ops = {
	.fault = nvmet_mdev_mmio_dbs_mmap_fault,
};

/* check that user db write is valid and send an error if not */
bool nvmet_mdev_mmio_db_check(struct nvmet_mdev_vctrl *vctrl, u16 qid, u16 size,
			      u16 db)
{
	if (db < size)
		return true;
	if (qid == 0) {
		_DBG(vctrl, "MMIO: invalid admin DB write - fatal error\n");
		nvmet_mdev_mmio_fatal_error(vctrl);
		return false;
	}

	_DBG(vctrl, "MMIO: invalid DB value write qid=%d, size=%d, value=%d\n",
	     qid, size, db);

	nvmet_add_async_event(vctrl->nvmet_ctrl, NVME_AER_ERROR,
			      NVME_AER_ERROR_INVALID_DB_VALUE, NVME_LOG_ERROR);
	return false;
}

/* handle submission queue doorbell write */
static void nvmet_mdev_mmio_db_write_sq(struct nvmet_mdev_vctrl *vctrl, u32 qid,
					u32 val)
{
	_DBG(vctrl, "MMIO: doorbell SQID %d, DB write %d\n", qid, val);

	/* check if the db belongs to a valid queue */
	if (qid >= NVMET_MDEV_MAX_NR_QUEUES || !test_bit(qid, vctrl->vsq_en))
		goto err_db;

	vctrl->io_idle = false;

	if (!vctrl->mmio.shadow_db_supported)
		return;

	nvmet_mdev_io_resume(vctrl);
	return;

err_db:
	_DBG(vctrl, "MMIO: inactive/invalid SQ DB write qid=%d, value=%d\n",
	     qid, val);

	nvmet_add_async_event(vctrl->nvmet_ctrl, NVME_AER_ERROR,
			      NVME_AER_ERROR_INVALID_DB_REG, NVME_LOG_ERROR);
}

/* handle doorbell write */
static void nvmet_mdev_mmio_db_write_cq(struct nvmet_mdev_vctrl *vctrl, u32 qid,
					u32 val)
{
	_DBG(vctrl, "MMIO: doorbell CQID %d, DB write %d\n", qid, val);

	lockdep_assert_held(&vctrl->lock);
	/* check if the db belongs to a valid queue */
	if (qid >= NVMET_MDEV_MAX_NR_QUEUES || !test_bit(qid, vctrl->vcq_en))
		goto err_db;

	if (!vctrl->mmio.shadow_db_supported)
		return;

	nvmet_mdev_io_resume(vctrl);
	return;

err_db:
	_DBG(vctrl,
	     "MMIO: inactive/invalid CQ DB write qid=%d, value=%d\n",
	     qid, val);

	nvmet_add_async_event(vctrl->nvmet_ctrl, NVME_AER_ERROR,
			      NVME_AER_ERROR_INVALID_DB_REG, NVME_LOG_ERROR);
}

/* This is called when user enables the controller */
static void nvmet_mdev_mmio_cntrl_enable(struct nvmet_mdev_vctrl *vctrl)
{
	u64 acq, asq;

	lockdep_assert_held(&vctrl->lock);

	/* Controller must be reset from the dead state */
	if (nvmet_mdev_vctrl_is_dead(vctrl))
		goto error;

	if (nvmet_cc_ams(vctrl->mmio.cc) != NVME_CC_AMS_RR)
		goto error;

	/* Check the page size */
	if (nvmet_cc_mps(vctrl->mmio.cc) != (PAGE_SHIFT - 12))
		goto error;

	/* Start the admin completion queue */
	acq = vctrl->mmio.acql | ((u64)vctrl->mmio.acqh << 32);
	asq = vctrl->mmio.asql | ((u64)vctrl->mmio.asqh << 32);

	if (!nvmet_mdev_vctrl_enable(vctrl, acq, asq, vctrl->mmio.aqa))
		goto error;

	/* Success! */
	vctrl->mmio.csts |= NVME_CSTS_RDY;
	return;
error:
	_WARN(vctrl, "MMIO: failure to enable the controller - fatal error\n");
	nvmet_mdev_mmio_fatal_error(vctrl);
}

/*
 * This is called when user sends a notification that controller is
 * about to be disabled
 */
static void nvmet_mdev_mmio_cntrl_shutdown(struct nvmet_mdev_vctrl *vctrl)
{
	lockdep_assert_held(&vctrl->lock);

	nvmet_update_cc(vctrl->nvmet_ctrl, vctrl->mmio.cc);
	/*
	 * Manually clear shutdown notification bits
	 * TODO: inherit csts/cc from nvmet core.
	 */
	vctrl->mmio.cc &= ~NVME_CC_SHN_MASK;

	if (nvmet_mdev_vctrl_is_dead(vctrl)) {
		_DBG(vctrl, "MMIO: shutdown notification for dead ctrl\n");
		return;
	}

	/* not enabled */
	if (!(vctrl->mmio.csts & NVME_CSTS_RDY)) {
		_DBG(vctrl, "MMIO: shutdown notification with CSTS.RDY==0\n");
		nvmet_mdev_assert_io_not_running(vctrl);
		return;
	}

	nvmet_mdev_io_pause(vctrl);
	nvmet_mdev_vctrl_disable(vctrl);
	vctrl->mmio.csts |= NVME_CSTS_SHST_CMPLT;
}

/* MMIO BAR read/write */
static int nvmet_mdev_mmio_bar_access(struct nvmet_mdev_vctrl *vctrl,
				      u16 offset, char *buf, u32 count,
				      bool is_write)
{
	u32 val, oldval;

	mutex_lock(&vctrl->lock);

	/*
	 * Drop non DWORD sized and aligned reads/writes
	 * (QWORD  read/writes are split by the caller)
	 */
	if (count != 4 || (offset & 0x3))
		goto drop;

	val = is_write ? le32_to_cpu(*(__le32 *)buf) : 0;

	switch (offset) {
	case NVME_REG_CAP:
		/* controller capabilities (low 32 bit) */
		if (is_write)
			goto drop;
		store_le32(buf, vctrl->mmio.cap & 0xFFFFFFFF);
		break;
	case NVME_REG_CAP + 4:
		/* controller capabilities (upper 32 bit) */
		if (is_write)
			goto drop;
		store_le32(buf, vctrl->mmio.cap >> 32);
		break;
	case NVME_REG_VS:
		if (is_write)
			goto drop;
		store_le32(buf, NVME_MDEV_NVME_VER);
		break;
	case NVME_REG_INTMS:
	case NVME_REG_INTMC:
		/* Interrupt Mask Set & Clear */
		goto drop;
	case NVME_REG_CC:
		/* Controller Configuration */
		if (!is_write) {
			store_le32(buf, vctrl->mmio.cc);
			break;
		}

		oldval = vctrl->mmio.cc;
		vctrl->mmio.cc = val;

		/* drop if reserved bits set */
		if (vctrl->mmio.cc & 0xFF00000E) {
			_DBG(vctrl,
			     "MMIO: reserved bits of CC set - fatal error\n");
			nvmet_mdev_mmio_fatal_error(vctrl);
			goto drop;
		}

		/*
		 * CSS(command set),MPS(memory page size),AMS(queue arbitration)
		 * must not be changed while controller is running
		 */
		if (vctrl->mmio.csts & NVME_CSTS_RDY) {
			if ((vctrl->mmio.cc & 0x3FF0) != (oldval & 0x3FF0)) {
				_DBG(vctrl,
				     "MMIO: attempt to change setting bits of CC while CC.EN=1 - fatal error\n");

				nvmet_mdev_mmio_fatal_error(vctrl);
				goto drop;
			}
		}

		if (nvmet_cc_shn(vctrl->mmio.cc)) {
			_DBG(vctrl, "MMIO: CC.SHN != 0 - shutdown\n");
			nvmet_mdev_mmio_cntrl_shutdown(vctrl);
		}

		/* change in controller enabled state */
		if (nvmet_cc_en(val) == nvmet_cc_en(oldval))
			break;

		if (nvmet_cc_en(vctrl->mmio.cc)) {
			_DBG(vctrl, "MMIO: CC.EN<=1 - enable the controller\n");
			nvmet_mdev_mmio_cntrl_enable(vctrl);
		} else {
			_DBG(vctrl, "MMIO: CC.EN<=0 - reset controller\n");
			__nvmet_mdev_vctrl_reset(vctrl, false);
		}

		break;
	case NVME_REG_CSTS:
		/* Controller Status */
		if (is_write)
			goto drop;
		store_le32(buf, vctrl->mmio.csts);
		break;
	case NVME_REG_AQA:
		/* admin queue submission and completion size */
		if (!is_write)
			store_le32(buf, vctrl->mmio.aqa);
		else if (!(vctrl->mmio.csts & NVME_CSTS_RDY))
			vctrl->mmio.aqa = val;
		else
			goto drop;
		break;
	case NVME_REG_ASQ:
		/* admin submission queue address (low 32 bit) */
		if (!is_write)
			store_le32(buf, vctrl->mmio.asql);
		else if (!(vctrl->mmio.csts & NVME_CSTS_RDY))
			vctrl->mmio.asql = val;
		else
			goto drop;
		break;

	case NVME_REG_ASQ + 4:
		/* admin submission queue address (high 32 bit) */
		if (!is_write)
			store_le32(buf, vctrl->mmio.asqh);
		else if (!(vctrl->mmio.csts & NVME_CSTS_RDY))
			vctrl->mmio.asqh = val;
		else
			goto drop;
		break;
	case NVME_REG_ACQ:
		/* admin completion queue address (low 32 bit) */
		if (!is_write)
			store_le32(buf, vctrl->mmio.acql);
		else if (!(vctrl->mmio.csts & NVME_CSTS_RDY))
			vctrl->mmio.acql = val;
		else
			goto drop;
		break;
	case NVME_REG_ACQ + 4:
		/* admin completion queue address (high 32 bit) */
		if (!is_write)
			store_le32(buf, vctrl->mmio.acqh);
		else if (!(vctrl->mmio.csts & NVME_CSTS_RDY))
			vctrl->mmio.acqh = val;
		else
			goto drop;
		break;
	case NVME_REG_CMBLOC:
	case NVME_REG_CMBSZ:
		/* not supported - hardwired to 0 */
		if (is_write)
			goto drop;
		store_le32(buf, 0);
		break;
	case NVME_REG_DBS ... (NVME_REG_DBS + DB_AREA_SIZE - 1): {
		/* completion and submission doorbells */
		u16 db_offset = offset - NVME_REG_DBS;
		u16 index = db_offset >> (DB_STRIDE_SHIFT + 2);
		u16 qid = index >> 1;
		bool sq = (index & 0x1) == 0;

		if (!is_write || (db_offset & DB_MASK))
			goto drop;

		if (!(vctrl->mmio.csts & NVME_CSTS_RDY))
			goto drop;

		if (nvmet_mdev_vctrl_is_dead(vctrl))
			goto drop;

		sq ? nvmet_mdev_mmio_db_write_sq(vctrl, qid, val) :
		     nvmet_mdev_mmio_db_write_cq(vctrl, qid, val);
		break;
	}
	default:
		goto drop;
	}

	mutex_unlock(&vctrl->lock);
	return count;

drop:
	_WARN(vctrl, "MMIO: dropping write at 0x%x\n", offset);
	mutex_unlock(&vctrl->lock);
	return 0;
}

/* Called when the virtual controller is created */
int nvmet_mdev_mmio_create(struct nvmet_mdev_vctrl *vctrl)
{
	struct nvmet_ctrl *ctrl = vctrl->nvmet_ctrl;
	int ret;

	/* BAR0 */
	nvmet_mdev_pci_setup_bar(vctrl, PCI_BASE_ADDRESS_0, MMIO_BAR_SIZE,
				 nvmet_mdev_mmio_bar_access);

	/* CAP has 4 bits for the doorbell stride shift */
	BUILD_BUG_ON(DB_STRIDE_SHIFT > 0xF);

	/* Shadow doorbell limits doorbells to 1 page */
	BUILD_BUG_ON(DB_AREA_SIZE > PAGE_SIZE);

	/* Just in case... */
	BUILD_BUG_ON((PAGE_SHIFT - 12) > 0xF);

	/*
	 * Inherit nvmet core's defaults and configfs values and then add
	 * specific settings.
	 */
	vctrl->mmio.cap = ctrl->cap;

	vctrl->mmio.cap |=
		/*
		 * CQR: physically contiguous queues until we fine a OS
		 * that needs otherwise.
		 */
		(1ULL << 16) |
		/* DSTRD: doorbell stride */
		((u64)DB_STRIDE_SHIFT << 32) |
		/* MPSMIN: Minimum page size supported is PAGE_SIZE */
		((u64)(PAGE_SHIFT - 12) << 48) |
		/* MPSMAX: Maximum page size is PAGE_SIZE as well */
		((u64)(PAGE_SHIFT - 12) << 52);

	/* Create the (regular) doorbell buffers */
	vctrl->mmio.dbs_page = alloc_pages(__GFP_ZERO, 0);

	ret = -ENOMEM;

	if (!vctrl->mmio.dbs_page)
		goto error0;

	vctrl->mmio.db_page_kmap = kmap_local_page(vctrl->mmio.dbs_page);
	if (!vctrl->mmio.db_page_kmap)
		goto error1;

	vctrl->mmio.fake_eidx_page = alloc_page(__GFP_ZERO);
	if (!vctrl->mmio.fake_eidx_page)
		goto error2;

	vctrl->mmio.fake_eidx_kmap = kmap_local_page(vctrl->mmio.fake_eidx_page);
	if (!vctrl->mmio.fake_eidx_kmap)
		goto error3;
	return 0;
error3:
	put_page(vctrl->mmio.fake_eidx_kmap);
error2:
	kunmap_local(vctrl->mmio.db_page_kmap);
error1:
	put_page(vctrl->mmio.dbs_page);
error0:
	return ret;
}

/* Called when the virtual controller is reset */
void nvmet_mdev_mmio_reset(struct nvmet_mdev_vctrl *vctrl, bool pci_reset)
{
	vctrl->mmio.cc = 0;
	vctrl->mmio.csts = 0;

	if (pci_reset) {
		vctrl->mmio.aqa  = 0;
		vctrl->mmio.asql = 0;
		vctrl->mmio.asqh = 0;
		vctrl->mmio.acql = 0;
		vctrl->mmio.acqh = 0;
	}
}

/* Called when the virtual controller is opened */
void nvmet_mdev_mmio_open(struct nvmet_mdev_vctrl *vctrl)
{
	if (!vctrl->mmio.shadow_db_supported)
		nvmet_mdev_vctrl_region_set_mmap(vctrl,
						 VFIO_PCI_BAR0_REGION_INDEX,
						 NVME_REG_DBS, PAGE_SIZE,
						 &nvmet_mdev_mmio_dbs_vm_ops);
	else
		nvmet_mdev_vctrl_region_disable_mmap(vctrl,
						     VFIO_PCI_BAR0_REGION_INDEX);
}

/* Called when the virtual controller queues are enabled */
int nvmet_mdev_mmio_enable_dbs(struct nvmet_mdev_vctrl *vctrl)
{
	if (WARN_ON(vctrl->mmio.shadow_db_en))
		return -EINVAL;

	nvmet_mdev_assert_io_not_running(vctrl);

	/* setup normal doorbells and reset them */
	vctrl->mmio.dbs = vctrl->mmio.db_page_kmap;
	vctrl->mmio.eidxs = vctrl->mmio.fake_eidx_kmap;
	memset((void *)vctrl->mmio.dbs, 0, DB_AREA_SIZE);
	memset((void *)vctrl->mmio.eidxs, 0, DB_AREA_SIZE);
	return 0;
}

/* Called when the virtual controller shadow doorbell is enabled */
int nvmet_mdev_mmio_enable_dbs_shadow(struct nvmet_mdev_vctrl *vctrl,
				      dma_addr_t sdb_iova,
				      dma_addr_t eidx_iova)
{
	int ret;

	ret = nvmet_mdev_viommu_add(&vctrl->viommu,
				    VFIO_DMA_MAP_FLAG_READ |
				    VFIO_DMA_MAP_FLAG_WRITE,
				    sdb_iova, PAGE_SIZE,
				    &vctrl->viommu.mem_map_list);
	if (ret)
		return ret;

	ret = nvmet_mdev_viommu_create_kmap(&vctrl->viommu, sdb_iova,
					    &vctrl->mmio.sdb_map);
	if (ret)
		goto unmap_sdb;

	ret = nvmet_mdev_viommu_add(&vctrl->viommu,
				    VFIO_DMA_MAP_FLAG_READ |
				    VFIO_DMA_MAP_FLAG_WRITE,
				    eidx_iova, PAGE_SIZE,
				    &vctrl->viommu.mem_map_list);
	if (ret)
		goto kunmap_sdb;

	ret = nvmet_mdev_viommu_create_kmap(&vctrl->viommu, eidx_iova,
					    &vctrl->mmio.seidx_map);
	if (ret)
		goto unmap_eidx;

	vctrl->mmio.sdb_iova = sdb_iova;
	vctrl->mmio.dbs = vctrl->mmio.sdb_map.kmap;

	vctrl->mmio.eidx_iova = eidx_iova;
	vctrl->mmio.eidxs = vctrl->mmio.seidx_map.kmap;

	memcpy((void *)vctrl->mmio.dbs, vctrl->mmio.db_page_kmap,
	       DB_AREA_SIZE);
	memcpy((void *)vctrl->mmio.eidxs, vctrl->mmio.db_page_kmap,
	       DB_AREA_SIZE);

	vctrl->mmio.shadow_db_en = true;
	return 0;

unmap_eidx:
	nvmet_mdev_vctrl_viommu_unmap(vctrl, eidx_iova, PAGE_SIZE);
kunmap_sdb:
	nvmet_mdev_viommu_free_kmap(&vctrl->viommu, &vctrl->mmio.sdb_map);
unmap_sdb:
	nvmet_mdev_vctrl_viommu_unmap(vctrl, sdb_iova, PAGE_SIZE);
	return ret;
}

/* Disable the doorbells */
void nvmet_mdev_mmio_disable_dbs(struct nvmet_mdev_vctrl *vctrl)
{
	nvmet_mdev_assert_io_not_running(vctrl);

	/* Free the shadow doorbells */
	nvmet_mdev_viommu_free_kmap(&vctrl->viommu, &vctrl->mmio.sdb_map);
	nvmet_mdev_viommu_free_kmap(&vctrl->viommu, &vctrl->mmio.seidx_map);

	nvmet_mdev_vctrl_viommu_unmap(vctrl, vctrl->mmio.sdb_iova, PAGE_SIZE);
	nvmet_mdev_vctrl_viommu_unmap(vctrl, vctrl->mmio.eidx_iova, PAGE_SIZE);

	/* Clear the doorbells */
	vctrl->mmio.sdb_iova = 0;
	vctrl->mmio.eidx_iova = 0;
	vctrl->mmio.dbs = NULL;
	vctrl->mmio.eidxs = NULL;
	vctrl->mmio.shadow_db_en = false;
}

/* Called when the virtual controller is about to be freed */
void nvmet_mdev_mmio_free(struct nvmet_mdev_vctrl *vctrl)
{
	nvmet_mdev_assert_io_not_running(vctrl);
	kunmap_local(vctrl->mmio.db_page_kmap);
	put_page(vctrl->mmio.dbs_page);
	kunmap_local(vctrl->mmio.fake_eidx_kmap);
	put_page(vctrl->mmio.fake_eidx_page);
}
