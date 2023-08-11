// SPDX-License-Identifier: GPL-2.0
/*
 * System Control and Management Interface (SCMI) Message
 * Qualcomm HVC/shmem Transport driver
 *
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 * Copyright 2020 NXP
 *
 * This is based on drivers/firmware/arm_scmi/smc.c
 */

#include <linux/arm-smccc.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/slab.h>

#include "common.h"

/**
 * struct scmi_qcom_hvc - Structure representing a SCMI qcom hvc transport
 *
 * @irq: An optional IRQ for completion
 * @cinfo: SCMI channel info
 * @shmem: Transmit/Receive shared memory area
 * @shmem_lock: Lock to protect access to Tx/Rx shared memory area.
 * @func_id: hvc call function-id
 * @cap_id: hvc doorbell's capability id
 */

struct scmi_qcom_hvc {
	int irq;
	struct scmi_chan_info *cinfo;
	struct scmi_shared_mem __iomem *shmem;
	/* Protect access to shmem area */
	struct mutex shmem_lock;
	u32 func_id;
	u64 cap_id;
};

static irqreturn_t qcom_hvc_msg_done_isr(int irq, void *data)
{
	struct scmi_qcom_hvc *scmi_info = data;

	scmi_rx_callback(scmi_info->cinfo, shmem_read_header(scmi_info->shmem), NULL);

	return IRQ_HANDLED;
}

static bool qcom_hvc_chan_available(struct device_node *of_node, int idx)
{
	struct device_node *np = of_parse_phandle(of_node, "shmem", 0);

	if (!np)
		return false;

	of_node_put(np);
	return true;
}

static int qcom_hvc_chan_setup(struct scmi_chan_info *cinfo,
			       struct device *dev, bool tx)
{
	struct device *cdev = cinfo->dev;
	struct scmi_qcom_hvc *scmi_info;
	struct device_node *np;
	resource_size_t size;
	struct resource res;
	u32 func_id;
	u64 cap_id;
	int ret;

	if (!tx)
		return -ENODEV;

	scmi_info = devm_kzalloc(dev, sizeof(*scmi_info), GFP_KERNEL);
	if (!scmi_info)
		return -ENOMEM;

	np = of_parse_phandle(cdev->of_node, "shmem", 0);
	if (!of_device_is_compatible(np, "arm,scmi-shmem")) {
		of_node_put(np);
		return -ENXIO;
	}

	ret = of_address_to_resource(np, 0, &res);
	of_node_put(np);
	if (ret) {
		dev_err(cdev, "failed to get SCMI Tx shared memory\n");
		return ret;
	}

	size = resource_size(&res);

	/* The func-id & capability-id are kept in last 16 bytes of shmem.
	 *     +-------+
	 *     |       |
	 *     | shmem |
	 *     |       |
	 *     |       |
	 *     +-------+ <-- (size - 16)
	 *     | funcId|
	 *     +-------+ <-- (size - 8)
	 *     | capId |
	 *     +-------+ <-- size
	 */

	scmi_info->shmem = devm_ioremap(dev, res.start, size);
	if (!scmi_info->shmem) {
		dev_err(dev, "failed to ioremap SCMI Tx shared memory\n");
		return -EADDRNOTAVAIL;
	}

	func_id = readl((void __iomem *)(scmi_info->shmem) + size - 16);

#ifdef CONFIG_ARM64
	cap_id = readq((void __iomem *)(scmi_info->shmem) + size - 8);
#else
	/* capability-id is 32 bit long on 32bit machines */
	cap_id = readl((void __iomem *)(scmi_info->shmem) + size - 8);
#endif

	/*
	 * If there is an interrupt named "a2p", then the service and
	 * completion of a message is signaled by an interrupt rather than by
	 * the return of the hvc call.
	 */
	scmi_info->irq = of_irq_get_byname(cdev->of_node, "a2p");
	if (scmi_info->irq > 0) {
		ret = request_irq(scmi_info->irq, qcom_hvc_msg_done_isr,
				  IRQF_NO_SUSPEND, dev_name(dev), scmi_info);
		if (ret) {
			dev_err(dev, "failed to setup SCMI completion irq\n");
			return ret;
		}
	} else {
		cinfo->no_completion_irq = true;
	}

	scmi_info->func_id = func_id;
	scmi_info->cap_id = cap_id;
	scmi_info->cinfo = cinfo;
	mutex_init(&scmi_info->shmem_lock);
	cinfo->transport_info = scmi_info;

	return 0;
}

static int qcom_hvc_chan_free(int id, void *p, void *data)
{
	struct scmi_chan_info *cinfo = p;
	struct scmi_qcom_hvc *scmi_info = cinfo->transport_info;

	/* Ignore any possible further reception on the IRQ path */
	if (scmi_info->irq > 0)
		free_irq(scmi_info->irq, scmi_info);

	cinfo->transport_info = NULL;
	scmi_info->cinfo = NULL;

	return 0;
}

static int qcom_hvc_send_message(struct scmi_chan_info *cinfo,
				 struct scmi_xfer *xfer)
{
	struct scmi_qcom_hvc *scmi_info = cinfo->transport_info;
	struct arm_smccc_res res;

	/*
	 * Channel will be released only once response has been
	 * surely fully retrieved, so after .mark_txdone()
	 */
	mutex_lock(&scmi_info->shmem_lock);

	shmem_tx_prepare(scmi_info->shmem, xfer, cinfo);

	arm_smccc_1_1_hvc(scmi_info->func_id, (unsigned long)scmi_info->cap_id,
			  0, 0, 0, 0, 0, 0, &res);

	if (res.a0) {
		mutex_unlock(&scmi_info->shmem_lock);
		return -EOPNOTSUPP;
	}

	return 0;
}

static void qcom_hvc_fetch_response(struct scmi_chan_info *cinfo,
				    struct scmi_xfer *xfer)
{
	struct scmi_qcom_hvc *scmi_info = cinfo->transport_info;

	shmem_fetch_response(scmi_info->shmem, xfer);
}

static void qcom_hvc_mark_txdone(struct scmi_chan_info *cinfo, int ret,
				 struct scmi_xfer *__unused)
{
	struct scmi_qcom_hvc *scmi_info = cinfo->transport_info;

	mutex_unlock(&scmi_info->shmem_lock);
}

static bool
qcom_hvc_poll_done(struct scmi_chan_info *cinfo, struct scmi_xfer *xfer)
{
	struct scmi_qcom_hvc *scmi_info = cinfo->transport_info;

	return shmem_poll_done(scmi_info->shmem, xfer);
}

static const struct scmi_transport_ops scmi_qcom_hvc_ops = {
	.chan_available = qcom_hvc_chan_available,
	.chan_setup = qcom_hvc_chan_setup,
	.chan_free = qcom_hvc_chan_free,
	.send_message = qcom_hvc_send_message,
	.mark_txdone = qcom_hvc_mark_txdone,
	.fetch_response = qcom_hvc_fetch_response,
	.poll_done = qcom_hvc_poll_done,
};

const struct scmi_desc scmi_qcom_hvc_desc = {
	.ops = &scmi_qcom_hvc_ops,
	.max_rx_timeout_ms = 30,
	.max_msg = 20,
	.max_msg_size = 128,
};
