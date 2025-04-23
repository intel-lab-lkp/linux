// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2018-2025, Advanced Micro Devices, Inc. */

#include <linux/kernel.h>

#include "ionic.h"
#include "ionic_bus.h"
#include "ionic_lif.h"

struct net_device *ionic_api_get_netdev_from_handle(void *handle)
{
	struct ionic_lif *lif = handle;

	if (!lif)
		return ERR_PTR(-ENXIO);

	dev_hold(lif->netdev);

	return lif->netdev;
}
EXPORT_SYMBOL_NS(ionic_api_get_netdev_from_handle, "NET_IONIC");

const union ionic_lif_identity *ionic_api_get_identity(void *handle,
						       int *lif_index)
{
	struct ionic_lif *lif = handle;

	if (lif_index)
		*lif_index = lif->index;

	return &lif->ionic->ident.lif;
}
EXPORT_SYMBOL_NS(ionic_api_get_identity, "NET_IONIC");

void ionic_api_request_reset(void *handle)
{
	struct ionic_lif *lif = handle;
	struct ionic *ionic;
	int err;

	union ionic_dev_cmd cmd = {
		.cmd.opcode = IONIC_CMD_RDMA_RESET_LIF,
		.cmd.lif_index = cpu_to_le16(lif->index),
	};

	ionic = lif->ionic;

	mutex_lock(&ionic->dev_cmd_lock);

	ionic_dev_cmd_go(&ionic->idev, &cmd);
	err = ionic_dev_cmd_wait(ionic, DEVCMD_TIMEOUT);

	mutex_unlock(&ionic->dev_cmd_lock);

	if (err)
		netdev_warn(lif->netdev, "request_reset: error %d\n", err);
}
EXPORT_SYMBOL_NS(ionic_api_request_reset, "NET_IONIC");

const struct ionic_devinfo *ionic_api_get_devinfo(void *handle)
{
	struct ionic_lif *lif = handle;

	return &lif->ionic->idev.dev_info;
}
EXPORT_SYMBOL_NS(ionic_api_get_devinfo, "NET_IONIC");

int ionic_api_get_intr(void *handle, int *irq)
{
	struct ionic_intr_info intr_obj;
	struct ionic_lif *lif = handle;
	int err;

	if (!lif->nrdma_eqs_avail)
		return -ENOSPC;

	err = ionic_intr_alloc(lif->ionic, &intr_obj);
	if (err)
		return err;

	err = ionic_bus_get_irq(lif->ionic, intr_obj.index);
	if (err < 0) {
		ionic_intr_free(lif->ionic, intr_obj.index);
		return err;
	}

	lif->nrdma_eqs_avail--;

	*irq = err;
	return intr_obj.index;
}
EXPORT_SYMBOL_NS(ionic_api_get_intr, "NET_IONIC");

void ionic_api_put_intr(void *handle, int intr_index)
{
	struct ionic_lif *lif = handle;

	ionic_intr_free(lif->ionic, intr_index);

	lif->nrdma_eqs_avail++;
}
EXPORT_SYMBOL_NS(ionic_api_put_intr, "NET_IONIC");

void ionic_api_kernel_dbpage(void *handle,
			     struct ionic_intr __iomem **intr_ctrl,
			     u32 *dbid, u64 __iomem **dbpage)
{
	struct ionic_lif *lif = handle;

	*intr_ctrl = lif->ionic->idev.intr_ctrl;

	*dbid = lif->kern_pid;
	*dbpage = lif->kern_dbpage;
}
EXPORT_SYMBOL_NS(ionic_api_kernel_dbpage, "NET_IONIC");

int ionic_api_adminq_post(void *handle, struct ionic_admin_ctx *ctx)
{
	return ionic_adminq_post(handle, ctx);
}
EXPORT_SYMBOL_NS(ionic_api_adminq_post, "NET_IONIC");

int ionic_api_adminq_post_wait(void *handle, struct ionic_admin_ctx *ctx)
{
	return ionic_adminq_post_wait(handle, ctx);
}
EXPORT_SYMBOL_NS(ionic_api_adminq_post_wait, "NET_IONIC");

int ionic_api_error_to_errno(enum ionic_status_code code)
{
	return ionic_error_to_errno(code);
}
EXPORT_SYMBOL_NS(ionic_api_error_to_errno, "NET_IONIC");
