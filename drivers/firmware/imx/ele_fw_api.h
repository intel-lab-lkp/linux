/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright 2026 NXP
 */

#ifndef ELE_FW_API_H
#define ELE_FW_API_H
#include "se_ctrl.h"

#define ELE_SESSION_OPEN_REQ            0x10u

#define ELE_SESSION_CLOSE_REQ_SZ	0x08u
#define ELE_SESSION_CLOSE_RSP_SZ	0x08u
#define ELE_SESSION_CLOSE_REQ           0x11u

#define ELE_STORAGE_OPEN_REQ            0xE0u

#define ELE_STORAGE_CLOSE_REQ_SZ	0x08u
#define ELE_STORAGE_CLOSE_RSP_SZ	0x08u
#define ELE_STORAGE_CLOSE_REQ           0xE1u

#define ELE_STORAGE_MASTER_IMPORT_REQ   0xE2u

int ele_uapi_allowed_fw_cmd(struct se_if_device_ctx *dev_ctx, struct se_msg_hdr *header);
void fw_api_specific_ops(struct se_if_device_ctx *dev_ctx, struct se_api_msg *rx_msg);
bool is_msg_xchng_for_tdown(void *tx_msg);
int se_close_session(struct se_if_priv *priv, u32 session_hdl);
int se_close_storage(struct se_if_priv *priv, u32 storage_hdl);
#endif /* ELE_FW_API_H */
