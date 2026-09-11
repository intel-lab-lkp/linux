// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2025 Intel Corporation */

#include "ixd.h"
#include "ixd_ctlq.h"
#include "ixd_devlink.h"
#include "ixd_lan_regs.h"
#include "ixd_virtchnl.h"

#define IXD_DFLT_MBX_Q_LEN 64

/**
 * ixd_init_ctlq_create_info - Initialize control queue info for creation
 * @info: destination
 * @type: type of the queue to create
 * @ctlq_reg: register assigned to the control queue
 */
static void ixd_init_ctlq_create_info(struct libie_ctlq_create_info *info,
				      enum libie_ctlq_type type,
				      const struct libie_ctlq_reg *ctlq_reg)
{
	*info = (struct libie_ctlq_create_info) {
		.type = type,
		.id = -1,
		.reg = *ctlq_reg,
		.len = IXD_DFLT_MBX_Q_LEN,
	};
}

/**
 * ixd_init_libie_xn_params - Initialize xn transaction manager creation info
 * @params: destination
 * @adapter: adapter info struct
 * @ctlqs: list of the managed queues to create
 * @num_queues: length of the queue list
 */
static void ixd_init_libie_xn_params(struct libie_ctlq_xn_init_params *params,
				     struct ixd_adapter *adapter,
				      struct libie_ctlq_create_info *ctlqs,
				      uint num_queues)
{
	*params = (struct libie_ctlq_xn_init_params){
		.cctlq_info = ctlqs,
		.ctx = &adapter->cp_ctx,
		.num_qs = num_queues,
	};
}

/**
 * ixd_adapter_fill_dflt_ctlqs - Find default control queues and store them
 * @adapter: adapter info struct
 */
static void ixd_adapter_fill_dflt_ctlqs(struct ixd_adapter *adapter)
{
	adapter->arq = libie_find_ctlq(&adapter->cp_ctx, LIBIE_CTLQ_TYPE_RX,
				       LIBIE_CTLQ_MBX_ID);
	adapter->asq = libie_find_ctlq(&adapter->cp_ctx, LIBIE_CTLQ_TYPE_TX,
				       LIBIE_CTLQ_MBX_ID);
}

static irqreturn_t ixd_mailbox_irq_handler(int __always_unused irq, void *data)
{
	struct ixd_adapter *adapter = (struct ixd_adapter *)data;

	queue_delayed_work(system_dfl_wq, &adapter->mbx_task, 0);

	return IRQ_HANDLED;
}

void ixd_mailbox_irq_enable(struct ixd_adapter *adapter)
{
	writel(PF_GLINT_DYN_CTL_INTENA_M | PF_GLINT_DYN_CTL_ITR_INDX_M,
	       adapter->mb_dyn_ctl);
	writel(PF_INT_DIR_OICR_ENA_M, adapter->oicr_ena);
}

static void ixd_mailbox_irq_disable(struct ixd_adapter *adapter)
{
	/* Clear INTENA, keep the ITR index pointing to the no update one. */
	writel(PF_GLINT_DYN_CTL_ITR_INDX_M, adapter->mb_dyn_ctl);
	writel(0, adapter->oicr_ena);
}

static int ixd_mailbox_irq_regs_init(struct ixd_adapter *adapter)
{
	struct libie_mmio_info *mmio = &adapter->cp_ctx.mmio_info;
	u32 dyn_ctl = le32_to_cpu(adapter->caps.mailbox_dyn_ctl);

	adapter->mb_dyn_ctl = libie_pci_get_mmio_addr(mmio, dyn_ctl);
	adapter->oicr_ena = libie_pci_get_mmio_addr(mmio, PF_INT_DIR_OICR_ENA);

	if (!adapter->mb_dyn_ctl || !adapter->oicr_ena)
		return -EINVAL;

	return 0;
}

static void ixd_mailbox_irq_deinit(struct ixd_adapter *adapter)
{
	if (!test_and_clear_bit(IXD_MB_INTR_MODE, adapter->flags))
		return;

	ixd_mailbox_irq_disable(adapter);

	/* The mailbox task re-arms the interrupt, make sure it isn't running
	 * anymore before the irq line is freed.
	 */
	cancel_delayed_work_sync(&adapter->mbx_task);

	kfree(free_irq(adapter->mb_irq.virq, adapter));
	libie_irq_free(&adapter->irq, adapter->mb_irq);
}

static int ixd_mailbox_irq_init(struct ixd_adapter *adapter)
{
	struct msi_map *irq = &adapter->mb_irq;
	char *name;
	int err;

	*irq = libie_irq_alloc(&adapter->irq, LIBIE_IRQ_STATIC);
	if (irq->index < 0)
		return irq->index;

	name = kasprintf(GFP_KERNEL, "%s-%s-%d",
			 dev_driver_string(ixd_to_dev(adapter)), "Mailbox", 0);
	if (!name) {
		libie_irq_free(&adapter->irq, adapter->mb_irq);

		return -ENOMEM;
	}

	err = request_irq(irq->virq, ixd_mailbox_irq_handler, 0, name, adapter);
	if (err) {
		kfree(name);
		libie_irq_free(&adapter->irq, adapter->mb_irq);
		dev_err(ixd_to_dev(adapter), "IRQ request for mailbox failed, error: %d\n",
			err);

		return err;
	}

	set_bit(IXD_MB_INTR_MODE, adapter->flags);

	return 0;
}

void ixd_deinit_interrupts(struct ixd_adapter *adapter)
{
	ixd_mailbox_irq_deinit(adapter);
	libie_irq_deinit(&adapter->irq);
}

static int ixd_init_interrupts(struct ixd_adapter *adapter)
{
	struct libie_irq *irq = &adapter->irq;
	int err;

	err = libie_irq_init(irq, ixd_to_pdev(adapter), 1, 1);
	if (err)
		return err;

	err = ixd_mailbox_irq_regs_init(adapter);
	if (err)
		goto free_irq;

	err = ixd_mailbox_irq_init(adapter);
	if (err)
		goto free_irq;

	ixd_mailbox_irq_enable(adapter);

	return 0;

free_irq:
	libie_irq_deinit(irq);

	return err;
}

/**
 * ixd_deinit_dflt_mbx - Deinitialize default mailbox
 * @adapter: adapter info struct
 */
void ixd_deinit_dflt_mbx(struct ixd_adapter *adapter)
{
	cancel_delayed_work_sync(&adapter->mbx_task);

	if (adapter->xnm)
		libie_ctlq_xn_shutdown(adapter->xnm);

	if (adapter->asq)
		ixd_ctlq_clean_sq(adapter, true);

	if (adapter->xnm)
		libie_ctlq_xn_deinit(adapter->xnm, &adapter->cp_ctx);

	adapter->arq = NULL;
	adapter->asq = NULL;
	adapter->xnm = NULL;
}

/**
 * ixd_init_dflt_mbx - Setup default mailbox parameters and make request
 * @adapter: adapter info struct
 *
 * Return: %0 on success, negative errno code on failure
 */
int ixd_init_dflt_mbx(struct ixd_adapter *adapter)
{
	struct libie_ctlq_create_info ctlqs_info[2];
	struct libie_ctlq_xn_init_params xn_params;
	struct libie_ctlq_reg ctlq_reg_tx;
	struct libie_ctlq_reg ctlq_reg_rx;
	int err;

	ixd_ctlq_reg_init(adapter, &ctlq_reg_tx, &ctlq_reg_rx);
	ixd_init_ctlq_create_info(&ctlqs_info[0], LIBIE_CTLQ_TYPE_TX,
				  &ctlq_reg_tx);
	ixd_init_ctlq_create_info(&ctlqs_info[1], LIBIE_CTLQ_TYPE_RX,
				  &ctlq_reg_rx);
	ixd_init_libie_xn_params(&xn_params, adapter, ctlqs_info,
				 ARRAY_SIZE(ctlqs_info));
	err = libie_ctlq_xn_init(&xn_params);
	if (err)
		return err;
	adapter->xnm = xn_params.xnm;

	ixd_adapter_fill_dflt_ctlqs(adapter);

	if (!adapter->asq || !adapter->arq) {
		ixd_deinit_dflt_mbx(adapter);
		return -ENOENT;
	}

	queue_delayed_work(system_dfl_wq, &adapter->mbx_task, 0);

	return 0;
}

/**
 * ixd_init_task - Initialize after reset
 * @work: init work struct
 */
void ixd_init_task(struct work_struct *work)
{
	struct ixd_adapter *adapter;
	int err;

	adapter = container_of(work, struct ixd_adapter,
			       init_task.init_work.work);

	if (!ixd_check_reset_complete(adapter)) {
		if (++adapter->init_task.reset_retries < 10)
			queue_delayed_work(system_dfl_wq,
					   &adapter->init_task.init_work,
					   IXD_INIT_TASK_DELAY_JIFFIES);
		else
			dev_err(ixd_to_dev(adapter),
				"Device reset failed. The driver was unable to contact the device's firmware. Check that the FW is running.\n");
		return;
	}

	adapter->init_task.reset_retries = 0;
	err = ixd_init_dflt_mbx(adapter);
	if (err) {
		dev_err(ixd_to_dev(adapter),
			"Failed to initialize the default mailbox: %pe\n",
			ERR_PTR(err));
		return;
	}

	err = ixd_vc_dev_init(adapter);
	if (!err) {
		adapter->init_task.vc_retries = 0;
		adapter->init_task.success = true;
		ixd_devlink_register(adapter);

		err = ixd_init_interrupts(adapter);
		if (err)
			dev_err(ixd_to_dev(adapter),
				"Failed to initialize interrupts: %d\n",
				err);
		return;
	}

	libie_ctlq_xn_shutdown(adapter->xnm);
	ixd_trigger_reset(adapter);
	ixd_deinit_dflt_mbx(adapter);
	if (++adapter->init_task.vc_retries > 5 ||
	    (err != -ETIMEDOUT && err != -EAGAIN && err != -EBUSY)) {
		dev_err(ixd_to_dev(adapter),
			"Failed to establish mailbox communication with the hardware: %pe\n",
			ERR_PTR(err));
		return;
	}

	queue_delayed_work(system_dfl_wq, &adapter->init_task.init_work,
			   IXD_INIT_TASK_DELAY_JIFFIES);
}
