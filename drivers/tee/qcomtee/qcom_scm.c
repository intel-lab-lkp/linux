// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/firmware/qcom/qcom_scm.h>

#include "qcomtee_private.h"

int qcomtee_object_invoke_ctx_invoke(struct qcomtee_object_invoke_ctx *oic,
				     int *result, u64 *res_type)
{
	phys_addr_t out_msg_paddr;
	phys_addr_t in_msg_paddr;
	int ret;
	u64 res;

	tee_shm_get_pa(oic->out_shm, 0, &out_msg_paddr);
	tee_shm_get_pa(oic->in_shm, 0, &in_msg_paddr);
	if (!(oic->flags & QCOMTEE_OIC_FLAG_BUSY)) {
		/* Direct QTEE object invocation. */
		ret = qcom_scm_qtee_invoke_smc(in_msg_paddr, oic->in_msg.size,
					       out_msg_paddr, oic->out_msg.size,
					       &res, res_type);
	} else {
		/* Submit callback response. */
		ret = qcom_scm_qtee_callback_response(out_msg_paddr,
						      oic->out_msg.size,
						      &res, res_type);
	}

	if (ret)
		pr_err("QTEE returned with %d.\n", ret);
	else
		*result = (int)res;

	return ret;
}
