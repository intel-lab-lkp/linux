/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright 2026 NXP
 */

#ifndef ELE_FW_API_H
#define ELE_FW_API_H
#include "se_ctrl.h"

#define ELE_SESSION_OPEN_REQ            0x10u
#define ELE_SESSION_CLOSE_REQ           0x11u
#define ELE_STORAGE_OPEN_REQ            0xE0u
#define ELE_STORAGE_CLOSE_REQ           0xE1u
#define ELE_STORAGE_MASTER_IMPORT_REQ   0xE2u

int ele_uapi_allowed_fw_cmd(struct se_if_device_ctx *dev_ctx, struct se_msg_hdr *header);
void fw_api_specific_ops(struct se_if_device_ctx *dev_ctx, struct se_msg_hdr *header);
#endif /* ELE_FW_API_H */
