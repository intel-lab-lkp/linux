/* SPDX-License-Identifier: GPL-2.0-only
 *
 * ZTE DingHai Rdma driver
 * Copyright (c) 2022-2026, ZTE Corporation
 */

#ifndef ZRDMA_DEFS_H
#define ZRDMA_DEFS_H

#include <linux/bitfield.h>
#include <rdma/ib_verbs.h>

/* RDMA TX DDR Access REG Masks */
#define ZXDH_TX_CACHE_ID GENMASK_ULL(1, 0)
#define ZXDH_TX_INDICATE_ID GENMASK_ULL(3, 2)
#define ZXDH_TX_AXI_ID GENMASK_ULL(6, 4)
#define ZXDH_TX_WAY_PARTITION GENMASK_ULL(9, 7)

/* RDMA RX DDR Access REG Masks */
#define ZXDH_RX_CACHE_ID GENMASK_ULL(1, 0)
#define ZXDH_RX_INDICATE_ID GENMASK_ULL(3, 2)
#define ZXDH_RX_AXI_ID GENMASK_ULL(6, 4)
#define ZXDH_RX_WAY_PARTITION GENMASK_ULL(9, 7)

/* RDMA IO REG Masks */
#define ZXDH_IOTABLE2_SID GENMASK_ULL(5, 0)

#define ZXDH_IOTABLE4_EPID GENMASK_ULL(14, 11)
#define ZXDH_IOTABLE4_VFID GENMASK_ULL(10, 3)
#define ZXDH_IOTABLE4_PFID GENMASK_ULL(2, 0)

#define ZXDH_IOTABLE7_PFID GENMASK_ULL(4, 2)
#define ZXDH_IOTABLE7_EPID GENMASK_ULL(8, 5)

#define RDMARX_MAX_MSG_SIZE 0x80000000

#endif /* ZRDMA_DEFS_H */
