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

static void job_worker(struct work_struct *work);

static struct cert_comp *aie4_lookup_cert_comp(struct amdxdna_dev_hdl *ndev, u32 msix_idx)
{
	struct amdxdna_dev *xdna = ndev->aie.xdna;
	struct cert_comp *cert_comp;
	int ret;

	guard(mutex)(&ndev->cert_comp_lock);

	cert_comp = xa_load(&ndev->cert_comp_xa, msix_idx);
	if (cert_comp) {
		kref_get(&cert_comp->kref);
		return cert_comp;
	}

	cert_comp = kzalloc_obj(*cert_comp);
	if (!cert_comp)
		return ERR_PTR(-ENOMEM);

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

	return cert_comp;

free_irq:
	aie4_free_notification(cert_comp);
free_cert_comp:
	kfree(cert_comp);
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

	/*
	 * priv->cert_comp is the per-hwctx field, guarded by io_lock. A non-NULL
	 * value means this ctx still holds its link-ref, so the object is alive and
	 * the kref_get here cannot race the free.
	 */
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

	/*
	 * Kernel-mode submission: set up this context's doorbell kick target
	 * (transport-specific, via aie4_doorbell_setup) so the driver can ring
	 * it, and keep it out of user space (hand back an invalid offset so the
	 * doorbell cannot be mmap'd/rung by the user).
	 */
	mutex_lock(&priv->io_lock);
	ret = aie4_doorbell_setup(hwctx, &resp);
	if (ret) {
		mutex_unlock(&priv->io_lock);
		aie4_put_cert_comp(cert_comp);
		aie4_msg_destroy_context(ndev, resp.hw_context_id);
		priv->hw_ctx_id = CTX_INVALID_ID;
		hwctx->fw_ctx_id = -1;
		return ret;
	}
	WRITE_ONCE(priv->has_reset, false);
	WRITE_ONCE(priv->cert_comp, cert_comp);
	mutex_unlock(&priv->io_lock);
	hwctx->doorbell_offset = CTX_INVALID_DOORBELL;
	wake_up_all(&priv->job_list_wq);

	return 0;
}

/*
 * The connected sentinel for the submit wait gate: a linked cert_comp means the
 * ctx is created and (for kernel submission) its doorbell is set up. Read
 * locklessly (a pointer null-check, never a dereference); create publishes it as
 * the last store under io_lock and destroy clears it first, so "connected"
 * implies a valid doorbell.
 */
static bool aie4_hwctx_connected(struct amdxdna_hwctx *hwctx)
{
	return !!READ_ONCE(hwctx->priv->cert_comp);
}

static bool aie4_hwctx_has_reset(struct amdxdna_hwctx *hwctx)
{
	return READ_ONCE(hwctx->priv->has_reset);
}

void aie4_hwctx_destroy(struct amdxdna_hwctx *hwctx, enum aie4_hwctx_flags flags)
{
	struct amdxdna_client *client = hwctx->client;
	struct amdxdna_hwctx_priv *priv = hwctx->priv;
	struct amdxdna_dev *xdna = client->xdna;
	struct amdxdna_dev_hdl *ndev = xdna->dev_handle;
	struct cert_comp *cert_comp;
	bool has_reset = false;

	drm_WARN_ON(&xdna->ddev, !mutex_is_locked(&xdna->dev_lock));

	if (flags == AIE4_HWCTX_DISCONNECT || flags == AIE4_HWCTX_ERROR)
		has_reset = true;

	mutex_lock(&priv->io_lock);
	if (has_reset)
		WRITE_ONCE(priv->has_reset, true);
	cert_comp = priv->cert_comp;
	WRITE_ONCE(priv->cert_comp, NULL);
	mutex_unlock(&priv->io_lock);

	if (cert_comp) {
		wake_up_all(&cert_comp->waitq);
		aie4_put_cert_comp(cert_comp);
	}

	if (has_reset)
		wake_up_all(&priv->job_list_wq);

	if (flags != AIE4_HWCTX_DISCONNECT && priv->hw_ctx_id != CTX_INVALID_ID)
		aie4_msg_destroy_context(ndev, priv->hw_ctx_id);

	priv->hw_ctx_id = CTX_INVALID_ID;
	hwctx->fw_ctx_id = -1;
	hwctx->doorbell_offset = CTX_INVALID_DOORBELL;

	/*
	 * When has_reset is true (AIE4_HWCTX_DISCONNECT or ERROR), cancel_work_sync()
	 * is skipped so the worker can wake up and abort in-flight jobs. Callers
	 * that re-create the context after DISCONNECT (e.g. reset recovery) must
	 * synchronize the worker (via aie4_hwctx_wait_for_running()) before calling
	 * aie4_hwctx_create().
	 */
	if (!has_reset)
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

	/*
	 * The HSA queue lives in a user-allocated BO (umq_bo_hdl) in both user- and
	 * kernel-mode submission; the driver does not allocate it privately. Under
	 * PASID/SVA the device reaches the queue through the submitting process's
	 * own page tables, so it must have a user virtual address - a kernel-private
	 * buffer would be unreachable by the device.
	 */
	umq_bo = amdxdna_gem_get_obj(hwctx->client, hwctx->umq_bo_hdl, AMDXDNA_BO_SHARE);
	if (!umq_bo) {
		XDNA_ERR(xdna, "cannot find umq_bo handle %d", hwctx->umq_bo_hdl);
		return -ENOENT;
	}

	/*
	 * Kernel-mode submission: the driver fills the host queue and rings the
	 * doorbell, so the user umq_bo must hold the header plus the direct and
	 * level-1 indirect packet arrays.
	 */
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

	/*
	 * The queue content is driver-owned and never trusted from user space
	 * (only read_index is read back to detect completion). Lay out the
	 * direct packets right after the header and the indirect packets after
	 * them, and publish the same base via data_address for CERT.
	 */
	data_dev_addr = amdxdna_gem_dev_addr(umq_bo) + sizeof(*qhdr);
	priv->umq_pkts = umq_va + sizeof(*qhdr);
	priv->umq_indirect_pkts = umq_va + sizeof(*qhdr) + pkts_sz;
	priv->umq_indirect_pkts_dev_addr = data_dev_addr + pkts_sz;

	/*
	 * Only the header + direct/indirect packet regions are driver-owned and
	 * used for kernel submission; the size check above guarantees they fit.
	 * Clear just that range, not the whole user-sized BO, so an oversized
	 * umq_bo cannot force a huge memset (and page faults) under dev_lock.
	 */
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
	priv->hw_ctx_id = CTX_INVALID_ID;

	/*
	 * io_lock guards the per-hwctx cert_comp binding (the connected sentinel)
	 * for every ctx, so initialize it unconditionally.  kzalloc left cert_comp
	 * NULL: disconnected until create links it.
	 */
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

	/*
	 * ERROR sets has_reset (like a TDR reset) so the worker drains the running
	 * list - the ctx is gone, so in-flight jobs are reaped - and stays live
	 * for aie4_hwctx_wait_for_running() to wait on. Submitters are already gone
	 * (amdxdna_hwctx_destroy_rcu() synchronize_srcu'd them out), then the
	 * queue is torn down.
	 */
	aie4_hwctx_destroy(hwctx, AIE4_HWCTX_ERROR);
	if (priv->job_work_q) {
		aie4_hwctx_wait_for_running(hwctx);
		destroy_workqueue(priv->job_work_q);
	}
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
	struct amdxdna_hwctx_priv *priv = hwctx->priv;
	struct amdxdna_dev *xdna = hwctx->client->xdna;
	u64 ri, wi;

	/*
	 * Sample read_index (written by CERT) before write_index. CERT can
	 * never complete more than has been published, so a write_index sampled
	 * after read_index always satisfies wi >= ri; sampling write_index
	 * first races the submit path / CERT and yields a bogus ri > wi.
	 *
	 * In kernel-mode submission write_index is the driver's host-owned copy
	 * in coherent kernel memory (always >= the value mirrored into the UMQ,
	 * and the device never writes it).
	 *
	 * Security: read_index lives in the umq_bo, which the owning process can
	 * map. Under PASID/SVA the device reaches the queue through that process's
	 * own page tables, preventing access to other contexts' unshared memory.
	 * A forged read_index only completes the process's own command early and
	 * corrupts or hangs itself; however, if BOs are exported via dma-buf,
	 * premature fence signaling can cause an importing process or device to
	 * observe DMA completion before CERT has finished processing.
	 */
	ri = READ_ONCE(*priv->umq_read_index);
	/* Order the read_index sample before the write_index sample. */
	smp_rmb();
	wi = READ_ONCE(priv->write_index);

	/*
	 * CERT cannot update read index as uint64 atomically. Driver may read
	 * a half-updated read index when it has bits in the high 32 bits. If it
	 * looks invalid, re-sample once -- WITHOUT sleeping, since this can run as
	 * a wait_event() condition. If still invalid, report not-advanced; the
	 * waiter re-checks on the next completion wake or timeout.
	 */
	if (!valid_queue_index(ri, wi, CTX_MAX_CMDS)) {
		ri = READ_ONCE(*priv->umq_read_index);
		/* Order the read_index sample before the write_index sample. */
		smp_rmb();
		wi = READ_ONCE(priv->write_index);
		if (!valid_queue_index(ri, wi, CTX_MAX_CMDS)) {
			/*
			 * Still invalid (torn 64-bit read, or a transient
			 * accounting skew). Return the last valid read_index
			 * instead of 0: read_index only advances, so the cached
			 * value is a safe lower bound -- it never reports a
			 * command complete that isn't, and never regresses the
			 * worker into falsely timing out a finished job.
			 */
			XDNA_DBG(xdna, "Invalid index, ri %llu, wi %llu", ri, wi);
			return READ_ONCE(priv->last_read_index);
		}
	}

	WRITE_ONCE(priv->last_read_index, ri);
	return ri;
}

/*
 * The ctx is "connected" as long as @comp is still the cert_comp linked to it.
 * A disconnect (teardown/reset) unlinks (and may re-link a fresh) cert_comp, so
 * a changed pointer means the caller must retry (-EAGAIN). This runs as a
 * wait_event() condition on the completion hot path (cert_comp->waitq is shared
 * per MSI-X), so keep it lockless: the caller pins @comp with a kref, making
 * this a pure pointer-identity compare - never a dereference, ABA-safe - and
 * READ_ONCE pairs with the WRITE_ONCE in aie4_hwctx_create()/
 * aie4_hwctx_destroy().
 */
static bool check_cert_comp_linked(struct amdxdna_hwctx *hwctx, struct cert_comp *comp)
{
	/* READ_ONCE pairs with the link/unlink WRITE_ONCE. */
	return comp == READ_ONCE(hwctx->priv->cert_comp);
}

static inline bool check_cmd_done(struct amdxdna_hwctx *hwctx, u64 seq, struct cert_comp *comp)
{
	/*
	 * Runs as a wait_event() condition, so it must not sleep.
	 * check_cert_comp_linked() is lockless (a READ_ONCE pointer compare); a
	 * disconnect (teardown/reset) unlinks @comp and breaks the wait, and the
	 * caller then confirms real completion by re-reading read_index, so a
	 * disconnect wake is not mistaken for success.
	 */
	if (!check_cert_comp_linked(hwctx, comp))
		return true;

	return get_read_index(hwctx) > seq;
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
					       check_cmd_done(hwctx, seq, cert_comp),
					       wait_jifs);

	if (!ret)
		ret = -ETIME;
	else if (ret > 0 && get_read_index(hwctx) <= seq)
		/* Woke on disconnect/reset, not on real completion. */
		ret = -EAGAIN;

	aie4_put_cert_comp(cert_comp);

	return ret <= 0 ? ret : 0;
}

/* ---- kernel-mode submission (driver fills the queue and rings doorbell) ---- */

/* Publish a command to CERT and return the assigned command sequence (slot). */
static u64 publish_cmd(struct amdxdna_hwctx *hwctx)
{
	struct amdxdna_hwctx_priv *priv = hwctx->priv;
	u64 wi = priv->write_index;

	/* Paired with the lockless READ_ONCE() readers of write_index. */
	WRITE_ONCE(priv->write_index, wi + 1);
	/* Order the packet-slot writes before CERT sees the new write_index. */
	wmb();
	WRITE_ONCE(*priv->umq_write_index, wi + 1);
	return wi;
}

static int wait_till_seq_completed(struct amdxdna_hwctx *hwctx, u64 seq)
{
	struct cert_comp *cert_comp;
	int ret;

	/*
	 * Freezable + interruptible: the submit path (wait_till_connected_hsa_not_full)
	 * reaches here while holding hwctx_srcu, and ctx teardown blocks on
	 * synchronize_srcu(), so a signal (e.g. the app being killed) must be
	 * able to unwind the wait - otherwise a full queue with a silent CERT
	 * would hang the submitter in D state and stall teardown forever.
	 * TASK_FREEZABLE lets the freezer suspend this wait in place during
	 * S3/S4 instead of aborting the suspend. Harmless for the job worker
	 * kthread (never gets a signal; simply freezes/thaws around it).
	 */
	cert_comp = aie4_get_cert_comp(hwctx);
	if (!cert_comp)
		return -EAGAIN;

	ret = wait_event_freezable(cert_comp->waitq,
				   check_cmd_done(hwctx, seq, cert_comp));
	if (ret) {
		aie4_put_cert_comp(cert_comp);
		return ret;	/* -ERESTARTSYS: signal on the submit path */
	}

	if (check_cert_comp_linked(hwctx, cert_comp))
		ret = 0;			/* real completion */
	else
		ret = -EAGAIN;			/* disconnect (suspend or TDR) */

	aie4_put_cert_comp(cert_comp);
	return ret;
}

static int wait_till_connected_hsa_not_full(struct amdxdna_hwctx *hwctx,
					    bool wait_through_reset)
{
	struct amdxdna_dev *xdna = hwctx->client->xdna;
	struct amdxdna_hwctx_priv *priv = hwctx->priv;
	u64 wi = READ_ONCE(priv->write_index);
	bool hsa_not_full = !!(wi < CTX_MAX_CMDS);
	int ret;

	do {
		mutex_unlock(&priv->io_lock);
		if (!hsa_not_full) {
			ret = wait_till_seq_completed(hwctx, wi - CTX_MAX_CMDS);
			if (ret && ret != -EAGAIN) {
				mutex_lock(&priv->io_lock);
				return ret;
			}
			if (!ret)
				hsa_not_full = true;
		}
		ret = wait_event_freezable(priv->job_list_wq,
					   aie4_hwctx_connected(hwctx) ||
					   (!wait_through_reset &&
					    aie4_hwctx_has_reset(hwctx)));
		mutex_lock(&priv->io_lock);
		if (ret)
			return ret;
		if (!wait_through_reset && aie4_hwctx_has_reset(hwctx)) {
			XDNA_DBG(xdna, "ctx %s reset while submitting; unwinding -ECONNRESET",
				 hwctx->name);
			return -ECONNRESET;
		}
	} while (!hsa_not_full || !aie4_hwctx_connected(hwctx));

	return 0;
}

static int fill_indirect_pkt(struct amdxdna_hwctx_priv *priv, u64 slot_idx,
			     u32 total_slots, struct amdxdna_cmd_start_dpu *dpu,
			     u16 entries)
{
	struct host_queue_packet *pkt = &priv->umq_pkts[slot_idx];
	struct host_indirect_packet_entry *hipe =
		(struct host_indirect_packet_entry *)(pkt->data);
	u16 i;

	for (i = 0; i < entries; i++, dpu++, hipe++) {
		struct host_indirect_packet_data *hipd;
		u64 indirect_pkt_dev_addr;
		u32 uci = dpu->uc_index;
		u32 idx;

		/*
		 * dpu is the user-shared cmd_abo payload, so uc_index is read at
		 * use time here and indexes priv->umq_indirect_pkts[]. Reject an
		 * out-of-range value: the slot is reused, so skipping the entry
		 * would leave a stale one that count still advertises to CERT.
		 * Abort before the packet is published.
		 */
		if (uci >= HSA_MAX_LEVEL1_INDIRECT_ENTRIES) {
			XDNA_ERR(priv->hwctx->client->xdna, "Invalid uc index %d", uci);
			return -EINVAL;
		}
		idx = uci * total_slots + slot_idx;
		hipd = &priv->umq_indirect_pkts[idx];
		indirect_pkt_dev_addr = priv->umq_indirect_pkts_dev_addr +
			sizeof(struct host_indirect_packet_data) * idx;

		/* Point the indirect entry at the indirect packet. */
		hipe->host_addr_low = lower_32_bits(indirect_pkt_dev_addr);
		hipe_set_host_addr_high(&hipe->host_addr_high_uc_index,
					upper_32_bits(indirect_pkt_dev_addr));
		hipe_set_uc_index(&hipe->host_addr_high_uc_index, uci);

		/* Fill in the indirect packet. */
		hipd->payload.dpu_control_code_host_addr_low =
			lower_32_bits(dpu->instruction_buffer);
		hipd->payload.dpu_control_code_host_addr_high =
			upper_32_bits(dpu->instruction_buffer);
		hipd->payload.dtrace_buf_host_addr_low =
			lower_32_bits(dpu->dtrace_buffer);
		hipd->payload.dtrace_buf_host_addr_high =
			lower_16_bits(upper_32_bits(dpu->dtrace_buffer));
	}
	pkt->pkt_header.common_header.distribute = 1;
	pkt->pkt_header.common_header.indirect = 1;
	pkt->pkt_header.common_header.count = entries * sizeof(*hipe);
	return 0;
}

static void fill_direct_pkt(struct amdxdna_hwctx_priv *priv, u64 slot_idx,
			    struct amdxdna_cmd_start_dpu *dpu)
{
	struct host_queue_packet *pkt = &priv->umq_pkts[slot_idx];
	struct exec_buf *ebuf = (struct exec_buf *)(pkt->data);

	memset(pkt->data, 0, sizeof(pkt->data));
	ebuf->dpu_control_code_host_addr_low = lower_32_bits(dpu->instruction_buffer);
	ebuf->dpu_control_code_host_addr_high = upper_32_bits(dpu->instruction_buffer);
	ebuf->dtrace_buf_host_addr_low = lower_32_bits(dpu->dtrace_buffer);
	ebuf->dtrace_buf_host_addr_high = lower_16_bits(upper_32_bits(dpu->dtrace_buffer));
	pkt->pkt_header.common_header.distribute = 0;
	pkt->pkt_header.common_header.indirect = 0;
	pkt->pkt_header.common_header.count = sizeof(*ebuf);
}

/*
 * Build and submit one HSA command for @cmd_abo into the user host queue and
 * ring the doorbell. Called with io_lock held.
 *
 * Security: cmd_abo is shared with user space; cache and validate its fields
 * before use and never trust the queue content (only read_index is read back).
 */
static int submit_one_cmd(struct amdxdna_hwctx *hwctx,
			  struct amdxdna_gem_obj *cmd_abo, bool last_of_chain,
			  bool first_cmd, u64 *seq)
{
	struct amdxdna_hwctx_priv *priv = hwctx->priv;
	struct amdxdna_dev *xdna = hwctx->client->xdna;
	struct amdxdna_cmd_start_dpu *dpu;
	struct host_queue_packet *pkt;
	u32 payload_size;
	u64 slot_idx;
	u16 chained;
	int ret;
	u32 op;

	op = amdxdna_cmd_get_op(cmd_abo);
	if (op != ERT_START_DPU) {
		XDNA_ERR(xdna, "Invalid exec buf op, %d", op);
		return -EINVAL;
	}

	dpu = amdxdna_cmd_get_payload(cmd_abo, &payload_size);
	if (!dpu) {
		XDNA_ERR(xdna, "Invalid DPU payload");
		return -EINVAL;
	}
	/*
	 * cmd_abo is shared with user space; validate the cached chained count
	 * against the actual payload size before dereferencing chained+1 DPU
	 * entries, so a bogus count cannot drive an out-of-bounds read.
	 */
	chained = dpu->chained;
	if (chained >= HSA_MAX_LEVEL1_INDIRECT_ENTRIES) {
		XDNA_ERR(xdna, "Invalid DPU data");
		return -EINVAL;
	}
	if (payload_size < (u32)(chained + 1) * sizeof(*dpu)) {
		XDNA_ERR(xdna, "DPU payload %u too small for %u entries",
			 payload_size, chained + 1);
		return -EINVAL;
	}

	/*
	 * Block until a queue slot is free and the ctx is connected (io_lock is
	 * dropped across the sleeps inside and re-acquired). The only failure is a
	 * signal interrupting the wait (-ERESTARTSYS), e.g. the app being killed.
	 */
	ret = wait_till_connected_hsa_not_full(hwctx, first_cmd);
	if (ret) {
		XDNA_DBG(xdna, "Wait for queue slot / ctx reconnect interrupted, ret %d", ret);
		return ret;
	}

	slot_idx = priv->write_index & (CTX_MAX_CMDS - 1);
	if (chained) {
		ret = fill_indirect_pkt(priv, slot_idx, CTX_MAX_CMDS, dpu, chained + 1);
		if (ret)
			return ret;
	} else {
		fill_direct_pkt(priv, slot_idx, dpu);
	}

	pkt = &priv->umq_pkts[slot_idx];
	pkt->pkt_header.common_header.opcode = OPCODE_EXEC_BUF;
	pkt->pkt_header.common_header.chain_flag =
		last_of_chain ? CHAIN_FLG_LAST_CMD : CHAIN_FLG_NOT_LAST_CMD;
	pkt->pkt_header.common_header.reserved = 0x0;
	pkt->pkt_header.completion_signal = amdxdna_gem_dev_addr(cmd_abo) +
					    offsetof(struct amdxdna_cmd, header);
	*seq = publish_cmd(hwctx);
	aie4_doorbell_ring(hwctx);
	XDNA_DBG(xdna, "Submitted one cmd, %s seq %lld", hwctx->name, *seq);
	return 0;
}

/*
 * Return the head running job without removing it. The job worker keeps the
 * in-flight job on the list while it waits so that a disconnect (suspend) can
 * just leave it there for resume - no dequeue/requeue - and running_job_list is
 * never transiently empty while a job is in flight.
 */
static struct amdxdna_sched_job *peek_running_job(struct amdxdna_hwctx *hwctx)
{
	struct amdxdna_hwctx_priv *priv = hwctx->priv;
	struct amdxdna_sched_job *job;

	mutex_lock(&priv->io_lock);
	job = list_first_entry_or_null(&priv->running_job_list,
				       struct amdxdna_sched_job, aie4_job_list);
	mutex_unlock(&priv->io_lock);
	return job;
}

/* Remove a job from the running list once it is completed or reaped. */
static void dequeue_running_job(struct amdxdna_hwctx *hwctx, struct amdxdna_sched_job *job)
{
	struct amdxdna_hwctx_priv *priv = hwctx->priv;

	mutex_lock(&priv->io_lock);
	list_del(&job->aie4_job_list);
	mutex_unlock(&priv->io_lock);
}

static void aie4_job_release(struct kref *ref)
{
	struct amdxdna_sched_job *job =
		container_of(ref, struct amdxdna_sched_job, refcnt);

	amdxdna_sched_job_cleanup(job);
	if (job->out_fence)
		dma_fence_put(job->out_fence);
	kfree(job);
}

static void job_done(struct amdxdna_sched_job *job)
{
	job->aie4_job_state = AIE4_JOB_STATE_DONE;
	dma_fence_signal(job->fence);
	/*
	 * Release the address-space reference taken at submit. On SVA/IOMMU
	 * platforms the device walks the submitter's page tables while the job
	 * runs, so its mm must stay alive until completion.
	 */
	mmput_async(job->mm);
	kref_put(&job->refcnt, aie4_job_release);
}

static void job_complete(struct amdxdna_sched_job *job)
{
	job_done(job);
}

/*
 * When CERT cannot complete a command (context teardown), the driver advances
 * read_index so any waiter observes the command as finished. Only valid while
 * the context is disconnected -- never race CERT's own read_index updates.
 */
static void update_read_index(struct amdxdna_hwctx *hwctx, u64 idx)
{
	struct amdxdna_hwctx_priv *priv = hwctx->priv;

	/* Order cmd-bo state write before the waiter observes completion. */
	wmb();
	WRITE_ONCE(*priv->umq_read_index, idx);
}

static void job_abort(struct amdxdna_sched_job *job)
{
	struct amdxdna_hwctx *hwctx = job->hwctx;

	XDNA_WARN(hwctx->client->xdna, "aborting %s job %lld", hwctx->name, job->seq);
	amdxdna_cmd_set_state(job->cmd_bo, ERT_CMD_STATE_ABORT);
	dma_fence_set_error(job->fence, -ECANCELED);
	/*
	 * Only force read_index forward when CERT has not already moved it past this
	 * job. On the reset drain read_index is still <= job->seq (CERT stopped), so
	 * advance it here to release waiters. For a partial chain closed by a later
	 * command's LAST_CMD, CERT has already advanced read_index past this job -
	 * do not clobber it.
	 */
	if (get_read_index(hwctx) <= job->seq)
		update_read_index(hwctx, job->seq + 1);
	job_done(job);
}

static void job_worker(struct work_struct *work)
{
	struct amdxdna_hwctx_priv *priv =
		container_of(work, struct amdxdna_hwctx_priv, job_work);
	struct amdxdna_hwctx *hwctx = priv->hwctx;
	struct amdxdna_sched_job *job;

	while ((job = peek_running_job(hwctx))) {
		wait_till_seq_completed(hwctx, job->seq);
		if (get_read_index(hwctx) > job->seq) {
			dequeue_running_job(hwctx, job);
			/*
			 * read_index advanced past this job. A fully published
			 * job (SUBMITTED) ran to completion. A partial chain
			 * (SUBMITTING: a later sub-command failed to publish, so
			 * the chain never got CHAIN_FLG_LAST_CMD) only reaches
			 * here once a *later* command's LAST_CMD closes the
			 * dangling runlist and advances read_index past it - so
			 * report it ABORT, not a false completion. If no such
			 * command follows, read_index never advances and we stay
			 * parked in wait_till_seq_completed() above until the
			 * user's wait_command() times out and breaks the wait
			 * (or ctx teardown reaps it).
			 */
			if (job->aie4_job_state != AIE4_JOB_STATE_SUBMITTED)
				job_abort(job);
			else
				job_complete(job);
		} else if (aie4_hwctx_has_reset(hwctx)) {
			dequeue_running_job(hwctx, job);
			job_abort(job);
		} else {
			/* suspend/resume */
			break;
		}
	}
}

int aie4_hwctx_wait_for_running(struct amdxdna_hwctx *hwctx)
{
	struct amdxdna_hwctx_priv *priv = hwctx->priv;
	struct amdxdna_dev *xdna = hwctx->client->xdna;
	struct amdxdna_sched_job *job;
	long error;
	int ret = 0;

	mutex_lock(&priv->io_lock);
	job = READ_ONCE(priv->pending_head);
	if (job && job->aie4_job_state == AIE4_JOB_STATE_SUBMITTING) {
		mutex_unlock(&priv->io_lock);
		error = wait_event_timeout(priv->job_list_wq,
					   READ_ONCE(priv->pending_head) != job,
					   msecs_to_jiffies(2000));
		if (!error) {
			XDNA_WARN(xdna, "hwctx %s wait for submitting job timed out",
				  hwctx->name);
			ret = -ETIMEDOUT;
		}
	} else {
		mutex_unlock(&priv->io_lock);
	}

	queue_work(priv->job_work_q, &priv->job_work);
	flush_work(&priv->job_work);
	return ret;
}

void aie4_hwctx_resume_jobs(struct amdxdna_hwctx *hwctx)
{
	struct amdxdna_hwctx_priv *priv = hwctx->priv;

	mutex_lock(&priv->io_lock);
	if (list_empty(&priv->running_job_list)) {
		mutex_unlock(&priv->io_lock);
		return;
	}
	aie4_doorbell_ring(hwctx);
	mutex_unlock(&priv->io_lock);

	queue_work(priv->job_work_q, &priv->job_work);
}
/*
 * Submit the command(s) carried by @job into the host queue. Called with
 * io_lock held. A single ERT_START_DPU maps to one queue entry; an
 * ERT_CMD_CHAIN expands to one entry per sub-command, only the last of which
 * carries CHAIN_FLG_LAST_CMD so CERT runs the whole chain back to back.
 *
 * job->seq tracks the last published sequence; the worker waits on it to reap
 * the entire chain. job->aie4_job_state advances past PENDING as soon as any
 * sub-command is published, so the caller knows whether in-flight commands must
 * still be reaped even when a later sub-command fails to enqueue.
 *
 * Security: the chain payload and its BO handles come from user space; cache
 * command_count and validate it against the payload size before walking the
 * handle array so a bogus count cannot drive an out-of-bounds read.
 */
static int submit_job_cmds(struct amdxdna_hwctx *hwctx,
			   struct amdxdna_sched_job *job, u32 op)
{
	struct amdxdna_gem_obj *cmd_abo = job->cmd_bo;
	struct amdxdna_dev *xdna = hwctx->client->xdna;
	struct amdxdna_cmd_chain *payload;
	u32 payload_len, ccnt;
	int ret;
	u32 i;

	/* Single cmd. */
	if (op == ERT_START_DPU) {
		ret = submit_one_cmd(hwctx, cmd_abo, true, true, &job->seq);
		if (!ret)
			job->aie4_job_state = AIE4_JOB_STATE_SUBMITTED;
		return ret;
	}

	/* Cmd chain. */
	payload = amdxdna_cmd_get_payload(cmd_abo, &payload_len);
	if (!payload) {
		XDNA_ERR(xdna, "Invalid cmd payload for chained cmd");
		return -EINVAL;
	}
	ccnt = payload->command_count;
	/*
	 * A chain (runlist) must fit within the queue. CERT advances the host-visible
	 * read_index only once per runlist - at the last sub-command (CHAIN_FLG_LAST_CMD),
	 * not per sub-command - so a chain's own entries never free a queue slot until
	 * the whole chain has been published and run. A chain longer than the queue
	 * could therefore never publish its tail: it would block forever in
	 * wait_till_connected_hsa_not_full() waiting for a slot that only frees at chain end.
	 * Reject ccnt > CTX_MAX_CMDS. Also validate against the payload size before
	 * walking the handle array so a bogus count cannot drive an out-of-bounds read.
	 */
	if (!ccnt || ccnt > CTX_MAX_CMDS ||
	    payload_len < struct_size(payload, data, ccnt)) {
		XDNA_ERR(xdna, "Invalid command count %u", ccnt);
		return -EINVAL;
	}

	for (i = 0; i < ccnt; i++) {
		u32 boh = (u32)(payload->data[i]);
		struct amdxdna_gem_obj *abo;

		abo = amdxdna_gem_get_obj(hwctx->client, boh, AMDXDNA_BO_SHARE);
		if (!abo) {
			XDNA_ERR(xdna, "Failed to find cmd BO %u", boh);
			ret = -ENOENT;
			break;
		}

		/*
		 * submit_one_cmd() blocks in wait_till_connected_hsa_not_full() until the
		 * ctx is connected and a slot is free, so a concurrent suspend/disconnect
		 * is waited out inline rather than returned here. The first sub-command
		 * (i == 0, nothing published yet) waits through a TDR reset and runs on
		 * the recreated ctx; a later sub-command returns -ECONNRESET if a reset
		 * landed while waiting for a slot, so the published prefix is not split
		 * across the reset. It also returns -ERESTARTSYS on a signal, or a
		 * validation error. Break on any; a published prefix is then reaped by
		 * the job worker's reset drain (see below).
		 */
		ret = submit_one_cmd(hwctx, abo, i + 1 == ccnt, i == 0, &job->seq);
		amdxdna_gem_put_obj(abo);
		if (ret)
			break;
		job->aie4_job_state = AIE4_JOB_STATE_SUBMITTING;
	}
	if (i == ccnt)
		job->aie4_job_state = AIE4_JOB_STATE_SUBMITTED;

	/*
	 * As long as at least one sub-command was published, return success so the
	 * caller enqueues the job on the running list; the job worker then reaps the
	 * published prefix and reports the partial chain as failed (ABORT). Only when
	 * nothing was published (i == 0) is the error returned to the caller.
	 */
	if (i > 0)
		return 0;

	return ret;
}

/*
 * Whole-job submission is serialized across submitters that share a ctx via the
 * pending list: a job is appended on entry and only the head of the list is
 * allowed to publish its command(s). Because the head stays on the list for the
 * entire duration of submit_job_cmds() -- which may drop io_lock to wait for
 * free queue slots -- no other submitter can interleave its commands into the
 * middle of the head job's command chain. io_lock protects the lists; the
 * job_list_wq waitqueue notifies parked submitters when the head changes.
 */
/* Publish the current pending-list head for the lockless submit wait condition.
 * Caller holds io_lock.
 */
static void update_pending_head(struct amdxdna_hwctx_priv *priv)
{
	WRITE_ONCE(priv->pending_head,
		   list_first_entry_or_null(&priv->pending_job_list,
					    struct amdxdna_sched_job, aie4_job_list));
}

static void enqueue_pending_job(struct amdxdna_hwctx *hwctx,
				struct amdxdna_sched_job *job)
{
	struct amdxdna_hwctx_priv *priv = hwctx->priv;

	mutex_lock(&priv->io_lock);
	list_add_tail(&job->aie4_job_list, &priv->pending_job_list);
	job->aie4_job_state = AIE4_JOB_STATE_PENDING;
	update_pending_head(priv);
	mutex_unlock(&priv->io_lock);

	/* Let the next pending submitter re-check whether it is now first. */
	wake_up_all(&priv->job_list_wq);
}

static void cancel_pending_job(struct amdxdna_hwctx *hwctx,
			       struct amdxdna_sched_job *job)
{
	struct amdxdna_hwctx_priv *priv = hwctx->priv;

	mutex_lock(&priv->io_lock);
	list_del(&job->aie4_job_list);
	job->aie4_job_state = AIE4_JOB_STATE_INIT;
	update_pending_head(priv);
	mutex_unlock(&priv->io_lock);
	/* Let the next pending submitter re-check whether it is now first. */
	wake_up_all(&priv->job_list_wq);
}

int aie4_cmd_submit(struct amdxdna_hwctx *hwctx, struct amdxdna_sched_job *job, u64 *seq)
{
	struct amdxdna_hwctx_priv *priv = hwctx->priv;
	struct amdxdna_dev *xdna = hwctx->client->xdna;
	struct ww_acquire_ctx acquire_ctx;
	struct amdxdna_gem_obj *abo;
	u32 op;
	int i;
	int ret;

	XDNA_DBG(xdna, "ctx %s job %p received", hwctx->name, job);

	if (!job->cmd_bo) {
		XDNA_ERR(xdna, "No command BO in job");
		return -EINVAL;
	}

	op = amdxdna_cmd_get_op(job->cmd_bo);
	if (op != ERT_START_DPU && op != ERT_CMD_CHAIN) {
		XDNA_ERR(xdna, "Invalid cmd opcode %d", op);
		return -EINVAL;
	}

	INIT_LIST_HEAD(&job->aie4_job_list);

	/*
	 * Hold a reference on the submitter's address space until the job
	 * completes (job_done): on SVA/IOMMU platforms the device walks the
	 * submitter's page tables while the command runs. Balanced with the
	 * mmput_async() in job_done() and the mmput() on the failure paths below.
	 */
	if (!mmget_not_zero(job->mm)) {
		XDNA_ERR(xdna, "Failed to get mm reference");
		return -ESRCH;
	}

	/*
	 * Lock all job BOs and reserve fences to match what aie2 does.
	 * This attaches job->out_fence to each BO's reservation object,
	 * ensuring concurrent invalidation waits for the job to complete.
	 */
	ret = drm_gem_lock_reservations(job->bos, job->bo_cnt, &acquire_ctx);
	if (ret) {
		XDNA_WARN(xdna, "Failed to lock BOs, ret %d", ret);
		goto put_mm;
	}

	for (i = 0; i < job->bo_cnt; i++) {
		ret = dma_resv_reserve_fences(job->bos[i]->resv, 1);
		if (ret) {
			XDNA_WARN(xdna, "Failed to reserve fences %d", ret);
			drm_gem_unlock_reservations(job->bos, job->bo_cnt, &acquire_ctx);
			goto put_mm;
		}
	}

	down_read(&xdna->notifier_lock);
	for (i = 0; i < job->bo_cnt; i++) {
		abo = to_xdna_obj(job->bos[i]);
		if (abo->mem.map_invalid) {
			up_read(&xdna->notifier_lock);
			drm_gem_unlock_reservations(job->bos, job->bo_cnt, &acquire_ctx);
			ret = -EINVAL;
			goto put_mm;
		}
	}

	job->out_fence = dma_fence_get(job->fence);
	for (i = 0; i < job->bo_cnt; i++)
		dma_resv_add_fence(job->bos[i]->resv, job->out_fence, DMA_RESV_USAGE_WRITE);

	up_read(&xdna->notifier_lock);
	drm_gem_unlock_reservations(job->bos, job->bo_cnt, &acquire_ctx);

	/*
	 * Wait until this job is at the head of the pending list before touching
	 * the queue (see enqueue_pending_job). Freezable so the freezer can
	 * suspend a parked submitter in place across S3/S4 rather than aborting
	 * the suspend; still interruptible so a signal (app exit/kill/^C) unwinds
	 * it and does not keep ctx teardown (synchronize_srcu) blocked.
	 */
	enqueue_pending_job(hwctx, job);
	ret = wait_event_freezable(priv->job_list_wq,
				   READ_ONCE(priv->pending_head) == job);
	if (ret) {
		cancel_pending_job(hwctx, job);
		goto signal_fence;
	}

	mutex_lock(&priv->io_lock);
	ret = submit_job_cmds(hwctx, job, op);
	if (ret) {
		mutex_unlock(&priv->io_lock);
		cancel_pending_job(hwctx, job);
		goto signal_fence;
	}

	list_move_tail(&job->aie4_job_list, &priv->running_job_list);
	update_pending_head(priv);
	*seq = job->seq;
	mutex_unlock(&priv->io_lock);

	/* Release the next pending submitter and kick the reaper. */
	wake_up_all(&priv->job_list_wq);
	atomic64_inc(&hwctx->job_submit_cnt);
	queue_work(priv->job_work_q, &priv->job_work);
	return 0;

signal_fence:
	/*
	 * Map internal -ERESTARTSYS to -ECANCELED for the fence so downstream
	 * consumers (e.g. sync_file, dma-buf importers) do not observe internal
	 * signal restart codes, while preserving ret for the syscall return.
	 */
	dma_fence_set_error(job->fence, ret == -ERESTARTSYS ? -ECANCELED : ret);
	dma_fence_signal(job->fence);
	dma_fence_put(job->out_fence);
	job->out_fence = NULL;
put_mm:
	mmput(job->mm);
	return ret;
}
