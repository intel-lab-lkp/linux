// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 */

#include <drm/amdxdna_accel.h>
#include <drm/drm_device.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_print.h>
#include <drm/gpu_scheduler.h>
#include <linux/iommu.h>
#include <linux/types.h>

#include "aie.h"
#include "aie4_host_queue.h"
#include "aie4_msg_priv.h"
#include "aie4_pci.h"
#include "amdxdna_ctx.h"
#include "amdxdna_gem.h"
#include "amdxdna_mailbox.h"
#include "amdxdna_mailbox_helper.h"
#include "amdxdna_pci_drv.h"

#define CTX_INVALID_ID			(~0U)
#define CTX_INVALID_DOORBELL		AMDXDNA_INVALID_DOORBELL_OFFSET

static void job_worker(struct work_struct *work)
{
}

static struct cert_comp *aie4_lookup_cert_comp(struct amdxdna_dev_hdl *ndev, u32 msix_idx)
{
	struct amdxdna_dev *xdna = ndev->aie.xdna;
	struct cert_comp *cert_comp;
	int ret;

	mutex_lock(&ndev->cert_comp_lock);
	cert_comp = xa_load(&ndev->cert_comp_xa, msix_idx);
	if (cert_comp) {
		kref_get(&cert_comp->kref);
		mutex_unlock(&ndev->cert_comp_lock);
		return cert_comp;
	}

	cert_comp = kzalloc_obj(*cert_comp);
	if (!cert_comp) {
		ret = -ENOMEM;
		goto unlock;
	}

	cert_comp->ndev = ndev;
	cert_comp->msix_idx = msix_idx;
	cert_comp->irq = -ENOENT;
	init_waitqueue_head(&cert_comp->waitq);
	kref_init(&cert_comp->kref);

	/* Transport-specific: PCI wires an MSI-X irq, platform an IPI callback. */
	ret = aie4_request_notification(cert_comp);
	if (ret) {
		XDNA_ERR(xdna, "request notification for msix idx %u failed %d", msix_idx, ret);
		goto free_cert_comp;
	}

	ret = xa_err(xa_store(&ndev->cert_comp_xa, msix_idx, cert_comp, GFP_KERNEL));
	if (ret) {
		XDNA_ERR(xdna, "store cert_comp for msix idx %u failed %d", msix_idx, ret);
		goto free_irq;
	}
	mutex_unlock(&ndev->cert_comp_lock);

	return cert_comp;

free_irq:
	aie4_free_notification(cert_comp);
free_cert_comp:
	kfree(cert_comp);
unlock:
	mutex_unlock(&ndev->cert_comp_lock);
	return ERR_PTR(ret);
}

static void cert_comp_release(struct kref *kref)
{
	struct cert_comp *cert_comp = container_of(kref, struct cert_comp, kref);
	struct amdxdna_dev_hdl *ndev = cert_comp->ndev;

	xa_erase(&ndev->cert_comp_xa, cert_comp->msix_idx);
	aie4_free_notification(cert_comp);
	kfree(cert_comp);
}

static void aie4_put_cert_comp(struct cert_comp *cert_comp)
{
	struct amdxdna_dev_hdl *ndev = cert_comp->ndev;

	guard(mutex)(&ndev->cert_comp_lock);

	kref_put(&cert_comp->kref, cert_comp_release);
}

static struct cert_comp *aie4_get_cert_comp(struct amdxdna_hwctx *hwctx)
{
	struct amdxdna_hwctx_priv *priv = hwctx->priv;
	struct cert_comp *cert_comp;

	/* Take io_lock to serialize against cert_comp link/unlink. */
	guard(mutex)(&priv->io_lock);

	cert_comp = READ_ONCE(priv->cert_comp);
	if (cert_comp)
		kref_get(&cert_comp->kref);

	return cert_comp;
}

static void aie4_msg_destroy_context(struct amdxdna_dev_hdl *ndev, u32 hw_context_id)
{
	DECLARE_AIE_MSG(aie4_msg_destroy_hw_context, AIE4_MSG_OP_DESTROY_HW_CONTEXT);
	struct amdxdna_dev *xdna = ndev->aie.xdna;
	int ret;

	if (hw_context_id == CTX_INVALID_ID)
		return;

	req.hw_context_id = hw_context_id;
	ret = aie_send_mgmt_msg_wait(&ndev->aie, &msg);
	if (ret)
		XDNA_WARN(xdna, "destroy ctx id %d failed %d", hw_context_id, ret);
}

static u8 aie4_parse_priority_to_dev(u32 priority)
{
	switch (priority) {
	case AMDXDNA_QOS_LOW_PRIORITY:
		return AIE4_CONTEXT_PRIORITY_BAND_IDLE;
	case AMDXDNA_QOS_NORMAL_PRIORITY:
		return AIE4_CONTEXT_PRIORITY_BAND_NORMAL;
	case AMDXDNA_QOS_HIGH_PRIORITY:
		return AIE4_CONTEXT_PRIORITY_BAND_FOCUS;
	case AMDXDNA_QOS_REALTIME_PRIORITY:
		return AIE4_CONTEXT_PRIORITY_BAND_REAL_TIME;
	default:
		return AIE4_CONTEXT_PRIORITY_BAND_NORMAL;
	}
}

int aie4_hwctx_create(struct amdxdna_hwctx *hwctx)
{
	DECLARE_AIE_MSG(aie4_msg_create_hw_context, AIE4_MSG_OP_CREATE_HW_CONTEXT);
	struct amdxdna_client *client = hwctx->client;
	struct amdxdna_hwctx_priv *priv = hwctx->priv;
	struct amdxdna_dev *xdna = client->xdna;
	struct amdxdna_dev_hdl *ndev = xdna->dev_handle;
	struct cert_comp *cert_comp;
	int ret;

	drm_WARN_ON(&xdna->ddev, !mutex_is_locked(&xdna->dev_lock));

	if (!ndev->partition_id || !hwctx->num_tiles) {
		XDNA_ERR(xdna, "invalid request partition_id %u, num_tiles %d",
			 ndev->partition_id, hwctx->num_tiles);
		return -EINVAL;
	}

	req.partition_id = ndev->partition_id;
	req.request_num_tiles = hwctx->num_tiles;
	req.pasid = aie4_msg_pasid(client);
	req.pasid = req.pasid == IOMMU_PASID_INVALID ? 0 : req.pasid;
	req.priority_band = aie4_parse_priority_to_dev(hwctx->qos.priority);
	req.hsa_addr_high = upper_32_bits(amdxdna_gem_dev_addr(priv->umq_bo));
	req.hsa_addr_low = lower_32_bits(amdxdna_gem_dev_addr(priv->umq_bo));

	XDNA_DBG(xdna, "pasid 0x%x, num_tiles %d, hsa[0x%x 0x%x]",
		 req.pasid, req.request_num_tiles, req.hsa_addr_high, req.hsa_addr_low);

	ret = aie_send_mgmt_msg_wait(&ndev->aie, &msg);
	if (ret) {
		XDNA_ERR(xdna, "create ctx failed: %d", ret);
		return ret;
	}

	XDNA_DBG(xdna, "resp msix: %d, ctx id: %d, doorbell: %d",
		 resp.job_complete_msix_idx, resp.hw_context_id,
		 resp.doorbell_offset);

	/* setup interrupt completion per msix index */
	cert_comp = aie4_lookup_cert_comp(ndev, resp.job_complete_msix_idx);
	if (IS_ERR(cert_comp)) {
		aie4_msg_destroy_context(ndev, resp.hw_context_id);
		return PTR_ERR(cert_comp);
	}

	priv->hw_ctx_id = resp.hw_context_id;

	hwctx->fw_ctx_id = resp.hw_context_id;
	hwctx->start_col = 0;
	hwctx->num_col = ndev->total_col;

	/* Set up driver doorbell and return invalid offset to userspace. */
	mutex_lock(&priv->io_lock);
	ret = aie4_doorbell_setup(hwctx, &resp);
	if (ret) {
		mutex_unlock(&priv->io_lock);
		aie4_msg_destroy_context(ndev, resp.hw_context_id);
		aie4_put_cert_comp(cert_comp);
		priv->hw_ctx_id = CTX_INVALID_ID;
		hwctx->fw_ctx_id = -1;
		return ret;
	}
	WRITE_ONCE(priv->cert_comp, cert_comp);
	mutex_unlock(&priv->io_lock);
	hwctx->doorbell_offset = CTX_INVALID_DOORBELL;
	wake_up_all(&priv->job_list_wq);

	return 0;
}

void aie4_hwctx_destroy(struct amdxdna_hwctx *hwctx, enum aie4_hwctx_flags flags)
{
	struct amdxdna_client *client = hwctx->client;
	struct amdxdna_hwctx_priv *priv = hwctx->priv;
	struct amdxdna_dev *xdna = client->xdna;
	struct amdxdna_dev_hdl *ndev = xdna->dev_handle;
	struct cert_comp *cert_comp;

	drm_WARN_ON(&xdna->ddev, !mutex_is_locked(&xdna->dev_lock));

	mutex_lock(&priv->io_lock);
	cert_comp = priv->cert_comp;
	WRITE_ONCE(priv->cert_comp, NULL);
	mutex_unlock(&priv->io_lock);

	if (cert_comp)
		wake_up_all(&cert_comp->waitq);

	if (flags != AIE4_HWCTX_DISCONNECT)
		aie4_msg_destroy_context(ndev, priv->hw_ctx_id);

	if (cert_comp)
		aie4_put_cert_comp(cert_comp);

	priv->hw_ctx_id = CTX_INVALID_ID;
	hwctx->fw_ctx_id = -1;
	hwctx->doorbell_offset = CTX_INVALID_DOORBELL;

	cancel_work_sync(&priv->job_work);
}

static void aie4_hwctx_umq_fini(struct amdxdna_hwctx *hwctx)
{
	if (hwctx->priv && hwctx->priv->umq_bo)
		drm_gem_object_put(to_gobj(hwctx->priv->umq_bo));
}

static int aie4_hwctx_umq_init(struct amdxdna_hwctx *hwctx)
{
	const size_t indir_pkts_sz = CTX_MAX_CMDS * HSA_MAX_LEVEL1_INDIRECT_ENTRIES *
				     sizeof(struct host_indirect_packet_data);
	const size_t pkts_sz = CTX_MAX_CMDS * sizeof(struct host_queue_packet);
	struct amdxdna_hwctx_priv *priv = hwctx->priv;
	struct amdxdna_dev *xdna = hwctx->client->xdna;
	struct amdxdna_gem_obj *umq_bo;
	struct host_queue_header *qhdr;
	u64 data_dev_addr;
	void *umq_va;
	int ret;
	int i;

	/* Queue lives in user-allocated BO so device can access it under PASID. */
	umq_bo = amdxdna_gem_get_obj(hwctx->client, hwctx->umq_bo_hdl, AMDXDNA_BO_SHARE);
	if (!umq_bo) {
		XDNA_ERR(xdna, "cannot find umq_bo handle %d", hwctx->umq_bo_hdl);
		return -ENOENT;
	}

	/* Ensure user BO can hold queue header and direct/indirect packet arrays. */
	if (umq_bo->mem.size < sizeof(*qhdr) ||
	    (umq_bo->mem.size < sizeof(*qhdr) + pkts_sz + indir_pkts_sz)) {
		XDNA_ERR(xdna, "umq_bo size %zu is too small",
			 (size_t)umq_bo->mem.size);
		ret = -EINVAL;
		goto put_umq_bo;
	}

	umq_va = amdxdna_gem_vmap(umq_bo);
	if (!umq_va) {
		ret = -ENOMEM;
		goto put_umq_bo;
	}
	qhdr = umq_va;

	priv->umq_bo = umq_bo;
	priv->umq_read_index = &qhdr->read_index;
	priv->umq_write_index = &qhdr->write_index;

	/* Lay out direct packets after header, followed by indirect packets. */
	data_dev_addr = amdxdna_gem_dev_addr(umq_bo) + sizeof(*qhdr);
	priv->umq_pkts = umq_va + sizeof(*qhdr);
	priv->umq_indirect_pkts = umq_va + sizeof(*qhdr) + pkts_sz;
	priv->umq_indirect_pkts_dev_addr = data_dev_addr + pkts_sz;

	/* Clear only the driver-managed header and packet regions. */
	memset(umq_va, 0, sizeof(*qhdr) + pkts_sz + indir_pkts_sz);
	priv->write_index = QUEUE_INDEX_START;
	qhdr->read_index = QUEUE_INDEX_START;
	qhdr->write_index = QUEUE_INDEX_START;
	qhdr->version.major = HOST_QUEUE_MAJOR_VERSION;
	qhdr->version.minor = HOST_QUEUE_MINOR_VERSION;
	qhdr->capacity = CTX_MAX_CMDS;
	qhdr->data_address = data_dev_addr;
	for (i = 0; i < CTX_MAX_CMDS; i++)
		priv->umq_pkts[i].pkt_header.common_header.opcode = OPCODE_EXEC_BUF;
	for (i = 0; i < CTX_MAX_CMDS * HSA_MAX_LEVEL1_INDIRECT_ENTRIES; i++) {
		priv->umq_indirect_pkts[i].header.opcode = OPCODE_EXEC_BUF;
		priv->umq_indirect_pkts[i].header.count = sizeof(struct exec_buf);
		priv->umq_indirect_pkts[i].header.distribute = 1;
	}

	return 0;

put_umq_bo:
	amdxdna_gem_put_obj(umq_bo);
	return ret;
}

int aie4_hwctx_init(struct amdxdna_hwctx *hwctx)
{
	struct amdxdna_client *client = hwctx->client;
	struct amdxdna_dev *xdna = client->xdna;
	struct amdxdna_dev_hdl *ndev = xdna->dev_handle;
	struct amdxdna_hwctx_priv *priv;
	int ret;

	if (!AIE_FEATURE_ON(&ndev->aie, AIE4_HSA_COMMAND))
		return -EOPNOTSUPP;

	priv = kzalloc_obj(*priv);
	if (!priv)
		return -ENOMEM;
	hwctx->priv = priv;
	priv->hwctx = hwctx;

	/* Initialize io_lock guarding cert_comp binding. */
	mutex_init(&priv->io_lock);

	INIT_LIST_HEAD(&priv->pending_job_list);
	INIT_LIST_HEAD(&priv->running_job_list);
	init_waitqueue_head(&priv->job_list_wq);
	INIT_WORK(&priv->job_work, job_worker);

	ret = aie4_hwctx_umq_init(hwctx);
	if (ret)
		goto destroy_lock;

	ret = aie4_hwctx_create(hwctx);
	if (ret)
		goto umq_fini;

	priv->job_work_q = alloc_ordered_workqueue("aie4_job_%d_%d", 0,
						   client->pid, hwctx->fw_ctx_id);
	if (!priv->job_work_q) {
		XDNA_ERR(xdna, "Create job_work_q failed");
		ret = -ENOMEM;
		goto destroy_ctx;
	}

	XDNA_DBG(xdna, "hwctx %d.%d init completed", client->pid, hwctx->fw_ctx_id);
	return 0;

destroy_ctx:
	aie4_hwctx_destroy(hwctx, AIE4_HWCTX_NORMAL);
umq_fini:
	aie4_hwctx_umq_fini(hwctx);
destroy_lock:
	mutex_destroy(&priv->io_lock);
	kfree(priv);
	hwctx->priv = NULL;
	return ret;
}

void aie4_hwctx_fini(struct amdxdna_hwctx *hwctx)
{
	struct amdxdna_hwctx_priv *priv = hwctx->priv;

	aie4_hwctx_destroy(hwctx, AIE4_HWCTX_ERROR);
	cancel_work_sync(&priv->job_work);
	if (priv->job_work_q)
		destroy_workqueue(priv->job_work_q);
	aie4_hwctx_umq_fini(hwctx);
	mutex_destroy(&priv->io_lock);
	kfree(hwctx->priv);
}

static inline bool valid_queue_index(u64 read, u64 write, u32 capacity)
{
	return (write >= read) && ((write - read) <= capacity);
}

static u64 get_read_index(struct amdxdna_hwctx *hwctx)
{
	u64 wi = READ_ONCE(*hwctx->priv->umq_write_index);
	u64 ri = READ_ONCE(*hwctx->priv->umq_read_index);
	struct amdxdna_dev *xdna = hwctx->client->xdna;

	/*
	 * CERT cannot update read index as uint64 atomically. Driver may read
	 * half-updated read index when it has bits in high 32bit. In case read
	 * index is not valid, wait for some time and retry once. It should
	 * allow CERT to complete the read index update.
	 */
	if (!valid_queue_index(ri, wi, CTX_MAX_CMDS)) {
		XDNA_WARN(xdna, "Invalid index, ri %llu, wi %llu", ri, wi);
		usleep_range(100, 200);
		ri = READ_ONCE(*hwctx->priv->umq_read_index);
		if (!valid_queue_index(ri, wi, CTX_MAX_CMDS)) {
			XDNA_ERR(xdna, "Invalid index after retry, ri %llu, wi %llu", ri, wi);
			ri = 0;
		}
	}

	return ri;
}

static inline bool check_cmd_done(struct amdxdna_hwctx *hwctx, u64 seq)
{
	u64 read_idx = get_read_index(hwctx);

	return read_idx > seq;
}

int aie4_cmd_wait(struct amdxdna_hwctx *hwctx, u64 seq, u32 timeout)
{
	unsigned long wait_jifs = MAX_SCHEDULE_TIMEOUT;
	struct cert_comp *cert_comp = aie4_get_cert_comp(hwctx);
	long ret;

	if (!cert_comp)
		return -EAGAIN;

	if (timeout)
		wait_jifs = msecs_to_jiffies(timeout);

	ret = wait_event_interruptible_timeout(cert_comp->waitq,
					       (check_cmd_done(hwctx, seq)),
					       wait_jifs);

	if (!ret)
		ret = -ETIME;

	aie4_put_cert_comp(cert_comp);

	return ret <= 0 ? ret : 0;
}
