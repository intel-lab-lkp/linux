/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2018-2025, Advanced Micro Devices, Inc. */

#ifndef _IONIC_IBDEV_H_
#define _IONIC_IBDEV_H_

#include <rdma/ib_verbs.h>
#include <linux/ionic/ionic_api.h>

#define IONIC_MIN_RDMA_VERSION	0
#define IONIC_MAX_RDMA_VERSION	2

struct ionic_ibdev {
	struct ib_device	ibdev;

	struct device		*hwdev;
	struct net_device	*ndev;

	const union ionic_lif_identity	*ident;

	void		*handle;
	int			lif_index;

	u8			rdma_version;
};

#endif /* _IONIC_IBDEV_H_ */
