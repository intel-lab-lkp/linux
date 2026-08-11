/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
 *
 * ZTE DingHai Rdma driver
 * Copyright (c) 2022-2026, ZTE Corporation
 */

#ifndef ZRDMA_DEFS_H
#define ZRDMA_DEFS_H

#include <linux/bitfield.h>
#include <rdma/ib_verbs.h>

#define ZXDH_FEATURE_RTS_AE 1ULL
#define ZXDH_FEATURE_CQ_RESIZE 2ULL
#define ZXDH_FEATURE_64_BYTE_CQE 128ULL

#define ZXDH_MAX_USER_PRIORITY 8

#endif /* ZRDMA_DEFS_H */
