/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Let other files use rdma_dim_{init,destroy}
 *
 * Author: Håkon Bugge <haakon.bugge@oracle.com>
 *
 * Copyright (c) 2024 Oracle and/or its affiliates.
 */

#ifndef CQ_H
#define CQ_H

void rdma_dim_init(struct ib_cq *cq);
void rdma_dim_destroy(struct ib_cq *cq);

#endif
