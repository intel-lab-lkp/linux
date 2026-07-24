// SPDX-License-Identifier: GPL-2.0
/*
 * NVMe Controller Data Queue (CDQ) support.
 */

#include "nvme.h"
#include "cdq.h"

static int nvme_submit_delete_cdq_cmd(const struct cdq_nvme_queue *cdq)
{
	struct nvme_command c = {
		.cdq.opcode = nvme_admin_cdq,
		.cdq.sel = NVME_CDQ_CMD_MGMT_DELETE,
		.cdq.dw11.cdqid = cpu_to_le16(cdq->id)
	};

	return __nvme_submit_sync_cmd(cdq->ctrl->admin_q, &c, NULL, NULL, 0, NVME_QID_ANY, 0);
}

/* Sends a CDQ delete NVMe cmd */
static void nvme_delete_cdq_ctrl(struct cdq_nvme_queue *cdq)
{
	if (nvme_submit_delete_cdq_cmd(cdq))
		WARN_ONCE(1, "Failed delete CDQ (id: %d)", cdq->id);
}

/* Does NOT send a CDQ delete NVMe cmd */
static void nvme_delete_cdq_host(struct cdq_nvme_queue *cdq)
{
	u16 cdq_id = cdq->id;
	struct nvme_ctrl *ctrl = cdq->ctrl;

	xa_erase(&ctrl->cdqs, cdq_id);
}

void nvme_delete_cdq(struct cdq_nvme_queue *cdq)
{
	nvme_delete_cdq_ctrl(cdq);
	nvme_delete_cdq_host(cdq);
}
EXPORT_SYMBOL_GPL(nvme_delete_cdq);

/* Will NOT send a CDQ delete NVMe cmd. */
void nvme_delete_cdqs_host(struct nvme_ctrl *ctrl)
{
	struct cdq_nvme_queue *cdq;
	unsigned long i;

	xa_for_each(&ctrl->cdqs, i, cdq)
		nvme_delete_cdq_host(cdq);
}

/* Final teardown at device->release: free all CDQs and destroy the xarray. */
void nvme_free_cdqs(struct nvme_ctrl *ctrl)
{
	/*
	 * Delete host side CDQ only. NOT sending delete cmd as
	 * Ctrl should delete on disable.
	 */
	nvme_delete_cdqs_host(ctrl);
	xa_destroy(&ctrl->cdqs);
}
