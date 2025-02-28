/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2025 Qualcomm Innovation Center, Inc. All rights reserved.
 */
#ifndef _TMELCOM_H_
#define _TMELCOM_H_

/*
 * Macro used to define unique TMEL Message Identifier based on
 * message type and action identifier.
 */
#define TMEL_MSG_UID_CREATE(msg_type, action_id)	\
	(FIELD_PREP_CONST((0xff << 8), msg_type) | FIELD_PREP_CONST(0xff, action_id))

/** Helper macro to extract the messageType from TMEL_MSG_UID. */
#define TMEL_MSG_UID_MSG_TYPE(v)	FIELD_GET(GENMASK(15, 8), v)

/** Helper macro to extract the actionID from TMEL_MSG_UID. */
#define TMEL_MSG_UID_ACTION_ID(v)	FIELD_GET(GENMASK(7, 0), v)

/****************************************************************************
 *
 * All definitions of supported messageTypes.
 *
 * <Template> : TMEL_MSG_<MSGTYPE_NAME>
 * **************************************************************************/
#define TMEL_MSG_SECBOOT		 0x00

/****************************************************************************
 *
 * All definitions of action IDs per messageType.
 *
 * <Template> : TMEL_ACTION_<MSGTYPE_NAME>_<ACTIONID_NAME>
 * **************************************************************************/

/*
 * ----------------------------------------------------------------------------
		Action ID's for TMEL_MSG_SECBOOT
 * ------------------------------------------------------------------------
 */
#define TMEL_ACTION_SECBOOT_SEC_AUTH		     0x04
#define TMEL_ACTION_SECBOOT_SS_TEAR_DOWN	     0x0a

/****************************************************************************
 *
 * All definitions of TMEL Message UIDs (messageType | actionID).
 *
 * <Template> : TMEL_MSG_UID_<MSGTYPE_NAME>_<ACTIONID_NAME>
 * *************************************************************************/

/*----------------------------------------------------------------------------
 * UID's for TMEL_MSG_SECBOOT
 *-------------------------------------------------------------------------
 */
#define TMEL_MSG_UID_SECBOOT_SEC_AUTH	    TMEL_MSG_UID_CREATE(TMEL_MSG_SECBOOT,\
					    TMEL_ACTION_SECBOOT_SEC_AUTH)

#define TMEL_MSG_UID_SECBOOT_SS_TEAR_DOWN	TMEL_MSG_UID_CREATE(TMEL_MSG_SECBOOT,\
						TMEL_ACTION_SECBOOT_SS_TEAR_DOWN)

#define HW_MBOX_SIZE			32
#define MBOX_QMP_CTRL_DATA_SIZE		4
#define MBOX_RSV_SIZE			4
#define MBOX_IPC_PACKET_SIZE		(HW_MBOX_SIZE - MBOX_QMP_CTRL_DATA_SIZE - MBOX_RSV_SIZE)
#define MBOX_IPC_MAX_PARAMS		5

#define MAX_PARAM_IN_PARAM_ID		14
#define PARAM_CNT_FOR_PARAM_TYPE_OUTBUF	3
#define SRAM_IPC_MAX_PARAMS		(MAX_PARAM_IN_PARAM_ID * PARAM_CNT_FOR_PARAM_TYPE_OUTBUF)
#define SRAM_IPC_MAX_BUF_SIZE		(SRAM_IPC_MAX_PARAMS * sizeof(u32))

#define TMEL_ERROR_GENERIC		(0x1u)
#define TMEL_ERROR_NOT_SUPPORTED	(0x2u)
#define TMEL_ERROR_BAD_PARAMETER	(0x3u)
#define TMEL_ERROR_BAD_MESSAGE		(0x4u)
#define TMEL_ERROR_BAD_ADDRESS		(0x5u)
#define TMEL_ERROR_TMELCOM_FAILURE	(0x6u)
#define TMEL_ERROR_TMEL_BUSY		(0x7u)

enum ipc_type {
	IPC_MBOX_ONLY,
	IPC_MBOX_SRAM,
};

struct ipc_header {
	u8 ipc_type:1;
	u8 msg_len:7;
	u8 msg_type;
	u8 action_id;
	s8 response;
} __packed;

struct mbox_payload {
	u32 param[MBOX_IPC_MAX_PARAMS];
};

struct sram_payload {
	u32 payload_ptr;
	u32 payload_len;
};

union ipc_payload {
	struct mbox_payload mbox_payload;
	struct sram_payload sram_payload;
} __packed;

struct tmel_ipc_pkt {
	struct ipc_header msg_hdr;
	union ipc_payload payload;
} __packed;

struct tmel_qmp_msg {
	void *msg;
	u32 msg_id;
};

struct tmel_sec_auth {
	void *data;
	u32 size;
	u32 pas_id;
};
#endif  /* _TMELCOM_H_ */
