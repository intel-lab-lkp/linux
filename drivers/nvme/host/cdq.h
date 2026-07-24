/* SPDX-License-Identifier: GPL-2.0 */
/*
 * NVMe Controller Data Queue (CDQ) support.
 */

#ifndef _NVME_CDQ_H
#define _NVME_CDQ_H

#include "nvme.h"

struct cdq_nvme_queue {
	u16 id;
	struct nvme_ctrl *ctrl;
};

void nvme_delete_cdq(struct cdq_nvme_queue *cdq);
void nvme_delete_cdqs_host(struct nvme_ctrl *ctrl);
void nvme_free_cdqs(struct nvme_ctrl *ctrl);

#endif /* _NVME_CDQ_H */
