// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2026 NXP
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/arm-smccc.h>
#include <linux/build_bug.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/mailbox_client.h>
#include <linux/mailbox/riscv-rpmi-message.h>
#include <linux/mailbox/riscv-sbi-mpxy-mbox.h>
#include <linux/printk.h>
#include <linux/property.h>
#include <linux/types.h>
#include <linux/uuid.h>
#include "optee_private.h"
#include "optee_rpmi.h"
#include "optee_smc.h"

/*
 * This file implements the conduit carrying the SMC ABI over the TEE
 * service group of the RISC-V Platform Management Interface (RPMI). Each
 * invocation of the ABI is a TEE_CALL request sent on an SBI MPXY channel
 * with the message layout described in optee_rpmi.h.
 *
 * The TEE_CALL request is not sent with mbox_send_message(): the target
 * TEE executes on the calling hart until it responds, so the transfer is
 * done with riscv_sbi_mpxy_mbox_call() which bypasses the mailbox core
 * queue and channel lock. Calls from different harts run concurrently,
 * exactly like SMCs do.
 */

static_assert(sizeof(struct optee_rpmi_call_req) == 92);
static_assert(sizeof(struct optee_rpmi_call_rsp) == 40);

/**
 * struct optee_rpmi_conduit - RPMI TEE service group conduit
 * @cl:		mailbox client owning @chan
 * @chan:	SBI MPXY channel implementing the RPMI TEE service group
 * @sender_id:	endpoint identifier of the REE
 * @target_id:	endpoint identifier of OP-TEE
 */
struct optee_rpmi_conduit {
	struct mbox_client cl;
	struct mbox_chan *chan;
	u32 sender_id;
	u32 target_id;
};

/* Like the rest of the SMC ABI, at most a single OP-TEE instance is supported */
static struct optee_rpmi_conduit optee_rpmi;

static const uuid_t optee_rpmi_service_uuid = OPTEE_RPMI_SERVICE_UUID;

static void optee_rpmi_invoke_fn(unsigned long a0, unsigned long a1,
				 unsigned long a2, unsigned long a3,
				 unsigned long a4, unsigned long a5,
				 unsigned long a6, unsigned long a7,
				 struct arm_smccc_res *res)
{
	struct optee_rpmi_call_req req = {
		.sender_id = cpu_to_le32(optee_rpmi.sender_id),
		.target_id = cpu_to_le32(optee_rpmi.target_id),
		.data_len = cpu_to_le32(sizeof(req.args)),
		.args = {
			cpu_to_le64(a0), cpu_to_le64(a1), cpu_to_le64(a2),
			cpu_to_le64(a3), cpu_to_le64(a4), cpu_to_le64(a5),
			cpu_to_le64(a6), cpu_to_le64(a7),
		},
	};
	struct optee_rpmi_call_rsp rsp = {};
	struct rpmi_mbox_message msg;
	int rc;

	export_uuid(req.service, &optee_rpmi_service_uuid);

	rpmi_mbox_init_send_with_response(&msg, RPMI_TEE_SRV_CALL,
					  &req, sizeof(req), &rsp, sizeof(rsp));
	rc = riscv_sbi_mpxy_mbox_call(optee_rpmi.chan, &msg);
	if (!rc) {
		if (msg.data.out_response_len < sizeof(rsp.status))
			rc = -EIO;
		else
			rc = rpmi_to_linux_error((s32)le32_to_cpu(rsp.status));
	}
	if (!rc && (msg.data.out_response_len < sizeof(rsp) ||
		    le32_to_cpu(rsp.rsp_len) != sizeof(rsp.rets)))
		rc = -EIO;

	if (rc) {
		/*
		 * The SBI implementation or the RPMI TEE framework failed to
		 * deliver the call, OP-TEE was not reached. Report it in a0
		 * as the SMC ABI does for a call that could not be handled.
		 */
		if (rc != -EBUSY)
			pr_warn_ratelimited("TEE_CALL of 0x%lx failed: %d\n",
					    a0, rc);
		res->a0 = rc == -EBUSY ? OPTEE_SMC_RETURN_EBUSY :
					 OPTEE_SMC_RETURN_ENOTAVAIL;
		res->a1 = 0;
		res->a2 = 0;
		res->a3 = 0;
		return;
	}

	res->a0 = le64_to_cpu(rsp.rets[0]);
	res->a1 = le64_to_cpu(rsp.rets[1]);
	res->a2 = le64_to_cpu(rsp.rets[2]);
	res->a3 = le64_to_cpu(rsp.rets[3]);
}

static void optee_rpmi_conduit_release(void *data)
{
	struct optee_rpmi_conduit *conduit = data;

	mbox_free_channel(conduit->chan);
	conduit->chan = NULL;
}

static int optee_rpmi_get_attr(struct optee_rpmi_conduit *conduit,
			       enum rpmi_mbox_attribute_id id, u32 *value)
{
	struct rpmi_mbox_message msg;
	int rc;

	rpmi_mbox_init_get_attribute(&msg, id);
	rc = rpmi_mbox_send_message(conduit->chan, &msg);
	if (rc)
		return rc;

	*value = msg.attr.value;
	return 0;
}

optee_invoke_fn *optee_rpmi_conduit_init(struct device *dev)
{
	struct optee_rpmi_conduit *conduit = &optee_rpmi;
	u32 value;
	int rc;

	if (conduit->chan)
		return ERR_PTR(-EBUSY);

	rc = device_property_read_u32(dev, "riscv,rpmi-tee-sender-id",
				      &conduit->sender_id);
	if (rc)
		return ERR_PTR(dev_err_probe(dev, rc,
					     "missing \"riscv,rpmi-tee-sender-id\" property\n"));

	rc = device_property_read_u32(dev, "riscv,rpmi-tee-target-id",
				      &conduit->target_id);
	if (rc)
		return ERR_PTR(dev_err_probe(dev, rc,
					     "missing \"riscv,rpmi-tee-target-id\" property\n"));

	conduit->cl.dev = dev;
	conduit->cl.tx_block = false;
	conduit->cl.knows_txdone = true;
	conduit->chan = mbox_request_channel(&conduit->cl, 0);
	if (IS_ERR(conduit->chan)) {
		rc = PTR_ERR(conduit->chan);
		conduit->chan = NULL;
		return ERR_PTR(dev_err_probe(dev, rc,
					     "failed to request MPXY channel\n"));
	}

	rc = devm_add_action_or_reset(dev, optee_rpmi_conduit_release, conduit);
	if (rc)
		return ERR_PTR(rc);

	rc = optee_rpmi_get_attr(conduit, RPMI_MBOX_ATTR_SERVICEGROUP_ID,
				 &value);
	if (rc)
		return ERR_PTR(dev_err_probe(dev, rc,
					     "failed to read RPMI service group ID\n"));
	if (value != RPMI_SRVGRP_TEE) {
		dev_err(dev, "MPXY channel implements RPMI service group 0x%x, not TEE\n",
			value);
		return ERR_PTR(-ENODEV);
	}

	rc = optee_rpmi_get_attr(conduit, RPMI_MBOX_ATTR_MAX_MSG_DATA_SIZE,
				 &value);
	if (rc)
		return ERR_PTR(dev_err_probe(dev, rc,
					     "failed to read RPMI max message size\n"));
	if (value < sizeof(struct optee_rpmi_call_req) ||
	    value < sizeof(struct optee_rpmi_call_rsp)) {
		dev_err(dev, "MPXY channel message size %u is too small\n",
			value);
		return ERR_PTR(-EINVAL);
	}

	pr_info("using RPMI TEE service group, endpoints %u -> %u\n",
		conduit->sender_id, conduit->target_id);

	return optee_rpmi_invoke_fn;
}
