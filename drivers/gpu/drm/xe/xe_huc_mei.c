// SPDX-License-Identifier: MIT
/*
 * Copyright © 2016-2022 Intel Corporation
 * Copyright © 2026 Arisa Snowbell
 */

/*
 * HuC load and authentication by the GSC, through mei (DG2).
 *
 * On DG2 the GSC both loads and authenticates the HuC. It is reached through
 * the PXP client on HECI1, owned by mei; mei_pxp hands us its ops as an
 * I915_COMPONENT_PXP component, as for i915 (intel_pxp_tee.c). Messages go
 * through gsc_command(), which takes the request and the reply as a
 * scatterlist in device memory.
 *
 * The load is asynchronous: mei binds some time after probe and again after
 * every system resume, and the HuC is lost on every reset and suspend. While a
 * load is pending, user submissions to the video decode engines wait for it on
 * a fence, as on i915.
 */

#include <linux/component.h>
#include <linux/dma-fence.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include <drm/drm_managed.h>
#include <drm/intel/i915_component.h>
#include <drm/intel/i915_pxp_tee_interface.h>

#include "abi/gsc_pxp_commands_abi.h"
#include "regs/xe_guc_regs.h"
#include "xe_bo.h"
#include "xe_device.h"
#include "xe_force_wake.h"
#include "xe_gt_printk.h"
#include "xe_huc.h"
#include "xe_huc_mei.h"
#include "xe_map.h"
#include "xe_mmio.h"
#include "xe_pm.h"
#include "xe_uc_fw.h"

#define PXP43_CMDID_START_HUC_AUTH 0x0000003A /* DG2 */

/* PXP-Input-Packet: HuC load and authentication */
struct pxp43_start_huc_auth_in {
	struct pxp_cmd_header header;
	__le64 huc_base_address;
} __packed;

/*
 * mei binds a few seconds after probe and re-binds after a system resume; past
 * this, media runs without HuC and later loads stop waiting for mei.
 */
#define HUC_MEI_COMPONENT_TIMEOUT_MS 10000

/* The GSC loads the whole binary before it flags the authentication */
#define HUC_MEI_AUTH_TIMEOUT_US 1000000

/* A safety net: the deadline above always settles a pending load first */
#define HUC_MEI_SETTLE_TIMEOUT_MS 15000

static struct xe_huc *mei_to_huc(struct xe_huc_mei *mei)
{
	return container_of(mei, struct xe_huc, mei);
}

static struct xe_gt *huc_to_gt(struct xe_huc *huc)
{
	return container_of(huc, struct xe_gt, uc.huc);
}

static struct xe_gt *mei_to_gt(struct xe_huc_mei *mei)
{
	return huc_to_gt(mei_to_huc(mei));
}

static const char *huc_mei_fence_driver_name(struct dma_fence *fence)
{
	return "xe";
}

static const char *huc_mei_fence_timeline_name(struct dma_fence *fence)
{
	return "huc_load";
}

static const struct dma_fence_ops huc_mei_fence_ops = {
	.get_driver_name = huc_mei_fence_driver_name,
	.get_timeline_name = huc_mei_fence_timeline_name,
};

/* Its own lock: users of the fence may outlive the device */
struct huc_mei_fence {
	struct dma_fence base;
	spinlock_t lock; /* the lock of @base */
};

/* start() runs in the reset and resume paths, which must not wait on reclaim */
static struct dma_fence *huc_mei_fence_create(struct xe_huc_mei *mei)
{
	struct huc_mei_fence *fence = kzalloc_obj(*fence, GFP_NOWAIT | __GFP_NOWARN);

	if (!fence)
		return NULL;

	spin_lock_init(&fence->lock);
	dma_fence_init(&fence->base, &huc_mei_fence_ops, &fence->lock,
		       mei->fence_context, atomic64_inc_return(&mei->seqno));

	return &fence->base;
}

/**
 * xe_huc_mei_is_used() - whether the GSC loads this HuC through mei
 * @huc: the HuC
 *
 * Return: true on DG2 once its HuC is set up to load through the GSC.
 */
bool xe_huc_mei_is_used(struct xe_huc *huc)
{
	return huc->mei.state != XE_HUC_MEI_UNUSED;
}

static u32 huc_mei_load_info(struct xe_gt *gt)
{
	CLASS(xe_force_wake, fw_ref)(gt_to_fw(gt), XE_FW_GT);
	if (!fw_ref.domains)
		return 0;

	return xe_mmio_read32(&gt->mmio, HUC_KERNEL_LOAD_INFO);
}

/* Ends the wait of the current load */
static void huc_mei_settle_locked(struct xe_huc_mei *mei)
{
	lockdep_assert_held(&mei->state_lock);

	if (mei->state != XE_HUC_MEI_LOADING)
		return;

	mei->state = XE_HUC_MEI_SETTLED;
	if (mei->load)
		dma_fence_signal(mei->load);
	cancel_delayed_work(&mei->deadline);
}

static int huc_mei_send_auth(struct xe_huc *huc, struct i915_pxp_component *pxp)
{
	struct xe_gt *gt = huc_to_gt(huc);
	struct xe_device *xe = gt_to_xe(gt);
	struct xe_bo *msg = huc->mei.msg;
	struct pxp43_start_huc_auth_in in = {};
	struct pxp43_huc_auth_out out = {};
	struct scatterlist sg;
	dma_addr_t huc_addr = xe_bo_main_addr(huc->fw.bo, PAGE_SIZE);
	ssize_t ret;

	in.header.api_version = PXP_APIVER(4, 3);
	in.header.command_id = PXP43_CMDID_START_HUC_AUTH;
	in.header.buffer_len = sizeof(in.huc_base_address);
	in.huc_base_address = cpu_to_le64(huc_addr);

	xe_map_memset(xe, &msg->vmap, 0, 0, PAGE_SIZE);
	xe_map_memcpy_to(xe, &msg->vmap, 0, &in, sizeof(in));

	/* the same page carries the request and the reply, as on i915 */
	sg_init_table(&sg, 1);
	sg.length = PAGE_SIZE;
	sg_dma_address(&sg) = xe_bo_main_addr(msg, PAGE_SIZE);
	sg_dma_len(&sg) = PAGE_SIZE;

	xe_gt_dbg(gt, "HuC: asking the GSC to load %zu bytes at DPA %pad\n",
		  xe_bo_size(huc->fw.bo), &huc_addr);

	ret = pxp->ops->gsc_command(pxp->tee_dev, 0, 0, &sg, sizeof(in), &sg);
	if (ret < 0) {
		xe_gt_err(gt, "HuC: GSC command failed: %pe\n", ERR_PTR(ret));
		return ret;
	}
	if (ret < (ssize_t)sizeof(out.header)) {
		xe_gt_err(gt, "HuC: GSC reply too short: %zd bytes\n", ret);
		return -EPROTO;
	}

	xe_map_memcpy_from(xe, &out, &msg->vmap, 0, sizeof(out));

	/*
	 * HuC can survive a shallow suspend, the GSC then refuses the load with
	 * OP_NOT_PERMITTED; the status register check that follows still
	 * catches a HuC that is not there.
	 */
	if (out.header.status != PXP_STATUS_SUCCESS &&
	    out.header.status != PXP_STATUS_OP_NOT_PERMITTED) {
		xe_gt_err(gt, "HuC: GSC refused the load: %#x\n", out.header.status);
		return -EPROTO;
	}

	return 0;
}

static int huc_mei_load_and_auth(struct xe_huc *huc)
{
	struct xe_gt *gt = huc_to_gt(huc);
	struct xe_huc_mei *mei = &huc->mei;
	int ret;

	if (huc_mei_load_info(gt) & HUC_LOAD_SUCCESSFUL)
		return 0;

	scoped_guard(mutex, &mei->lock) {
		/* mei binds later; the bind queues this again */
		if (!mei->pxp)
			return -EAGAIN;

		ret = huc_mei_send_auth(huc, mei->pxp);
		if (ret)
			return ret;
	}

	xe_uc_fw_change_status(&huc->fw, XE_UC_FIRMWARE_TRANSFERRED);

	CLASS(xe_force_wake, fw_ref)(gt_to_fw(gt), XE_FW_GT);
	if (!fw_ref.domains)
		return -ETIMEDOUT;

	ret = xe_mmio_wait32(&gt->mmio, HUC_KERNEL_LOAD_INFO, HUC_LOAD_SUCCESSFUL,
			     HUC_LOAD_SUCCESSFUL, HUC_MEI_AUTH_TIMEOUT_US, NULL, false);
	if (ret)
		xe_gt_err(gt, "HuC: not authenticated after the GSC load, status %#x\n",
			  xe_mmio_read32(&gt->mmio, HUC_KERNEL_LOAD_INFO));

	return ret;
}

/*
 * The work runs on a runtime PM reference its queuer took: suspend flushes it,
 * so it must never resume the device itself.
 */
static void huc_mei_queue(struct xe_huc_mei *mei)
{
	struct xe_device *xe = gt_to_xe(mei_to_gt(mei));

	xe_pm_runtime_get_noresume(xe);
	if (!queue_work(system_dfl_wq, &mei->auth))
		xe_pm_runtime_put(xe);
}

/* Drops a queued but not yet started load along with its reference */
static void huc_mei_cancel(struct xe_huc_mei *mei)
{
	if (cancel_work_sync(&mei->auth))
		xe_pm_runtime_put(gt_to_xe(mei_to_gt(mei)));
	cancel_delayed_work_sync(&mei->deadline);
}

static bool huc_mei_gt_up(struct xe_huc_mei *mei)
{
	guard(spinlock_irqsave)(&mei->state_lock);

	return mei->state == XE_HUC_MEI_LOADING || mei->state == XE_HUC_MEI_SETTLED;
}

static void huc_mei_auth_work(struct work_struct *work)
{
	struct xe_huc_mei *mei = container_of(work, struct xe_huc_mei, auth);
	struct xe_huc *huc = mei_to_huc(mei);
	struct xe_gt *gt = huc_to_gt(huc);
	int ret;

	guard(xe_pm_runtime_release_only)(gt_to_xe(gt));

	/* stop() cancels this work before the GT goes down */
	if (!huc_mei_gt_up(mei))
		return;

	ret = huc_mei_load_and_auth(huc);
	if (ret == -EAGAIN)
		return;

	if (ret) {
		xe_gt_err(gt, "HuC: load and authentication via GSC failed: %pe\n", ERR_PTR(ret));
		xe_uc_fw_change_status(&huc->fw, XE_UC_FIRMWARE_LOAD_FAIL);
	} else {
		if (!mei->announced)
			xe_gt_info(gt, "HuC: authenticated via GSC (version %u.%u.%u)\n",
				   huc->fw.versions.found[XE_UC_FW_VER_RELEASE].major,
				   huc->fw.versions.found[XE_UC_FW_VER_RELEASE].minor,
				   huc->fw.versions.found[XE_UC_FW_VER_RELEASE].patch);
		mei->announced = true;
		xe_uc_fw_change_status(&huc->fw, XE_UC_FIRMWARE_RUNNING);
	}

	scoped_guard(spinlock_irqsave, &mei->state_lock)
		huc_mei_settle_locked(mei);
}

static void huc_mei_deadline_work(struct work_struct *work)
{
	struct xe_huc_mei *mei = container_of(work, struct xe_huc_mei, deadline.work);
	struct xe_gt *gt = mei_to_gt(mei);

	/* not @mei->lock: a GSC command that never returns holds it */
	guard(spinlock_irqsave)(&mei->state_lock);
	if (mei->state != XE_HUC_MEI_LOADING)
		return;

	if (mei->bound) {
		xe_gt_warn(gt, "HuC: the GSC load takes over %u ms, media no longer waits for it\n",
			   HUC_MEI_COMPONENT_TIMEOUT_MS);
	} else {
		xe_gt_notice(gt, "HuC: no mei PXP component after %u ms (is mei_pxp loaded?), media runs without HuC until it binds\n",
			     HUC_MEI_COMPONENT_TIMEOUT_MS);
		mei->absent = true;
	}
	huc_mei_settle_locked(mei);
}

static int huc_mei_component_bind(struct device *xe_kdev, struct device *tee_kdev, void *data)
{
	struct xe_device *xe = kdev_to_xe_device(xe_kdev);
	struct xe_huc_mei *mei = &xe_root_mmio_gt(xe)->uc.huc.mei;
	struct i915_pxp_component *pxp = data;

	scoped_guard(mutex, &mei->lock) {
		mei->pxp = pxp;
		mei->pxp->tee_dev = tee_kdev;
	}

	scoped_guard(spinlock_irqsave, &mei->state_lock) {
		mei->bound = true;
		mei->absent = false;
	}

	xe_gt_dbg(mei_to_gt(mei), "HuC: mei PXP component bound (%s)\n", dev_name(tee_kdev));

	/* a suspended device loads the HuC when start() runs at resume */
	if (!xe_pm_runtime_get_if_active(xe))
		return 0;

	if (huc_mei_gt_up(mei))
		huc_mei_queue(mei);
	xe_pm_runtime_put(xe);

	return 0;
}

static void huc_mei_component_unbind(struct device *xe_kdev, struct device *tee_kdev, void *data)
{
	struct xe_device *xe = kdev_to_xe_device(xe_kdev);
	struct xe_huc_mei *mei = &xe_root_mmio_gt(xe)->uc.huc.mei;

	scoped_guard(spinlock_irqsave, &mei->state_lock)
		mei->bound = false;

	scoped_guard(mutex, &mei->lock)
		mei->pxp = NULL;
}

static const struct component_ops huc_mei_component_ops = {
	.bind = huc_mei_component_bind,
	.unbind = huc_mei_component_unbind,
};

static void huc_mei_go_down(struct xe_huc_mei *mei)
{
	struct dma_fence *load;

	scoped_guard(spinlock_irqsave, &mei->state_lock) {
		huc_mei_settle_locked(mei);
		mei->state = XE_HUC_MEI_DOWN;
		load = mei->load;
		mei->load = NULL;
	}
	dma_fence_put(load);
}

static void huc_mei_fini(void *arg)
{
	struct xe_huc_mei *mei = arg;

	huc_mei_go_down(mei);
	component_del(gt_to_xe(mei_to_gt(mei))->drm.dev, &huc_mei_component_ops);
	huc_mei_cancel(mei);
}

/* The DG2 blob is parsed for the GSC, it has no image to DMA */
static int huc_mei_check_mode(struct xe_huc *huc)
{
	struct xe_gt *gt = huc_to_gt(huc);

	CLASS(xe_force_wake, fw_ref)(gt_to_fw(gt), XE_FW_GT);
	if (!fw_ref.domains)
		return -ETIMEDOUT;

	if (!(xe_mmio_read32(&gt->mmio, GUC_SHIM_CONTROL2) & GSC_LOADS_HUC)) {
		xe_gt_err(gt, "HuC: HW in DMA mode, but the DG2 blob is GSC-only\n");
		return -ENOEXEC;
	}

	return 0;
}

/**
 * xe_huc_mei_init() - set up HuC loading through the GSC on DG2
 * @huc: the HuC, its firmware already in VRAM
 *
 * A failure leaves the HuC unloaded and does not fail the probe.
 */
void xe_huc_mei_init(struct xe_huc *huc)
{
	struct xe_gt *gt = huc_to_gt(huc);
	struct xe_device *xe = gt_to_xe(gt);
	struct xe_tile *tile = gt_to_tile(gt);
	struct xe_huc_mei *mei = &huc->mei;
	struct xe_bo *bo;
	int ret;

	if (xe->info.platform != XE_DG2)
		return;

	if (!IS_ENABLED(CONFIG_INTEL_MEI_PXP) || !IS_ENABLED(CONFIG_INTEL_MEI_GSC)) {
		xe_gt_notice(gt, "HuC: can't load without CONFIG_INTEL_MEI_PXP and CONFIG_INTEL_MEI_GSC\n");
		xe_uc_fw_change_status(&huc->fw, XE_UC_FIRMWARE_INIT_FAIL);
		return;
	}

	ret = huc_mei_check_mode(huc);
	if (ret)
		goto fail;

	ret = drmm_mutex_init(&xe->drm, &mei->lock);
	if (ret)
		goto fail;

	spin_lock_init(&mei->state_lock);
	mei->fence_context = dma_fence_context_alloc(1);
	INIT_WORK(&mei->auth, huc_mei_auth_work);
	INIT_DELAYED_WORK(&mei->deadline, huc_mei_deadline_work);

	/* pinned VRAM BOs are contiguous; the GSC reads the page by its DPA */
	bo = xe_managed_bo_create_pin_map(xe, tile, PAGE_SIZE, XE_BO_FLAG_VRAM_IF_DGFX(tile));
	if (IS_ERR(bo)) {
		ret = PTR_ERR(bo);
		goto fail;
	}
	mei->msg = bo;
	mei->state = XE_HUC_MEI_DOWN;

	ret = component_add_typed(xe->drm.dev, &huc_mei_component_ops, I915_COMPONENT_PXP);
	if (ret) {
		mei->state = XE_HUC_MEI_UNUSED;
		goto fail;
	}

	ret = devm_add_action_or_reset(xe->drm.dev, huc_mei_fini, mei);
	if (ret) {
		mei->state = XE_HUC_MEI_UNUSED;
		goto fail;
	}

	return;

fail:
	xe_gt_err(gt, "HuC: can't load through the GSC: %pe\n", ERR_PTR(ret));
	/* not loadable: no DMA upload or GuC auth of a GSC-only blob either */
	xe_uc_fw_change_status(&huc->fw, XE_UC_FIRMWARE_INIT_FAIL);
}

/**
 * xe_huc_mei_start() - ask the GSC for the HuC once the GT runs
 * @huc: the HuC
 *
 * Called at the end of every uC load (probe, GT reset, resume) with the device
 * awake. The HuC does not survive those, the GSC must load it again; video
 * decode submissions wait for it until it settles.
 */
void xe_huc_mei_start(struct xe_huc *huc)
{
	struct xe_huc_mei *mei = &huc->mei;
	struct dma_fence *load, *old;
	bool wait;

	if (!xe_huc_mei_is_used(huc) || !xe_uc_fw_is_loadable(&huc->fw))
		return;

	scoped_guard(spinlock_irqsave, &mei->state_lock)
		wait = !mei->absent;

	/* without a fence submissions just don't wait */
	load = wait ? huc_mei_fence_create(mei) : NULL;

	scoped_guard(spinlock_irqsave, &mei->state_lock) {
		huc_mei_settle_locked(mei);
		old = mei->load;
		mei->state = wait ? XE_HUC_MEI_LOADING : XE_HUC_MEI_SETTLED;
		mei->load = load;
	}
	dma_fence_put(old);

	if (wait)
		mod_delayed_work(system_dfl_wq, &mei->deadline,
				 msecs_to_jiffies(HUC_MEI_COMPONENT_TIMEOUT_MS));
	huc_mei_queue(mei);
}

/**
 * xe_huc_mei_stop() - forget the HuC before a reset, a suspend or an FLR
 * @huc: the HuC
 *
 * Waits for a running load and drops a pending one; submissions stop waiting
 * until the next start().
 */
void xe_huc_mei_stop(struct xe_huc *huc)
{
	struct xe_huc_mei *mei = &huc->mei;

	if (!xe_huc_mei_is_used(huc))
		return;

	huc_mei_go_down(mei);
	huc_mei_cancel(mei);
}

/**
 * xe_huc_mei_load_fence() - the fence of a pending HuC load
 * @huc: the HuC
 *
 * Return: a reference to the fence that signals when the pending load settles,
 * or NULL when no load is pending.
 */
struct dma_fence *xe_huc_mei_load_fence(struct xe_huc *huc)
{
	struct xe_huc_mei *mei = &huc->mei;

	if (!xe_huc_mei_is_used(huc))
		return NULL;

	guard(spinlock_irqsave)(&mei->state_lock);
	if (mei->state != XE_HUC_MEI_LOADING || !mei->load)
		return NULL;

	return dma_fence_get(mei->load);
}

/**
 * xe_huc_mei_wait() - wait until a pending HuC load settled
 * @huc: the HuC
 *
 * A HuC query must not race the load that follows a probe or a resume, or
 * userspace would see a HuC-less device.
 *
 * Return: 0 once settled or timed out, -ERESTARTSYS when interrupted.
 */
int xe_huc_mei_wait(struct xe_huc *huc)
{
	struct dma_fence *load = xe_huc_mei_load_fence(huc);
	long ret;

	if (!load)
		return 0;

	ret = dma_fence_wait_timeout(load, true, msecs_to_jiffies(HUC_MEI_SETTLE_TIMEOUT_MS));
	dma_fence_put(load);

	return ret < 0 ? ret : 0;
}
