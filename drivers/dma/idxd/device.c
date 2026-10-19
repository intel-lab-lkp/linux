// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2019 Intel Corporation. All rights rsvd. */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/dmaengine.h>
#include <linux/irq.h>
#include <uapi/linux/idxd.h>
#include "../dmaengine.h"
#include "idxd.h"
#include "registers.h"

/* generous bound: drain of a loaded WQ is the slowest legitimate command */
#define IDXD_CMD_TIMEOUT_MS	30000

static void idxd_cmd_exec(struct idxd_device *idxd, int cmd_code, u32 operand,
			  u32 *status);
static void idxd_device_wqs_clear_state(struct idxd_device *idxd);
static void idxd_wq_disable_cleanup(struct idxd_wq *wq);
static inline bool idxd_device_is_halted(struct idxd_device *idxd);
static int idxd_wq_config_write(struct idxd_wq *wq);

/* Interrupt control bits */
void idxd_unmask_error_interrupts(struct idxd_device *idxd)
{
	union genctrl_reg genctrl;

	genctrl.bits = ioread32(idxd->reg_base + IDXD_GENCTRL_OFFSET);
	genctrl.softerr_int_en = 1;
	genctrl.halt_int_en = 1;
	iowrite32(genctrl.bits, idxd->reg_base + IDXD_GENCTRL_OFFSET);
}

void idxd_mask_error_interrupts(struct idxd_device *idxd)
{
	union genctrl_reg genctrl;

	genctrl.bits = ioread32(idxd->reg_base + IDXD_GENCTRL_OFFSET);
	genctrl.softerr_int_en = 0;
	genctrl.halt_int_en = 0;
	iowrite32(genctrl.bits, idxd->reg_base + IDXD_GENCTRL_OFFSET);
}

static void free_hw_descs(struct idxd_wq *wq)
{
	int i;

	for (i = 0; i < wq->num_descs; i++)
		kfree(wq->hw_descs[i]);

	kfree(wq->hw_descs);
}

static int alloc_hw_descs(struct idxd_wq *wq, int num)
{
	struct device *dev = &wq->idxd->pdev->dev;
	int i;
	int node = dev_to_node(dev);

	wq->hw_descs = kcalloc_node(num, sizeof(struct dsa_hw_desc *),
				    GFP_KERNEL, node);
	if (!wq->hw_descs)
		return -ENOMEM;

	for (i = 0; i < num; i++) {
		wq->hw_descs[i] = kzalloc_node(sizeof(*wq->hw_descs[i]),
					       GFP_KERNEL, node);
		if (!wq->hw_descs[i]) {
			free_hw_descs(wq);
			return -ENOMEM;
		}
	}

	return 0;
}

static void free_descs(struct idxd_wq *wq)
{
	int i;

	for (i = 0; i < wq->num_descs; i++)
		kfree(wq->descs[i]);

	kfree(wq->descs);
}

static int alloc_descs(struct idxd_wq *wq, int num)
{
	struct device *dev = &wq->idxd->pdev->dev;
	int i;
	int node = dev_to_node(dev);

	wq->descs = kcalloc_node(num, sizeof(struct idxd_desc *),
				 GFP_KERNEL, node);
	if (!wq->descs)
		return -ENOMEM;

	for (i = 0; i < num; i++) {
		wq->descs[i] = kzalloc_node(sizeof(*wq->descs[i]),
					    GFP_KERNEL, node);
		if (!wq->descs[i]) {
			free_descs(wq);
			return -ENOMEM;
		}
	}

	return 0;
}

/* WQ control bits */
int idxd_wq_alloc_resources(struct idxd_wq *wq)
{
	struct idxd_device *idxd = wq->idxd;
	struct device *dev = &idxd->pdev->dev;
	int rc, num_descs, i;

	if (wq->type != IDXD_WQT_KERNEL)
		return 0;

	/* A fresh pool: nothing outstanding can land in it. */
	wq->stuck = false;
	INIT_LIST_HEAD(&wq->quarantine);
	num_descs = wq_dedicated(wq) ? wq->size : wq->threshold;
	wq->num_descs = num_descs;

	rc = alloc_hw_descs(wq, num_descs);
	if (rc < 0)
		return rc;

	wq->compls_size = num_descs * idxd->data->compl_size;
	wq->compls = dma_alloc_coherent(dev, wq->compls_size, &wq->compls_addr, GFP_KERNEL);
	if (!wq->compls) {
		rc = -ENOMEM;
		goto fail_alloc_compls;
	}

	rc = alloc_descs(wq, num_descs);
	if (rc < 0)
		goto fail_alloc_descs;

	rc = sbitmap_queue_init_node(&wq->sbq, num_descs, -1, false, GFP_KERNEL,
				     dev_to_node(dev));
	if (rc < 0)
		goto fail_sbitmap_init;

	for (i = 0; i < num_descs; i++) {
		struct idxd_desc *desc = wq->descs[i];

		desc->hw = wq->hw_descs[i];
		if (idxd->data->type == IDXD_TYPE_DSA)
			desc->completion = &wq->compls[i];
		else if (idxd->data->type == IDXD_TYPE_IAX)
			desc->iax_completion = &wq->iax_compls[i];
		desc->compl_dma = wq->compls_addr + idxd->data->compl_size * i;
		desc->id = i;
		desc->wq = wq;
		desc->cpu = -1;
	}

	return 0;

 fail_sbitmap_init:
	free_descs(wq);
 fail_alloc_descs:
	dma_free_coherent(dev, wq->compls_size, wq->compls, wq->compls_addr);
 fail_alloc_compls:
	free_hw_descs(wq);
	return rc;
}
EXPORT_SYMBOL_NS_GPL(idxd_wq_alloc_resources, "IDXD");

void idxd_wq_free_resources(struct idxd_wq *wq)
{
	struct device *dev = &wq->idxd->pdev->dev;
	LIST_HEAD(dead_descs);

	if (wq->type != IDXD_WQT_KERNEL)
		return;

	/*
	 * The disable frees the pool only after its drain and its reset
	 * completed, and a failed enable has run nothing, so the device no
	 * longer reaches what a quarantined descriptor maps.
	 */
	idxd_wq_quarantine_take(wq, &dead_descs);
	idxd_wq_quarantine_free(wq, &dead_descs);
	free_hw_descs(wq);
	free_descs(wq);
	dma_free_coherent(dev, wq->compls_size, wq->compls, wq->compls_addr);
	sbitmap_queue_free(&wq->sbq);
	wq->type = IDXD_WQT_NONE;
}
EXPORT_SYMBOL_NS_GPL(idxd_wq_free_resources, "IDXD");

int idxd_wq_enable(struct idxd_wq *wq)
{
	struct idxd_device *idxd = wq->idxd;
	struct device *dev = &idxd->pdev->dev;
	u32 status;

	if (wq->state == IDXD_WQ_ENABLED) {
		dev_dbg(dev, "WQ %d already enabled\n", wq->id);
		return 0;
	}

	idxd_cmd_exec(idxd, IDXD_CMD_ENABLE_WQ, wq->id, &status);

	if (status != IDXD_CMDSTS_SUCCESS &&
	    status != IDXD_CMDSTS_ERR_WQ_ENABLED) {
		dev_dbg(dev, "WQ enable failed: %#x\n", status);
		return -ENXIO;
	}

	wq->state = IDXD_WQ_ENABLED;
	set_bit(wq->id, idxd->wq_enable_map);
	dev_dbg(dev, "WQ %d enabled\n", wq->id);
	return 0;
}

int idxd_wq_disable(struct idxd_wq *wq, bool reset_config)
{
	struct idxd_device *idxd = wq->idxd;
	struct device *dev = &idxd->pdev->dev;
	u32 status, operand;
	int rc;

	dev_dbg(dev, "Disabling WQ %d\n", wq->id);

	if (wq->state != IDXD_WQ_ENABLED) {
		dev_dbg(dev, "WQ %d in wrong state: %d\n", wq->id, wq->state);
		return 0;
	}

	/*
	 * Disable WQ does not drain address translations, if WQ attributes are
	 * changed before translations are drained, pending translations can
	 * be issued using updated WQ attibutes, resulting in invalid
	 * translations being cached in the device translation cache.
	 *
	 * To make sure pending translations are drained before WQ
	 * attributes are changed, we use a WQ Drain followed by WQ Reset and
	 * then restore the WQ configuration.
	 *
	 * A drain that timed out leaves descriptors in the queue. Report
	 * it instead of resetting, so that the caller keeps whatever those
	 * descriptors can still reach, a PASID for instance.
	 */
	rc = idxd_wq_drain(wq);
	if (rc)
		return rc;

	operand = BIT(wq->id % 16) | ((wq->id / 16) << 16);
	idxd_cmd_exec(idxd, IDXD_CMD_RESET_WQ, operand, &status);

	if (status != IDXD_CMDSTS_SUCCESS) {
		dev_dbg(dev, "WQ reset failed: %#x\n", status);
		return -ENXIO;
	}

	idxd_wq_config_write(wq);

	if (reset_config)
		idxd_wq_disable_cleanup(wq);
	clear_bit(wq->id, idxd->wq_enable_map);
	wq->state = IDXD_WQ_DISABLED;
	dev_dbg(dev, "WQ %d disabled\n", wq->id);
	return 0;
}

/*
 * The result of a drain or a reset. IDXD_CMDSTS_HW_ERR stands for a
 * command that timed out, or was refused after one did, and then the
 * device may still run what it holds. It is also the device's own code
 * for a hardware error, after which the specification has software read
 * GENSTS. Both leave the device enabled unless it halted, and a halted
 * device runs nothing until it is reset, so what it holds is then as
 * gone as after a drain and that is not a timeout.
 */
static int idxd_cmd_drain_result(struct idxd_device *idxd, u32 status)
{
	if (status != IDXD_CMDSTS_HW_ERR || idxd_device_is_halted(idxd))
		return 0;
	return -ETIMEDOUT;
}

/*
 * Returns -ETIMEDOUT when the command never completed, or was refused
 * because an earlier one never did, in which case descriptors may still
 * be in the device and the caller must not free anything it can write
 * into.
 */
int idxd_wq_drain(struct idxd_wq *wq)
{
	struct idxd_device *idxd = wq->idxd;
	struct device *dev = &idxd->pdev->dev;
	u32 operand, status;

	if (wq->state != IDXD_WQ_ENABLED) {
		dev_dbg(dev, "WQ %d in wrong state: %d\n", wq->id, wq->state);
		return 0;
	}

	dev_dbg(dev, "Draining WQ %d\n", wq->id);
	operand = BIT(wq->id % 16) | ((wq->id / 16) << 16);
	idxd_cmd_exec(idxd, IDXD_CMD_DRAIN_WQ, operand, &status);
	return idxd_cmd_drain_result(idxd, status);
}

int idxd_wq_reset(struct idxd_wq *wq)
{
	struct idxd_device *idxd = wq->idxd;
	struct device *dev = &idxd->pdev->dev;
	u32 operand, status;
	int rc;

	if (wq->state != IDXD_WQ_ENABLED) {
		dev_dbg(dev, "WQ %d in wrong state: %d\n", wq->id, wq->state);
		return 0;
	}

	operand = BIT(wq->id % 16) | ((wq->id / 16) << 16);
	idxd_cmd_exec(idxd, IDXD_CMD_RESET_WQ, operand, &status);
	/*
	 * A reset that timed out, or was refused after a command did,
	 * leaves the queue enabled in the device on its configuration.
	 * Its software state stays enabled too, as after a failed
	 * idxd_wq_disable(), so that the queue is neither reconfigured nor
	 * enabled again on a configuration the device does not run until
	 * a device disable or reset has stopped it.
	 */
	rc = idxd_cmd_drain_result(idxd, status);
	if (rc)
		return rc;
	idxd_wq_disable_cleanup(wq);
	return 0;
}

int idxd_wq_map_portal(struct idxd_wq *wq)
{
	struct idxd_device *idxd = wq->idxd;
	struct pci_dev *pdev = idxd->pdev;
	struct device *dev = &pdev->dev;
	resource_size_t start;

	start = pci_resource_start(pdev, IDXD_WQ_BAR);
	start += idxd_get_wq_portal_full_offset(wq->id, IDXD_PORTAL_LIMITED);

	wq->portal = devm_ioremap(dev, start, IDXD_PORTAL_SIZE);
	if (!wq->portal)
		return -ENOMEM;

	return 0;
}

void idxd_wq_unmap_portal(struct idxd_wq *wq)
{
	struct device *dev = &wq->idxd->pdev->dev;

	devm_iounmap(dev, wq->portal);
	wq->portal = NULL;
	wq->portal_offset = 0;
}

void idxd_wqs_unmap_portal(struct idxd_device *idxd)
{
	int i;

	for (i = 0; i < idxd->max_wqs; i++) {
		struct idxd_wq *wq = idxd->wqs[i];

		if (wq->portal)
			idxd_wq_unmap_portal(wq);
	}
}

static void __idxd_wq_set_pasid_locked(struct idxd_wq *wq, int pasid)
{
	struct idxd_device *idxd = wq->idxd;
	union wqcfg wqcfg;
	unsigned int offset;

	offset = WQCFG_OFFSET(idxd, wq->id, WQCFG_PASID_IDX);
	spin_lock(&idxd->dev_lock);
	wqcfg.bits[WQCFG_PASID_IDX] = ioread32(idxd->reg_base + offset);
	wqcfg.pasid_en = 1;
	wqcfg.pasid = pasid;
	wq->wqcfg->bits[WQCFG_PASID_IDX] = wqcfg.bits[WQCFG_PASID_IDX];
	iowrite32(wqcfg.bits[WQCFG_PASID_IDX], idxd->reg_base + offset);
	spin_unlock(&idxd->dev_lock);
}

int idxd_wq_set_pasid(struct idxd_wq *wq, int pasid)
{
	int rc;

	rc = idxd_wq_disable(wq, false);
	if (rc < 0)
		return rc;

	__idxd_wq_set_pasid_locked(wq, pasid);

	rc = idxd_wq_enable(wq);
	if (rc < 0)
		return rc;

	return 0;
}

int idxd_wq_disable_pasid(struct idxd_wq *wq)
{
	struct idxd_device *idxd = wq->idxd;
	int rc;
	union wqcfg wqcfg;
	unsigned int offset;

	rc = idxd_wq_disable(wq, false);
	if (rc < 0)
		return rc;

	offset = WQCFG_OFFSET(idxd, wq->id, WQCFG_PASID_IDX);
	spin_lock(&idxd->dev_lock);
	wqcfg.bits[WQCFG_PASID_IDX] = ioread32(idxd->reg_base + offset);
	wqcfg.pasid_en = 0;
	wqcfg.pasid = 0;
	iowrite32(wqcfg.bits[WQCFG_PASID_IDX], idxd->reg_base + offset);
	spin_unlock(&idxd->dev_lock);

	rc = idxd_wq_enable(wq);
	if (rc < 0)
		return rc;

	return 0;
}

static void idxd_wq_disable_cleanup(struct idxd_wq *wq)
{
	struct idxd_device *idxd = wq->idxd;

	lockdep_assert_held(&wq->wq_lock);
	wq->state = IDXD_WQ_DISABLED;
	memset(wq->wqcfg, 0, idxd->wqcfg_size);
	wq->threshold = 0;
	wq->priority = 0;
	wq->enqcmds_retries = IDXD_ENQCMDS_RETRIES;
	wq->flags = 0;
	memset(wq->name, 0, WQ_NAME_SIZE);
	wq->max_xfer_bytes = WQ_DEFAULT_MAX_XFER;
	idxd_wq_set_max_batch_size(idxd->data->type, wq, WQ_DEFAULT_MAX_BATCH);
	idxd_wq_set_init_max_sgl_size(idxd, wq);
	if (wq->opcap_bmap)
		bitmap_copy(wq->opcap_bmap, idxd->opcap_bmap, IDXD_MAX_OPCAP_BITS);
}

static void idxd_wq_device_reset_cleanup(struct idxd_wq *wq)
{
	lockdep_assert_held(&wq->wq_lock);

	wq->size = 0;
	wq->group = NULL;
}

static void idxd_wq_ref_release(struct percpu_ref *ref)
{
	struct idxd_wq *wq = container_of(ref, struct idxd_wq, wq_active);

	complete(&wq->wq_dead);
}

int idxd_wq_init_percpu_ref(struct idxd_wq *wq)
{
	int rc;

	memset(&wq->wq_active, 0, sizeof(wq->wq_active));
	rc = percpu_ref_init(&wq->wq_active, idxd_wq_ref_release,
			     PERCPU_REF_ALLOW_REINIT, GFP_KERNEL);
	if (rc < 0)
		return rc;
	reinit_completion(&wq->wq_dead);
	reinit_completion(&wq->wq_resurrect);
	return 0;
}
EXPORT_SYMBOL_NS_GPL(idxd_wq_init_percpu_ref, "IDXD");

/*
 * Kill wq_active and wait for the last submitter to drop its reference.
 * Every killer runs under wq_lock and waits before it unlocks, so a
 * reference found dying here was killed and waited out by an earlier
 * caller, such as a halt flush that did not revive the queue. Killing
 * it again would drop the count below zero and wait forever. A
 * reference that percpu_ref_exit() released reads as dying too, so a
 * queue the device re-enabled behind an unbind is skipped as well.
 * Returns whether this call killed the reference.
 */
bool idxd_wq_kill(struct idxd_wq *wq)
{
	lockdep_assert_held(&wq->wq_lock);
	if (percpu_ref_is_dying(&wq->wq_active))
		return false;
	reinit_completion(&wq->wq_resurrect);
	percpu_ref_kill(&wq->wq_active);
	wait_for_completion(&wq->wq_dead);
	return true;
}

void __idxd_wq_quiesce(struct idxd_wq *wq)
{
	idxd_wq_kill(wq);
	/* Let parked submitters retry and fail on the dead reference. */
	complete_all(&wq->wq_resurrect);
}
EXPORT_SYMBOL_NS_GPL(__idxd_wq_quiesce, "IDXD");

void idxd_wq_quiesce(struct idxd_wq *wq)
{
	mutex_lock(&wq->wq_lock);
	__idxd_wq_quiesce(wq);
	mutex_unlock(&wq->wq_lock);
}
EXPORT_SYMBOL_NS_GPL(idxd_wq_quiesce, "IDXD");

/* Device control bits */
static inline bool idxd_is_enabled(struct idxd_device *idxd)
{
	union gensts_reg gensts;

	gensts.bits = ioread32(idxd->reg_base + IDXD_GENSTATS_OFFSET);

	if (gensts.state == IDXD_DEVICE_STATE_ENABLED)
		return true;
	return false;
}

static inline bool idxd_device_is_halted(struct idxd_device *idxd)
{
	union gensts_reg gensts;

	gensts.bits = ioread32(idxd->reg_base + IDXD_GENSTATS_OFFSET);

	return (gensts.state == IDXD_DEVICE_STATE_HALT);
}

/*
 * This is function is only used for reset during probe and will
 * poll for completion. Once the device is setup with interrupts,
 * all commands will be done via interrupt completion.
 */
int idxd_device_init_reset(struct idxd_device *idxd)
{
	struct device *dev = &idxd->pdev->dev;
	union idxd_command_reg cmd;

	if (idxd_device_is_halted(idxd)) {
		dev_warn(&idxd->pdev->dev, "Device is HALTED!\n");
		return -ENXIO;
	}

	memset(&cmd, 0, sizeof(cmd));
	cmd.cmd = IDXD_CMD_RESET_DEVICE;
	dev_dbg(dev, "%s: sending reset for init.\n", __func__);
	spin_lock(&idxd->cmd_lock);
	iowrite32(cmd.bits, idxd->reg_base + IDXD_CMD_OFFSET);

	while (ioread32(idxd->reg_base + IDXD_CMDSTS_OFFSET) &
	       IDXD_CMDSTS_ACTIVE)
		cpu_relax();
	spin_unlock(&idxd->cmd_lock);
	return 0;
}

static void idxd_cmd_exec(struct idxd_device *idxd, int cmd_code, u32 operand,
			  u32 *status)
{
	union idxd_command_reg cmd;
	DECLARE_COMPLETION_ONSTACK(done);
	u32 stat;
	unsigned long flags, left;

	if (idxd_device_is_halted(idxd)) {
		dev_warn(&idxd->pdev->dev, "Device is HALTED!\n");
		if (status)
			*status = IDXD_CMDSTS_HW_ERR;
		return;
	}

	if (test_bit(IDXD_FLAG_CMD_TIMEDOUT, &idxd->flags)) {
		dev_warn(&idxd->pdev->dev,
			 "Not sending command %#x: an earlier device command timed out, device needs a reset\n",
			 cmd_code);
		if (status)
			*status = IDXD_CMDSTS_HW_ERR;
		return;
	}

	memset(&cmd, 0, sizeof(cmd));
	cmd.cmd = cmd_code;
	cmd.operand = operand;
	cmd.int_req = 1;

	spin_lock_irqsave(&idxd->cmd_lock, flags);
	/*
	 * Wait for the running command, if any. Only its owner can tell
	 * that it is stuck, so this wait does not declare a timeout of its
	 * own: its budget covers one owner and restarts whenever another
	 * command starts running, and it ends early when the owner gives
	 * up and marks the device. Several healthy commands queued behind
	 * one another therefore never add up to a false timeout.
	 */
	while (test_bit(IDXD_FLAG_CMD_RUNNING, &idxd->flags)) {
		u32 seq = idxd->cmd_seq;

		if (test_bit(IDXD_FLAG_CMD_TIMEDOUT, &idxd->flags)) {
			spin_unlock_irqrestore(&idxd->cmd_lock, flags);
			dev_err(&idxd->pdev->dev,
				"not sending command %#x: the running device command timed out, device needs a reset\n",
				cmd_code);
			if (status)
				*status = IDXD_CMDSTS_HW_ERR;
			return;
		}
		if (!wait_event_lock_irq_timeout(idxd->cmd_waitq,
				!test_bit(IDXD_FLAG_CMD_RUNNING, &idxd->flags) ||
				test_bit(IDXD_FLAG_CMD_TIMEDOUT, &idxd->flags) ||
				idxd->cmd_seq != seq,
				idxd->cmd_lock,
				msecs_to_jiffies(IDXD_CMD_TIMEOUT_MS))) {
			/*
			 * The owner started before this wait and has the
			 * same budget, so its own timeout is due; it marks
			 * the device and wakes this queue. Give up without
			 * marking anything here.
			 */
			spin_unlock_irqrestore(&idxd->cmd_lock, flags);
			dev_err(&idxd->pdev->dev,
				"timed out waiting for the running device command before %#x\n",
				cmd_code);
			if (status)
				*status = IDXD_CMDSTS_HW_ERR;
			return;
		}
	}

	dev_dbg(&idxd->pdev->dev, "%s: sending cmd: %#x op: %#x\n",
		__func__, cmd_code, operand);

	idxd->cmd_status = 0;
	__set_bit(IDXD_FLAG_CMD_RUNNING, &idxd->flags);
	idxd->cmd_seq++;
	idxd->cmd_done = &done;
	iowrite32(cmd.bits, idxd->reg_base + IDXD_CMD_OFFSET);

	/*
	 * After command submitted, release lock and go to sleep until
	 * the command completes via interrupt.
	 */
	spin_unlock_irqrestore(&idxd->cmd_lock, flags);
	left = wait_for_completion_timeout(&done,
				msecs_to_jiffies(IDXD_CMD_TIMEOUT_MS));
	spin_lock(&idxd->cmd_lock);
	/*
	 * A reset, or the recovery from a halt, forgot this command
	 * meanwhile, and another may be running by now. Forgetting the
	 * command completes the wait, but it can also happen between the
	 * interrupt's completion and this lock, or after the wait ran out,
	 * so ownership is checked whichever way the wait ended. The device
	 * is not stuck on this command, and the command state belongs to
	 * whatever runs now. Mark nothing and leave that state alone, even
	 * if this command completed just before the reset or the halt: its
	 * status can no longer be trusted.
	 */
	if (idxd->cmd_done != &done) {
		spin_unlock(&idxd->cmd_lock);
		dev_err(&idxd->pdev->dev,
			"device command %#x was ended by a reset\n",
			cmd_code);
		if (status)
			*status = IDXD_CMDSTS_HW_ERR;
		return;
	}
	/* A timeout, unless the interrupt beat the lock. */
	if (!left && !completion_done(&done)) {
		/*
		 * The device did not complete the command. Detach the
		 * on-stack completion so a late interrupt cannot touch it,
		 * and leave IDXD_FLAG_CMD_RUNNING set: the device may still
		 * be executing, so no further commands may be issued.
		 * IDXD_FLAG_CMD_TIMEDOUT makes waiters and future callers
		 * fail fast; a late completion interrupt or a device reset
		 * clears both flags.
		 */
		idxd->cmd_done = NULL;
		set_bit(IDXD_FLAG_CMD_TIMEDOUT, &idxd->flags);
		wake_up(&idxd->cmd_waitq);
		spin_unlock(&idxd->cmd_lock);
		dev_err(&idxd->pdev->dev,
			"device command %#x timed out, device needs a reset\n",
			cmd_code);
		if (status)
			*status = IDXD_CMDSTS_HW_ERR;
		return;
	}
	/*
	 * Read the status while this command still owns the register: no
	 * other command is written before RUNNING is cleared below.
	 */
	stat = ioread32(idxd->reg_base + IDXD_CMDSTS_OFFSET);
	idxd->cmd_done = NULL;
	if (status)
		*status = stat;
	idxd->cmd_status = stat & GENMASK(7, 0);

	__clear_bit(IDXD_FLAG_CMD_RUNNING, &idxd->flags);
	/* Wake up other pending commands */
	wake_up(&idxd->cmd_waitq);
	spin_unlock(&idxd->cmd_lock);
}

/*
 * Forget the running command, whether its owner still waits for it or
 * has given up on it. Called when the device has been reset or has
 * halted, so the command can no longer be executing. A waiting owner is
 * woken instead of sleeping out its timeout. It finds that it no longer
 * owns the command and returns without touching the command state,
 * which belongs to whatever command runs next.
 */
void idxd_cmd_reset_state(struct idxd_device *idxd)
{
	unsigned long flags;

	spin_lock_irqsave(&idxd->cmd_lock, flags);
	/*
	 * The completion is on the owner's stack. It is valid while
	 * cmd_done points to it, since the owner clears cmd_done, or sees
	 * that it points elsewhere, under cmd_lock before it returns.
	 */
	if (idxd->cmd_done)
		complete(idxd->cmd_done);
	idxd->cmd_done = NULL;
	__clear_bit(IDXD_FLAG_CMD_RUNNING, &idxd->flags);
	clear_bit(IDXD_FLAG_CMD_TIMEDOUT, &idxd->flags);
	wake_up(&idxd->cmd_waitq);
	spin_unlock_irqrestore(&idxd->cmd_lock, flags);
}

/*
 * Hold device commands off while the re-probe after a reset frees the
 * interrupts and requests them again. A command whose completion
 * interrupt comes while the misc vector is gone is never completed, and
 * its owner would time out and fence the device that was just reset.
 * The running command, if any, is waited for first: its owner gives up
 * within its timeout, and the misc vector still takes its interrupt.
 * RUNNING is then held without an owner, and new commands wait for it
 * as for any running command. Returns false, holding nothing, when a
 * command timed out, since commands then fail fast anyway.
 */
bool idxd_cmd_hold(struct idxd_device *idxd, u32 *seq)
{
	bool held = false;

	spin_lock_irq(&idxd->cmd_lock);
	wait_event_lock_irq(idxd->cmd_waitq,
			    !test_bit(IDXD_FLAG_CMD_RUNNING, &idxd->flags) ||
			    test_bit(IDXD_FLAG_CMD_TIMEDOUT, &idxd->flags),
			    idxd->cmd_lock);
	if (!test_bit(IDXD_FLAG_CMD_RUNNING, &idxd->flags)) {
		__set_bit(IDXD_FLAG_CMD_RUNNING, &idxd->flags);
		*seq = ++idxd->cmd_seq;
		held = true;
	}
	spin_unlock_irq(&idxd->cmd_lock);
	return held;
}

/*
 * End a hold taken by idxd_cmd_hold(). A halt's recovery may have
 * forgotten the hold meanwhile and let another command run, which then
 * has an owner or a newer sequence number, and its state is left alone.
 */
void idxd_cmd_unhold(struct idxd_device *idxd, u32 seq)
{
	spin_lock_irq(&idxd->cmd_lock);
	if (idxd->cmd_seq == seq && !idxd->cmd_done) {
		__clear_bit(IDXD_FLAG_CMD_RUNNING, &idxd->flags);
		wake_up(&idxd->cmd_waitq);
	}
	spin_unlock_irq(&idxd->cmd_lock);
}

int idxd_device_enable(struct idxd_device *idxd)
{
	struct device *dev = &idxd->pdev->dev;
	u32 status;

	if (idxd_is_enabled(idxd)) {
		dev_dbg(dev, "Device already enabled\n");
		return -ENXIO;
	}

	idxd_cmd_exec(idxd, IDXD_CMD_ENABLE_DEVICE, 0, &status);

	/* If the command is successful or if the device was enabled */
	if (status != IDXD_CMDSTS_SUCCESS &&
	    status != IDXD_CMDSTS_ERR_DEV_ENABLED) {
		dev_dbg(dev, "%s: err_code: %#x\n", __func__, status);
		return -ENXIO;
	}

	idxd->state = IDXD_DEV_ENABLED;
	return 0;
}

int idxd_device_disable(struct idxd_device *idxd)
{
	struct device *dev = &idxd->pdev->dev;
	u32 status;

	if (!idxd_is_enabled(idxd)) {
		dev_dbg(dev, "Device is not enabled\n");
		return 0;
	}

	idxd_cmd_exec(idxd, IDXD_CMD_DISABLE_DEVICE, 0, &status);

	/*
	 * The command succeeded, or the device was already disabled. The
	 * error code is a value, not a bit mask, so compare it whole. A
	 * refused command reports IDXD_CMDSTS_HW_ERR, which shares a bit
	 * with the already-disabled code but leaves the device enabled,
	 * and clearing the software state of an enabled device lets a
	 * later teardown free memory the device still writes.
	 */
	if (status != IDXD_CMDSTS_SUCCESS &&
	    (status & IDXD_CMDSTS_ERR_MASK) != IDXD_CMDSTS_ERR_DIS_DEV_EN) {
		dev_dbg(dev, "%s: err_code: %#x\n", __func__, status);
		return -ENXIO;
	}

	idxd_device_clear_state(idxd);
	return 0;
}

/*
 * Returns -ENXIO when the device refused or never ran the reset. The
 * software state is cleared only after a reset that ran, since clearing
 * it for an enabled device lets a later teardown free memory the device
 * still writes.
 */
int idxd_device_reset(struct idxd_device *idxd)
{
	u32 status;

	idxd_cmd_exec(idxd, IDXD_CMD_RESET_DEVICE, 0, &status);
	if (status != IDXD_CMDSTS_SUCCESS) {
		dev_dbg(&idxd->pdev->dev, "%s: err_code: %#x\n", __func__, status);
		return -ENXIO;
	}
	idxd_device_clear_state(idxd);
	spin_lock(&idxd->dev_lock);
	idxd_unmask_error_interrupts(idxd);
	spin_unlock(&idxd->dev_lock);
	return 0;
}

/* Returns -ETIMEDOUT when the drain was never run or never completed. */
int idxd_device_drain_pasid(struct idxd_device *idxd, int pasid)
{
	struct device *dev = &idxd->pdev->dev;
	u32 operand, status;

	operand = pasid;
	dev_dbg(dev, "cmd: %u operand: %#x\n", IDXD_CMD_DRAIN_PASID, operand);
	idxd_cmd_exec(idxd, IDXD_CMD_DRAIN_PASID, operand, &status);
	if (idxd_cmd_drain_result(idxd, status))
		return -ETIMEDOUT;
	dev_dbg(dev, "pasid %d drained\n", pasid);
	return 0;
}

int idxd_device_request_int_handle(struct idxd_device *idxd, int idx, int *handle,
				   enum idxd_interrupt_type irq_type)
{
	struct device *dev = &idxd->pdev->dev;
	u32 operand, status;

	if (!(idxd->hw.cmd_cap & BIT(IDXD_CMD_REQUEST_INT_HANDLE)))
		return -EOPNOTSUPP;

	dev_dbg(dev, "get int handle, idx %d\n", idx);

	operand = idx & GENMASK(15, 0);
	if (irq_type == IDXD_IRQ_IMS)
		operand |= CMD_INT_HANDLE_IMS;

	dev_dbg(dev, "cmd: %u operand: %#x\n", IDXD_CMD_REQUEST_INT_HANDLE, operand);

	idxd_cmd_exec(idxd, IDXD_CMD_REQUEST_INT_HANDLE, operand, &status);

	if ((status & IDXD_CMDSTS_ERR_MASK) != IDXD_CMDSTS_SUCCESS) {
		dev_dbg(dev, "request int handle failed: %#x\n", status);
		return -ENXIO;
	}

	*handle = (status >> IDXD_CMDSTS_RES_SHIFT) & GENMASK(15, 0);

	dev_dbg(dev, "int handle acquired: %u\n", *handle);
	return 0;
}

int idxd_device_release_int_handle(struct idxd_device *idxd, int handle,
				   enum idxd_interrupt_type irq_type)
{
	struct device *dev = &idxd->pdev->dev;
	u32 operand, status;

	if (!(idxd->hw.cmd_cap & BIT(IDXD_CMD_RELEASE_INT_HANDLE)))
		return -EOPNOTSUPP;

	dev_dbg(dev, "release int handle, handle %d\n", handle);

	operand = handle & GENMASK(15, 0);

	if (irq_type == IDXD_IRQ_IMS)
		operand |= CMD_INT_HANDLE_IMS;

	dev_dbg(dev, "cmd: %u operand: %#x\n", IDXD_CMD_RELEASE_INT_HANDLE, operand);

	idxd_cmd_exec(idxd, IDXD_CMD_RELEASE_INT_HANDLE, operand, &status);

	if ((status & IDXD_CMDSTS_ERR_MASK) != IDXD_CMDSTS_SUCCESS) {
		dev_dbg(dev, "release int handle failed: %#x\n", status);
		return -ENXIO;
	}

	dev_dbg(dev, "int handle released.\n");
	return 0;
}

/* Device configuration bits */
static void idxd_engines_clear_state(struct idxd_device *idxd)
{
	struct idxd_engine *engine;
	int i;

	lockdep_assert_held(&idxd->dev_lock);
	for (i = 0; i < idxd->max_engines; i++) {
		engine = idxd->engines[i];
		engine->group = NULL;
	}
}

static void idxd_groups_clear_state(struct idxd_device *idxd)
{
	struct idxd_group *group;
	int i;

	lockdep_assert_held(&idxd->dev_lock);
	for (i = 0; i < idxd->max_groups; i++) {
		group = idxd->groups[i];
		memset(&group->grpcfg, 0, sizeof(group->grpcfg));
		group->num_engines = 0;
		group->num_wqs = 0;
		group->use_rdbuf_limit = false;
		/*
		 * The default value is the same as the value of
		 * total read buffers in GRPCAP.
		 */
		group->rdbufs_allowed = idxd->max_rdbufs;
		group->rdbufs_reserved = 0;
		if (idxd->hw.version <= DEVICE_VERSION_2 && !tc_override) {
			group->tc_a = 1;
			group->tc_b = 1;
		} else {
			group->tc_a = -1;
			group->tc_b = -1;
		}
		group->desc_progress_limit = 0;
		group->batch_progress_limit = 0;
	}
}

static void idxd_device_wqs_clear_state(struct idxd_device *idxd)
{
	int i;

	for (i = 0; i < idxd->max_wqs; i++) {
		struct idxd_wq *wq = idxd->wqs[i];

		mutex_lock(&wq->wq_lock);
		idxd_wq_disable_cleanup(wq);
		idxd_wq_device_reset_cleanup(wq);
		mutex_unlock(&wq->wq_lock);
	}
}

void idxd_device_clear_state(struct idxd_device *idxd)
{
	/* IDXD is always disabled. Other states are cleared only when IDXD is configurable. */
	if (test_bit(IDXD_FLAG_CONFIGURABLE, &idxd->flags)) {
		/*
		 * Clearing wq state is protected by wq lock.
		 * So no need to be protected by device lock.
		 */
		idxd_device_wqs_clear_state(idxd);

		spin_lock(&idxd->dev_lock);
		idxd_groups_clear_state(idxd);
		idxd_engines_clear_state(idxd);
	} else {
		spin_lock(&idxd->dev_lock);
	}

	idxd->state = IDXD_DEV_DISABLED;
	spin_unlock(&idxd->dev_lock);
}

static int idxd_device_evl_setup(struct idxd_device *idxd)
{
	union gencfg_reg gencfg;
	union evlcfg_reg evlcfg;
	union genctrl_reg genctrl;
	struct device *dev = &idxd->pdev->dev;
	void *addr;
	dma_addr_t dma_addr;
	int size;
	struct idxd_evl *evl = idxd->evl;
	unsigned long *bmap;
	int rc;

	if (!evl)
		return 0;

	size = evl_size(idxd);

	bmap = bitmap_zalloc(size, GFP_KERNEL);
	if (!bmap) {
		rc = -ENOMEM;
		goto err_bmap;
	}

	/*
	 * Address needs to be page aligned. However, dma_alloc_coherent() provides
	 * at minimal page size aligned address. No manual alignment required.
	 */
	addr = dma_alloc_coherent(dev, size, &dma_addr, GFP_KERNEL);
	if (!addr) {
		rc = -ENOMEM;
		goto err_alloc;
	}

	mutex_lock(&evl->lock);
	evl->log = addr;
	evl->dma = dma_addr;
	evl->log_size = size;
	evl->bmap = bmap;

	memset(&evlcfg, 0, sizeof(evlcfg));
	evlcfg.bits[0] = dma_addr & GENMASK(63, 12);
	evlcfg.size = evl->size;

	iowrite64(evlcfg.bits[0], idxd->reg_base + IDXD_EVLCFG_OFFSET);
	iowrite64(evlcfg.bits[1], idxd->reg_base + IDXD_EVLCFG_OFFSET + 8);

	genctrl.bits = ioread32(idxd->reg_base + IDXD_GENCTRL_OFFSET);
	genctrl.evl_int_en = 1;
	iowrite32(genctrl.bits, idxd->reg_base + IDXD_GENCTRL_OFFSET);

	gencfg.bits = ioread32(idxd->reg_base + IDXD_GENCFG_OFFSET);
	gencfg.evl_en = 1;
	iowrite32(gencfg.bits, idxd->reg_base + IDXD_GENCFG_OFFSET);

	mutex_unlock(&evl->lock);
	return 0;

err_alloc:
	bitmap_free(bmap);
err_bmap:
	return rc;
}

static void idxd_device_evl_free(struct idxd_device *idxd)
{
	void *evl_log;
	unsigned int evl_log_size;
	dma_addr_t evl_dma;
	union gencfg_reg gencfg;
	union genctrl_reg genctrl;
	struct device *dev = &idxd->pdev->dev;
	struct idxd_evl *evl = idxd->evl;

	if (!evl)
		return;

	mutex_lock(&evl->lock);
	gencfg.evl_en = 0;
	iowrite32(gencfg.bits, idxd->reg_base + IDXD_GENCFG_OFFSET);

	genctrl.bits = ioread32(idxd->reg_base + IDXD_GENCTRL_OFFSET);
	genctrl.evl_int_en = 0;
	iowrite32(genctrl.bits, idxd->reg_base + IDXD_GENCTRL_OFFSET);

	iowrite64(0, idxd->reg_base + IDXD_EVLCFG_OFFSET);
	iowrite64(0, idxd->reg_base + IDXD_EVLCFG_OFFSET + 8);

	bitmap_free(evl->bmap);
	evl_log = evl->log;
	evl_log_size = evl->log_size;
	evl_dma = evl->dma;
	evl->log = NULL;
	evl->size = IDXD_EVL_SIZE_MIN;
	mutex_unlock(&evl->lock);

	dma_free_coherent(dev, evl_log_size, evl_log, evl_dma);
}

static void idxd_group_config_write(struct idxd_group *group)
{
	struct idxd_device *idxd = group->idxd;
	struct device *dev = &idxd->pdev->dev;
	int i;
	u32 grpcfg_offset;

	dev_dbg(dev, "Writing group %d cfg registers\n", group->id);

	/* setup GRPWQCFG */
	for (i = 0; i < GRPWQCFG_STRIDES; i++) {
		grpcfg_offset = GRPWQCFG_OFFSET(idxd, group->id, i);
		iowrite64(group->grpcfg.wqs[i], idxd->reg_base + grpcfg_offset);
		dev_dbg(dev, "GRPCFG wq[%d:%d: %#x]: %#llx\n",
			group->id, i, grpcfg_offset,
			ioread64(idxd->reg_base + grpcfg_offset));
	}

	/* setup GRPENGCFG */
	grpcfg_offset = GRPENGCFG_OFFSET(idxd, group->id);
	iowrite64(group->grpcfg.engines, idxd->reg_base + grpcfg_offset);
	dev_dbg(dev, "GRPCFG engs[%d: %#x]: %#llx\n", group->id,
		grpcfg_offset, ioread64(idxd->reg_base + grpcfg_offset));

	/* setup GRPFLAGS */
	grpcfg_offset = GRPFLGCFG_OFFSET(idxd, group->id);
	iowrite64(group->grpcfg.flags.bits, idxd->reg_base + grpcfg_offset);
	dev_dbg(dev, "GRPFLAGS flags[%d: %#x]: %#llx\n",
		group->id, grpcfg_offset,
		ioread64(idxd->reg_base + grpcfg_offset));
}

static int idxd_groups_config_write(struct idxd_device *idxd)

{
	union gencfg_reg reg;
	int i;
	struct device *dev = &idxd->pdev->dev;

	/* Setup bandwidth rdbuf limit */
	if (idxd->hw.gen_cap.config_en && idxd->rdbuf_limit) {
		reg.bits = ioread32(idxd->reg_base + IDXD_GENCFG_OFFSET);
		reg.rdbuf_limit = idxd->rdbuf_limit;
		iowrite32(reg.bits, idxd->reg_base + IDXD_GENCFG_OFFSET);
	}

	dev_dbg(dev, "GENCFG(%#x): %#x\n", IDXD_GENCFG_OFFSET,
		ioread32(idxd->reg_base + IDXD_GENCFG_OFFSET));

	for (i = 0; i < idxd->max_groups; i++) {
		struct idxd_group *group = idxd->groups[i];

		idxd_group_config_write(group);
	}

	return 0;
}

static bool idxd_device_pasid_priv_enabled(struct idxd_device *idxd)
{
	struct pci_dev *pdev = idxd->pdev;

	if (pdev->pasid_enabled && (pdev->pasid_features & PCI_PASID_CAP_PRIV))
		return true;
	return false;
}

static int idxd_wq_config_write(struct idxd_wq *wq)
{
	struct idxd_device *idxd = wq->idxd;
	struct device *dev = &idxd->pdev->dev;
	u32 wq_offset;
	int i, n;

	if (!wq->group)
		return 0;

	/*
	 * Instead of memset the entire shadow copy of WQCFG, copy from the hardware after
	 * wq reset. This will copy back the sticky values that are present on some devices.
	 */
	for (i = 0; i < WQCFG_STRIDES(idxd); i++) {
		wq_offset = WQCFG_OFFSET(idxd, wq->id, i);
		wq->wqcfg->bits[i] |= ioread32(idxd->reg_base + wq_offset);
	}

	if (wq->size == 0 && wq->type != IDXD_WQT_NONE)
		wq->size = WQ_DEFAULT_QUEUE_DEPTH;

	/* byte 0-3 */
	wq->wqcfg->wq_size = wq->size;

	/* bytes 4-7 */
	wq->wqcfg->wq_thresh = wq->threshold;

	/* byte 8-11 */
	if (wq_dedicated(wq))
		wq->wqcfg->mode = 1;

	/*
	 * The WQ priv bit is set depending on the WQ type. priv = 1 if the
	 * WQ type is kernel to indicate privileged access. This setting only
	 * matters for dedicated WQ. According to the DSA spec:
	 * If the WQ is in dedicated mode, WQ PASID Enable is 1, and the
	 * Privileged Mode Enable field of the PCI Express PASID capability
	 * is 0, this field must be 0.
	 *
	 * In the case of a dedicated kernel WQ that is not able to support
	 * the PASID cap, then the configuration will be rejected.
	 */
	if (wq_dedicated(wq) && wq->wqcfg->pasid_en &&
	    !idxd_device_pasid_priv_enabled(idxd) &&
	    wq->type == IDXD_WQT_KERNEL) {
		idxd->cmd_status = IDXD_SCMD_WQ_NO_PRIV;
		return -EOPNOTSUPP;
	}

	wq->wqcfg->priority = wq->priority;

	if (idxd->hw.gen_cap.block_on_fault &&
	    test_bit(WQ_FLAG_BLOCK_ON_FAULT, &wq->flags) &&
	    !test_bit(WQ_FLAG_PRS_DISABLE, &wq->flags))
		wq->wqcfg->bof = 1;

	if (idxd->hw.wq_cap.wq_ats_support)
		wq->wqcfg->wq_ats_disable = test_bit(WQ_FLAG_ATS_DISABLE, &wq->flags);

	if (idxd->hw.wq_cap.wq_prs_support)
		wq->wqcfg->wq_prs_disable = test_bit(WQ_FLAG_PRS_DISABLE, &wq->flags);

	/* bytes 12-15 */
	wq->wqcfg->max_xfer_shift = ilog2(wq->max_xfer_bytes);
	idxd_wqcfg_set_max_batch_shift(idxd->data->type, wq->wqcfg, ilog2(wq->max_batch_size));
	if (idxd_sgl_supported(idxd))
		wq->wqcfg->max_sgl_shift = ilog2(wq->max_sgl_size);

	/* bytes 32-63 */
	if (idxd->hw.wq_cap.op_config && wq->opcap_bmap) {
		memset(wq->wqcfg->op_config, 0, IDXD_MAX_OPCAP_BITS / 8);
		for_each_set_bit(n, wq->opcap_bmap, IDXD_MAX_OPCAP_BITS) {
			int pos = n % BITS_PER_LONG_LONG;
			int idx = n / BITS_PER_LONG_LONG;

			wq->wqcfg->op_config[idx] |= BIT(pos);
		}
	}

	dev_dbg(dev, "WQ %d CFGs\n", wq->id);
	for (i = 0; i < WQCFG_STRIDES(idxd); i++) {
		wq_offset = WQCFG_OFFSET(idxd, wq->id, i);
		iowrite32(wq->wqcfg->bits[i], idxd->reg_base + wq_offset);
		dev_dbg(dev, "WQ[%d][%d][%#x]: %#x\n",
			wq->id, i, wq_offset,
			ioread32(idxd->reg_base + wq_offset));
	}

	return 0;
}

static int idxd_wqs_config_write(struct idxd_device *idxd)
{
	int i, rc;

	for (i = 0; i < idxd->max_wqs; i++) {
		struct idxd_wq *wq = idxd->wqs[i];

		rc = idxd_wq_config_write(wq);
		if (rc < 0)
			return rc;
	}

	return 0;
}

static void idxd_group_flags_setup(struct idxd_device *idxd)
{
	int i;

	/* TC-A 0 and TC-B 1 should be defaults */
	for (i = 0; i < idxd->max_groups; i++) {
		struct idxd_group *group = idxd->groups[i];

		if (group->tc_a == -1)
			group->tc_a = group->grpcfg.flags.tc_a = 0;
		else
			group->grpcfg.flags.tc_a = group->tc_a;
		if (group->tc_b == -1)
			group->tc_b = group->grpcfg.flags.tc_b = 1;
		else
			group->grpcfg.flags.tc_b = group->tc_b;
		group->grpcfg.flags.use_rdbuf_limit = group->use_rdbuf_limit;
		group->grpcfg.flags.rdbufs_reserved = group->rdbufs_reserved;
		group->grpcfg.flags.rdbufs_allowed = group->rdbufs_allowed;
		group->grpcfg.flags.desc_progress_limit = group->desc_progress_limit;
		group->grpcfg.flags.batch_progress_limit = group->batch_progress_limit;
	}
}

static int idxd_engines_setup(struct idxd_device *idxd)
{
	int i, engines = 0;
	struct idxd_engine *eng;
	struct idxd_group *group;

	for (i = 0; i < idxd->max_groups; i++) {
		group = idxd->groups[i];
		group->grpcfg.engines = 0;
	}

	for (i = 0; i < idxd->max_engines; i++) {
		eng = idxd->engines[i];
		group = eng->group;

		if (!group)
			continue;

		group->grpcfg.engines |= BIT(eng->id);
		engines++;
	}

	if (!engines)
		return -EINVAL;

	return 0;
}

static int idxd_wqs_setup(struct idxd_device *idxd)
{
	struct idxd_wq *wq;
	struct idxd_group *group;
	int i, j, configured = 0;
	struct device *dev = &idxd->pdev->dev;

	for (i = 0; i < idxd->max_groups; i++) {
		group = idxd->groups[i];
		for (j = 0; j < 4; j++)
			group->grpcfg.wqs[j] = 0;
	}

	for (i = 0; i < idxd->max_wqs; i++) {
		wq = idxd->wqs[i];
		group = wq->group;

		if (!wq->group)
			continue;

		if (wq_shared(wq) && !wq_shared_supported(wq)) {
			idxd->cmd_status = IDXD_SCMD_WQ_NO_SWQ_SUPPORT;
			dev_warn(dev, "No shared wq support but configured.\n");
			return -EINVAL;
		}

		group->grpcfg.wqs[wq->id / 64] |= BIT(wq->id % 64);
		configured++;
	}

	if (configured == 0) {
		idxd->cmd_status = IDXD_SCMD_WQ_NONE_CONFIGURED;
		return -EINVAL;
	}

	return 0;
}

int idxd_device_config(struct idxd_device *idxd)
{
	int rc;

	guard(spinlock)(&idxd->dev_lock);

	if (!test_bit(IDXD_FLAG_CONFIGURABLE, &idxd->flags))
		return 0;

	rc = idxd_wqs_setup(idxd);
	if (rc < 0)
		return rc;

	rc = idxd_engines_setup(idxd);
	if (rc < 0)
		return rc;

	idxd_group_flags_setup(idxd);

	rc = idxd_wqs_config_write(idxd);
	if (rc < 0)
		return rc;

	rc = idxd_groups_config_write(idxd);
	if (rc < 0)
		return rc;

	return 0;
}

static int idxd_wq_load_config(struct idxd_wq *wq)
{
	struct idxd_device *idxd = wq->idxd;
	struct device *dev = &idxd->pdev->dev;
	int wqcfg_offset;
	int i;

	wqcfg_offset = WQCFG_OFFSET(idxd, wq->id, 0);
	memcpy_fromio(wq->wqcfg, idxd->reg_base + wqcfg_offset, idxd->wqcfg_size);

	wq->size = wq->wqcfg->wq_size;
	wq->threshold = wq->wqcfg->wq_thresh;

	/* The driver does not support shared WQ mode in read-only config yet */
	if (wq->wqcfg->mode == 0 || wq->wqcfg->pasid_en)
		return -EOPNOTSUPP;

	set_bit(WQ_FLAG_DEDICATED, &wq->flags);

	wq->priority = wq->wqcfg->priority;

	wq->max_xfer_bytes = 1ULL << wq->wqcfg->max_xfer_shift;
	idxd_wq_set_max_batch_size(idxd->data->type, wq, 1U << wq->wqcfg->max_batch_shift);
	if (idxd_sgl_supported(idxd))
		wq->max_sgl_size = 1U << wq->wqcfg->max_sgl_shift;

	for (i = 0; i < WQCFG_STRIDES(idxd); i++) {
		wqcfg_offset = WQCFG_OFFSET(idxd, wq->id, i);
		dev_dbg(dev, "WQ[%d][%d][%#x]: %#x\n", wq->id, i, wqcfg_offset, wq->wqcfg->bits[i]);
	}

	return 0;
}

static void idxd_group_load_config(struct idxd_group *group)
{
	struct idxd_device *idxd = group->idxd;
	struct device *dev = &idxd->pdev->dev;
	int i, j, grpcfg_offset;

	/*
	 * Load WQS bit fields
	 * Iterate through all 256 bits 64 bits at a time
	 */
	for (i = 0; i < GRPWQCFG_STRIDES; i++) {
		struct idxd_wq *wq;

		grpcfg_offset = GRPWQCFG_OFFSET(idxd, group->id, i);
		group->grpcfg.wqs[i] = ioread64(idxd->reg_base + grpcfg_offset);
		dev_dbg(dev, "GRPCFG wq[%d:%d: %#x]: %#llx\n",
			group->id, i, grpcfg_offset, group->grpcfg.wqs[i]);

		if (i * 64 >= idxd->max_wqs)
			break;

		/* Iterate through all 64 bits and check for wq set */
		for (j = 0; j < 64; j++) {
			int id = i * 64 + j;

			/* No need to check beyond max wqs */
			if (id >= idxd->max_wqs)
				break;

			/* Set group assignment for wq if wq bit is set */
			if (group->grpcfg.wqs[i] & BIT(j)) {
				wq = idxd->wqs[id];
				wq->group = group;
			}
		}
	}

	grpcfg_offset = GRPENGCFG_OFFSET(idxd, group->id);
	group->grpcfg.engines = ioread64(idxd->reg_base + grpcfg_offset);
	dev_dbg(dev, "GRPCFG engs[%d: %#x]: %#llx\n", group->id,
		grpcfg_offset, group->grpcfg.engines);

	/* Iterate through all 64 bits to check engines set */
	for (i = 0; i < 64; i++) {
		if (i >= idxd->max_engines)
			break;

		if (group->grpcfg.engines & BIT(i)) {
			struct idxd_engine *engine = idxd->engines[i];

			engine->group = group;
		}
	}

	grpcfg_offset = GRPFLGCFG_OFFSET(idxd, group->id);
	group->grpcfg.flags.bits = ioread64(idxd->reg_base + grpcfg_offset);
	dev_dbg(dev, "GRPFLAGS flags[%d: %#x]: %#llx\n",
		group->id, grpcfg_offset, group->grpcfg.flags.bits);
}

int idxd_device_load_config(struct idxd_device *idxd)
{
	union gencfg_reg reg;
	int i, rc;

	reg.bits = ioread32(idxd->reg_base + IDXD_GENCFG_OFFSET);
	idxd->rdbuf_limit = reg.rdbuf_limit;

	for (i = 0; i < idxd->max_groups; i++) {
		struct idxd_group *group = idxd->groups[i];

		idxd_group_load_config(group);
	}

	for (i = 0; i < idxd->max_wqs; i++) {
		struct idxd_wq *wq = idxd->wqs[i];

		rc = idxd_wq_load_config(wq);
		if (rc < 0)
			return rc;
	}

	return 0;
}

static void idxd_flush_pending_descs(struct idxd_irq_entry *ie,
				     bool keep_callbacks, bool quarantine)
{
	struct idxd_wq *wq = ie_to_wq(ie);
	struct idxd_desc *desc, *itr;
	struct llist_node *head;
	LIST_HEAD(flist);
	LIST_HEAD(qlist);
	enum idxd_complete_type ctype;

	spin_lock(&ie->list_lock);
	head = llist_del_all(&ie->pending_llist);
	if (head) {
		llist_for_each_entry_safe(desc, itr, head, llnode)
			list_add_tail(&desc->list, &ie->work_list);
	}

	/*
	 * A halt flush that delivers the callbacks of a dmaengine queue
	 * leaves its descriptors to the interrupt thread, which runs them
	 * without wq_lock, as it runs every completion. A callback may
	 * terminate the channel, which takes that lock, and the halt flush
	 * holds it. The halted device writes no record any more, so one it
	 * never finished is marked aborted for the thread to complete.
	 */
	if (keep_callbacks && idxd_wq_has_txd(wq)) {
		bool wake = false;

		list_for_each_entry(desc, &ie->work_list, list) {
			if (!desc->completion->status)
				desc->completion->status = IDXD_COMP_DESC_ABORT;
			wake = true;
		}
		spin_unlock(&ie->list_lock);
		if (wake)
			irq_wake_thread(ie->vector, ie);
		return;
	}

	list_for_each_entry_safe(desc, itr, &ie->work_list, list)
		list_move_tail(&desc->list, &flist);
	spin_unlock(&ie->list_lock);

	list_for_each_entry_safe(desc, itr, &flist, list) {
		struct dma_async_tx_descriptor *tx;

		list_del(&desc->list);
		ctype = desc->completion->status ? IDXD_COMPLETE_NORMAL : IDXD_COMPLETE_ABORT;
		/*
		 * Another driver's descriptor carries that driver's request
		 * in place of a txd, and a client may still wait for it, so it
		 * goes back through that driver's completion. The completion
		 * reads the outcome from the record, which the device never
		 * wrote for an unfinished descriptor. Only a dmaengine channel
		 * terminates, so nothing of such a queue is quarantined.
		 */
		if (!idxd_wq_has_txd(wq)) {
			if (ctype == IDXD_COMPLETE_ABORT)
				desc->completion->status = IDXD_COMP_DESC_ABORT;
			idxd_desc_complete(desc, ctype, true);
			continue;
		}
		/*
		 * wq is being disabled or terminated. Any remaining
		 * descriptors are likely to be stuck and can be dropped.
		 * With no client left the callback could point to code that
		 * is no longer accessible (dmatest unloaded), and a client's
		 * own terminate must not get it, so it is stripped. A halt
		 * that delivers the callbacks left them to the thread above.
		 */
		tx = &desc->txd;
		tx->callback = NULL;
		tx->callback_result = NULL;
		/*
		 * A descriptor the device has not finished still has its
		 * completion record to write. A flush that leaves the
		 * device running keeps its slot out of use, or the record
		 * would land on the slot's next owner and complete it with
		 * this descriptor's status. Its unmap data stays with the
		 * slot as well, since the device may still access what it
		 * maps, and an address released now could be mapped again
		 * for someone else before the late access.
		 */
		if (quarantine && ctype == IDXD_COMPLETE_ABORT) {
			struct dmaengine_unmap_data *unmap = tx->unmap;

			tx->unmap = NULL;
			idxd_dma_complete_txd(desc, ctype, false, NULL, NULL);
			tx->unmap = unmap;
			list_add_tail(&desc->list, &qlist);
			continue;
		}
		/*
		 * The abort type keeps a record that reports a revoked
		 * interrupt handle from being resubmitted. The device is done
		 * with it, and with the handle kept across a terminate the
		 * copy would run once the client may have freed its memory.
		 */
		idxd_dma_complete_txd(desc, IDXD_COMPLETE_ABORT, true, NULL,
				      NULL);
	}

	if (list_empty(&qlist))
		return;
	spin_lock(&ie->list_lock);
	list_splice_tail(&qlist, &wq->quarantine);
	spin_unlock(&ie->list_lock);
}

/*
 * Take what terminate_all has quarantined so far. A drain covers only
 * what the device held when it was issued, so a synchronize takes the
 * quarantine before it drains and frees it if the drain completed. A
 * descriptor terminated meanwhile waits for the next one. The list lock
 * serialises this with the flush that adds to the quarantine and with a
 * halt or a teardown that frees it all.
 */
void idxd_wq_quarantine_take(struct idxd_wq *wq, struct list_head *list)
{
	spin_lock(&wq->ie.list_lock);
	list_splice_init(&wq->quarantine, list);
	spin_unlock(&wq->ie.list_lock);
}

/* The drain did not complete: the device may still run them. */
void idxd_wq_quarantine_put_back(struct idxd_wq *wq, struct list_head *list)
{
	spin_lock(&wq->ie.list_lock);
	list_splice(list, &wq->quarantine);
	spin_unlock(&wq->ie.list_lock);
}

/*
 * Nothing the device holds can write their completion records, or reach
 * the memory their unmap data maps, any more. The unmap data goes before
 * the slot, which a new owner may take at once.
 */
void idxd_wq_quarantine_free(struct idxd_wq *wq, struct list_head *list)
{
	struct idxd_desc *desc, *itr;

	list_for_each_entry_safe(desc, itr, list, list) {
		list_del(&desc->list);
		dma_descriptor_unmap(&desc->txd);
		idxd_free_desc(wq, desc);
	}
}

/*
 * A halt killed every descriptor the device held: the quarantined slots
 * are free, and the fence a failed drain raised can be lifted. The halt
 * flush calls this under wq_lock, since an unbind racing the halt may
 * free the pool.
 */
void idxd_wq_halt_release(struct idxd_wq *wq)
{
	LIST_HEAD(dead_descs);

	lockdep_assert_held(&wq->wq_lock);

	if (wq->state != IDXD_WQ_ENABLED || wq->type != IDXD_WQT_KERNEL)
		return;

	idxd_wq_quarantine_take(wq, &dead_descs);
	idxd_wq_quarantine_free(wq, &dead_descs);
	WRITE_ONCE(wq->stuck, false);
}

static void idxd_device_set_perm_entry(struct idxd_device *idxd,
				       struct idxd_irq_entry *ie)
{
	union msix_perm mperm;

	if (ie->pasid == IOMMU_PASID_INVALID)
		return;

	mperm.bits = 0;
	mperm.pasid = ie->pasid;
	mperm.pasid_en = 1;
	iowrite32(mperm.bits, idxd->reg_base + idxd->msix_perm_offset + ie->id * 8);
}

static void idxd_device_clear_perm_entry(struct idxd_device *idxd,
					 struct idxd_irq_entry *ie)
{
	iowrite32(0, idxd->reg_base + idxd->msix_perm_offset + ie->id * 8);
}

void idxd_wq_free_irq(struct idxd_wq *wq)
{
	struct idxd_device *idxd = wq->idxd;
	struct idxd_irq_entry *ie = &wq->ie;

	if (wq->type != IDXD_WQT_KERNEL)
		return;

	free_irq(ie->vector, ie);
	idxd_flush_pending_descs(ie, false, false);

	/* FLR revokes every handle; the halt path marks them invalid. */
	if (ie->int_handle == INVALID_INT_HANDLE)
		return;

	/*
	 * The release waits for a running command like any other command.
	 * It is not sent, or does not complete, only on a halted device or
	 * after a command timed out, and the reset such a device needs
	 * revokes the handle. The handle is forgotten in any case.
	 */
	if (idxd->request_int_handles)
		idxd_device_release_int_handle(idxd, ie->int_handle, IDXD_IRQ_MSIX);
	idxd_device_clear_perm_entry(idxd, ie);
	ie->vector = -1;
	ie->int_handle = INVALID_INT_HANDLE;
	ie->pasid = IOMMU_PASID_INVALID;
}

/*
 * @keep_callbacks: deliver the flushed descriptors' completions to the
 * client, aborted, or with their hardware status where the device had
 * already finished them. A client's own terminate_all must not get them
 * (the dmaengine contract); a device halt with a client still holding the
 * channel must, or the client waits for them forever. The interrupt
 * thread delivers them, outside wq_lock.
 * @quarantine: the device keeps running, so the slots of the descriptors
 * it has not finished stay out of use until a drain covers them. A halt
 * has killed them and frees their slots at once.
 * The caller holds wq_lock.
 */
void __idxd_wq_flush_descs(struct idxd_wq *wq, bool keep_callbacks,
			   bool quarantine)
{
	struct idxd_irq_entry *ie = &wq->ie;
	bool killed = false;

	lockdep_assert_held(&wq->wq_lock);

	if (wq->state != IDXD_WQ_ENABLED || wq->type != IDXD_WQT_KERNEL)
		return;

	/*
	 * A submitter between its list add and its portal write holds
	 * wq_active. Waiting for the reference to die puts every listed
	 * descriptor in the device, where the drain that frees the
	 * quarantine covers it. A halt flush has done this itself.
	 */
	if (quarantine)
		killed = idxd_wq_kill(wq);
	idxd_flush_pending_descs(ie, keep_callbacks, quarantine);
	if (killed) {
		percpu_ref_reinit(&wq->wq_active);
		complete_all(&wq->wq_resurrect);
	}
}

void idxd_wq_flush_descs(struct idxd_wq *wq, bool keep_callbacks,
			 bool quarantine)
{
	guard(mutex)(&wq->wq_lock);
	__idxd_wq_flush_descs(wq, keep_callbacks, quarantine);
}

int idxd_wq_request_irq(struct idxd_wq *wq)
{
	struct idxd_device *idxd = wq->idxd;
	struct pci_dev *pdev = idxd->pdev;
	struct device *dev = &pdev->dev;
	struct idxd_irq_entry *ie;
	int rc;

	if (wq->type != IDXD_WQT_KERNEL)
		return 0;

	ie = &wq->ie;
	ie->vector = pci_irq_vector(pdev, ie->id);
	ie->pasid = device_pasid_enabled(idxd) ? idxd->pasid : IOMMU_PASID_INVALID;
	idxd_device_set_perm_entry(idxd, ie);

	rc = request_threaded_irq(ie->vector, NULL, idxd_wq_thread, 0, "idxd-portal", ie);
	if (rc < 0) {
		dev_err(dev, "Failed to request irq %d.\n", ie->vector);
		goto err_irq;
	}

	if (idxd->request_int_handles) {
		rc = idxd_device_request_int_handle(idxd, ie->id, &ie->int_handle,
						    IDXD_IRQ_MSIX);
		if (rc < 0)
			goto err_int_handle;
	} else {
		ie->int_handle = ie->id;
	}

	return 0;

err_int_handle:
	ie->int_handle = INVALID_INT_HANDLE;
	free_irq(ie->vector, ie);
err_irq:
	idxd_device_clear_perm_entry(idxd, ie);
	ie->pasid = IOMMU_PASID_INVALID;
	return rc;
}

int idxd_drv_enable_wq(struct idxd_wq *wq)
{
	struct idxd_device *idxd = wq->idxd;
	struct device *dev = &idxd->pdev->dev;
	int rc = -ENXIO;

	lockdep_assert_held(&wq->wq_lock);

	/*
	 * The device remove releases the queues' drivers before it disables
	 * the device, so the device still reads enabled meanwhile.
	 */
	if (test_bit(IDXD_FLAG_REMOVING, &idxd->flags)) {
		idxd->cmd_status = IDXD_SCMD_DEV_NOT_ENABLED;
		rc = -ENODEV;
		goto err;
	}

	if (idxd->state != IDXD_DEV_ENABLED) {
		idxd->cmd_status = IDXD_SCMD_DEV_NOT_ENABLED;
		goto err;
	}

	if (wq->state != IDXD_WQ_DISABLED) {
		dev_dbg(dev, "wq %d already enabled.\n", wq->id);
		idxd->cmd_status = IDXD_SCMD_WQ_ENABLED;
		rc = -EBUSY;
		goto err;
	}

	if (!wq->group) {
		dev_dbg(dev, "wq %d not attached to group.\n", wq->id);
		idxd->cmd_status = IDXD_SCMD_WQ_NO_GRP;
		goto err;
	}

	if (strlen(wq->name) == 0) {
		idxd->cmd_status = IDXD_SCMD_WQ_NO_NAME;
		dev_dbg(dev, "wq %d name not set.\n", wq->id);
		goto err;
	}

	/* Shared WQ checks */
	if (wq_shared(wq)) {
		if (!wq_shared_supported(wq)) {
			idxd->cmd_status = IDXD_SCMD_WQ_NO_SVM;
			dev_dbg(dev, "PASID not enabled and shared wq.\n");
			goto err;
		}
		/*
		 * Shared wq with the threshold set to 0 means the user
		 * did not set the threshold or transitioned from a
		 * dedicated wq but did not set threshold. A value
		 * of 0 would effectively disable the shared wq. The
		 * driver does not allow a value of 0 to be set for
		 * threshold via sysfs.
		 */
		if (wq->threshold == 0) {
			idxd->cmd_status = IDXD_SCMD_WQ_NO_THRESH;
			dev_dbg(dev, "Shared wq and threshold 0.\n");
			goto err;
		}
	}

	/*
	 * In the event that the WQ is configurable for pasid, the driver
	 * should setup the pasid, pasid_en bit. This is true for both kernel
	 * and user shared workqueues. There is no need to setup priv bit in
	 * that in-kernel DMA will also do user privileged requests.
	 * A dedicated wq that is not 'kernel' type will configure pasid and
	 * pasid_en later on so there is no need to setup.
	 */
	if (test_bit(IDXD_FLAG_CONFIGURABLE, &idxd->flags)) {
		if (wq_pasid_enabled(wq)) {
			if (is_idxd_wq_kernel(wq) || wq_shared(wq)) {
				u32 pasid = wq_dedicated(wq) ? idxd->pasid : 0;

				__idxd_wq_set_pasid_locked(wq, pasid);
			}
		}
	}

	rc = idxd_device_config(idxd);
	if (rc < 0) {
		dev_dbg(dev, "Writing wq %d config failed: %d\n", wq->id, rc);
		goto err;
	}

	rc = idxd_wq_enable(wq);
	if (rc < 0) {
		dev_dbg(dev, "wq %d enabling failed: %d\n", wq->id, rc);
		goto err;
	}

	rc = idxd_wq_map_portal(wq);
	if (rc < 0) {
		idxd->cmd_status = IDXD_SCMD_WQ_PORTAL_ERR;
		dev_dbg(dev, "wq %d portal mapping failed: %d\n", wq->id, rc);
		goto err_map_portal;
	}

	wq->client_count = 0;

	rc = idxd_wq_request_irq(wq);
	if (rc < 0) {
		idxd->cmd_status = IDXD_SCMD_WQ_IRQ_ERR;
		dev_dbg(dev, "WQ %d irq setup failed: %d\n", wq->id, rc);
		goto err_irq;
	}

	rc = idxd_wq_alloc_resources(wq);
	if (rc < 0) {
		idxd->cmd_status = IDXD_SCMD_WQ_RES_ALLOC_ERR;
		dev_dbg(dev, "WQ resource alloc failed\n");
		goto err_res_alloc;
	}

	rc = idxd_wq_init_percpu_ref(wq);
	if (rc < 0) {
		idxd->cmd_status = IDXD_SCMD_PERCPU_ERR;
		dev_dbg(dev, "percpu_ref setup failed\n");
		goto err_ref;
	}

	return 0;

err_ref:
	idxd_wq_free_resources(wq);
err_res_alloc:
	idxd_wq_free_irq(wq);
err_irq:
	idxd_wq_unmap_portal(wq);
err_map_portal:
	if (idxd_wq_disable(wq, false))
		dev_dbg(dev, "wq %s disable failed\n", dev_name(wq_confdev(wq)));
err:
	return rc;
}
EXPORT_SYMBOL_NS_GPL(idxd_drv_enable_wq, "IDXD");

void idxd_drv_disable_wq(struct idxd_wq *wq)
{
	struct idxd_device *idxd = wq->idxd;
	struct device *dev = &idxd->pdev->dev;
	int rc;

	lockdep_assert_held(&wq->wq_lock);

	if (idxd_wq_refcount(wq))
		dev_warn(dev, "Clients has claim on wq %d: %d\n",
			 wq->id, idxd_wq_refcount(wq));

	idxd_wq_unmap_portal(wq);
	rc = idxd_wq_drain(wq);
	idxd_wq_free_irq(wq);
	rc |= idxd_wq_reset(wq);
	/*
	 * A drain or reset that never completed leaves descriptors in the
	 * device, and their completion records live in the memory freed
	 * here. Leak it; the device writing into freed memory is worse.
	 */
	if (rc)
		dev_err(dev, "wq %d: drain or reset timed out, leaking its descriptor memory\n",
			wq->id);
	else
		idxd_wq_free_resources(wq);
	percpu_ref_exit(&wq->wq_active);
	wq->client_count = 0;
}
EXPORT_SYMBOL_NS_GPL(idxd_drv_disable_wq, "IDXD");

int idxd_device_drv_probe(struct idxd_dev *idxd_dev)
{
	struct idxd_device *idxd = idxd_dev_to_idxd(idxd_dev);
	int rc = 0;

	if (test_bit(IDXD_FLAG_REMOVING, &idxd->flags))
		return -ENODEV;

	/*
	 * Device should be in disabled state for the idxd_drv to load. If it's in
	 * enabled state, then the device was altered outside of driver's control.
	 * If the state is in halted state, then we don't want to proceed.
	 */
	if (idxd->state != IDXD_DEV_DISABLED) {
		idxd->cmd_status = IDXD_SCMD_DEV_ENABLED;
		return -ENXIO;
	}

	/* Device configuration */
	rc = idxd_device_config(idxd);
	if (rc < 0)
		return -ENXIO;

	/*
	 * System PASID is preserved across device disable/enable cycle, but
	 * genconfig register content gets cleared during device reset. We
	 * need to re-enable user interrupts for kernel work queue completion
	 * IRQ to function.
	 */
	if (idxd->pasid != IOMMU_PASID_INVALID)
		idxd_set_user_intr(idxd, 1);

	rc = idxd_device_evl_setup(idxd);
	if (rc < 0) {
		idxd->cmd_status = IDXD_SCMD_DEV_EVL_ERR;
		return rc;
	}

	/* Start device */
	rc = idxd_device_enable(idxd);
	if (rc < 0) {
		idxd_device_evl_free(idxd);
		return rc;
	}

	/* Setup DMA device without channels */
	rc = idxd_register_dma_device(idxd);
	if (rc < 0) {
		idxd_device_disable(idxd);
		idxd_device_evl_free(idxd);
		idxd->cmd_status = IDXD_SCMD_DEV_DMA_ERR;
		return rc;
	}

	idxd->cmd_status = 0;
	return 0;
}

void idxd_device_drv_remove(struct idxd_dev *idxd_dev)
{
	struct device *dev = &idxd_dev->conf_dev;
	struct idxd_device *idxd = idxd_dev_to_idxd(idxd_dev);
	bool stopped;
	int i;

	for (i = 0; i < idxd->max_wqs; i++) {
		struct idxd_wq *wq = idxd->wqs[i];
		struct device *wq_dev = wq_confdev(wq);

		/*
		 * A halt's clear_state leaves a bound queue DISABLED while
		 * its driver still holds the queue's channel and resources.
		 * Release every bound driver, not only the enabled queues',
		 * before the dma_device below frees their channels. For a
		 * queue without a driver the release is a no-op.
		 */
		if (wq->state != IDXD_WQ_DISABLED)
			dev_warn(dev, "Active wq %d on disable %s.\n", i,
				 dev_name(wq_dev));
		device_release_driver(wq_dev);
	}

	idxd_unregister_dma_device(idxd);
	/*
	 * A disable or a reset that ran, or found the device disabled or
	 * halted, leaves nothing in flight. Both are refused after a device
	 * command timed out, and the device then stays enabled with whatever
	 * it holds, so what it may still write is kept.
	 */
	stopped = !idxd_device_disable(idxd);
	if (test_bit(IDXD_FLAG_CONFIGURABLE, &idxd->flags) &&
	    !idxd_device_reset(idxd))
		stopped = true;
	if (stopped)
		idxd_device_evl_free(idxd);
	else
		dev_err(dev, "device still enabled after a command timeout, leaking the event log\n");
}

static enum idxd_dev_type dev_types[] = {
	IDXD_DEV_DSA,
	IDXD_DEV_IAX,
	IDXD_DEV_NONE,
};

struct idxd_device_driver idxd_drv = {
	.type = dev_types,
	.probe = idxd_device_drv_probe,
	.remove = idxd_device_drv_remove,
	.name = "idxd",
};
EXPORT_SYMBOL_GPL(idxd_drv);
