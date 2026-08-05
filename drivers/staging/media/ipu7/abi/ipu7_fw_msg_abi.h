/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2020 - 2025 Intel Corporation
 */

#ifndef IPU7_FW_MSG_ABI_H
#define IPU7_FW_MSG_ABI_H

#include "ipu7_fw_common_abi.h"

#pragma pack(push, 1)

#define IPU_MSG_NODE_MAX_DEVICES	(128U)
#define DEB_NUM_UINT32	(IPU_MSG_NODE_MAX_DEVICES / (sizeof(u32) * 8U))

typedef u32 ipu7_msg_teb_t[2];
typedef u32 ipu7_msg_deb_t[DEB_NUM_UINT32];

#define IPU_MSG_NODE_MAX_ROUTE_ENABLES	(128U)
#define RBM_NUM_UINT32	(IPU_MSG_NODE_MAX_ROUTE_ENABLES / (sizeof(u32) * 8U))

typedef u32 ipu7_msg_rbm_t[RBM_NUM_UINT32];

#pragma pack(pop)

#pragma pack(push, 1)

#define IPU_MSG_LINK_FOREIGN_KEY_NONE		(65535U)
#define IPU_MSG_LINK_PBK_ID_DONT_CARE		(255U)
#define IPU_MSG_LINK_PBK_SLOT_ID_DONT_CARE	(255U)

#pragma pack(pop)

#pragma pack(push, 1)
#pragma pack(pop)

#pragma pack(push, 1)
#pragma pack(pop)

#pragma pack(push, 1)

#pragma pack(pop)

#pragma pack(push, 1)
#pragma pack(pop)

#define FWPS_MSG_ABI_MAX_INPUT_QUEUES	(60U)
#define FWPS_MSG_ABI_MAX_OUTPUT_QUEUES	(2U)

#define FWPS_MSG_ABI_OUT_LOG_QUEUE_ID	(IA_GOFO_MSG_ABI_OUT_LOG_QUEUE_ID)
#if (FWPS_MSG_ABI_OUT_LOG_QUEUE_ID >= FWPS_MSG_ABI_MAX_OUTPUT_QUEUES)
#error "Maximum output queues configuration is too small to fit ACK and LOG \
queues"
#endif
#define FWPS_MSG_ABI_IN_RESERVED_QUEUE_ID	(3U)
#define FWPS_MSG_ABI_IN_FIRST_TASK_QUEUE_ID \
	(FWPS_MSG_ABI_IN_RESERVED_QUEUE_ID + 1U)

#if (FWPS_MSG_ABI_IN_FIRST_TASK_QUEUE_ID >= FWPS_MSG_ABI_MAX_INPUT_QUEUES)
#error "Maximum queues configuration is too small to fit minimum number of \
useful queues"
#endif

#endif
