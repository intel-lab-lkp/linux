// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2018-2025, Advanced Micro Devices, Inc. */

#include <linux/kernel.h>

#include "ionic.h"
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

const struct ionic_devinfo *ionic_api_get_devinfo(void *handle)
{
	struct ionic_lif *lif = handle;

	return &lif->ionic->idev.dev_info;
}
EXPORT_SYMBOL_NS(ionic_api_get_devinfo, "NET_IONIC");
