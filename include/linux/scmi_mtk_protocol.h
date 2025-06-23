/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * SCMI Message Protocol driver MediaTek extension header
 *
 * Copyright (c) 2021 MediaTek Inc.
 * Copyright (c) 2025 Collabora Ltd
 *                    AngeloGioacchino Del Regno <angelogioacchino.delregno@collabora.com>
 */

#ifndef _LINUX_SCMI_MTK_PROTOCOL_H
#define _LINUX_SCMI_MTK_PROTOCOL_H

#include <linux/bitfield.h>
#include <linux/device.h>
#include <linux/notifier.h>
#include <linux/types.h>

#define SCMI_PROTOCOL_MTK_TINYSYS	0x80
#define SCMI_MTK_VENDOR			"MediaTek"

#define SCMI_MTK_MSG_COMMON_PARAM_BYTES	5
#define SCMI_MTK_MSG_COMMON_REPLY_BYTES	2
#define SCMI_MTK_MSG_NOTIF_ST_BYTES	4
#define SCMI_MTK_MSG_SLBC_PARAM_BYTES	4

struct scmi_mtk_tinysys_status {
	u32 rsvd;
	u32 reply[SCMI_MTK_MSG_COMMON_REPLY_BYTES];
};

struct scmi_mtk_tinysys_slbc {
	u32 cmd;
	u32 arg[SCMI_MTK_MSG_SLBC_PARAM_BYTES];
};

struct scmi_mtk_tinysys_proto_ops {
	int (*common_get)(const struct scmi_protocol_handle *ph,
			  u32 ctrl_id, u32 cmd,
			  struct scmi_mtk_tinysys_status *retval);
	int (*cm_mgr_set)(const struct scmi_protocol_handle *ph,
			  u32 ctrl_id, u32 cmd, u32 arg);
	int (*gpu_pwr_set)(const struct scmi_protocol_handle *ph,
			   u32 ctrl_id, u8 pwr_indication, bool enable);
	int (*slbc_req)(const struct scmi_protocol_handle *ph,
			const struct scmi_mtk_tinysys_slbc *req,
			struct scmi_mtk_tinysys_slbc *retval);
	bool (*sspm_is_alive)(const struct scmi_protocol_handle *ph,
			      u32 ctrl_id);
	int (*sspm_mem_set)(const struct scmi_protocol_handle *ph,
			    u32 ctrl_id, u32 pa, u32 mem_sz);
};

enum scmi_mtk_tinysys_notification_events {
	SCMI_EVENT_MTK_TINYSYS_NOTIFIER = 0x0,
};

struct scmi_mtk_tinysys_notif_report {
	ktime_t timestamp;
	unsigned int fid;
	unsigned int status[SCMI_MTK_MSG_NOTIF_ST_BYTES];
};
#endif
