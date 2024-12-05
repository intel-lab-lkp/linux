/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2022,2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */
#ifndef _TMELCOM_H_
#define _TMELCOM_H_

/*----------------------------------------------------------------------------
 * Documentation
 * --------------------------------------------------------------------------
 */

/*
 * TMEL Messages Unique Identifiers bit layout
    _____________________________________
   |	   |	    |	   |
   | 31------16| 15-------8 | 7-------0 |
   | Reserved  |messageType | actionID  |
   |___________|____________|___________|
	       \___________  ___________/
			   \/
		      TMEL_MSG_UID
*/

/*
 * TMEL Messages Unique Identifiers Parameter ID bit layout
_________________________________________________________________________________________
|     |     |     |     |     |     |     |     |     |     |     |    |    |    |       |
|31-30|29-28|27-26|25-24|23-22|21-20|19-18|17-16|15-14|13-12|11-10|9--8|7--6|5--4|3-----0|
| p14 | p13 | p12 | p11 | p10 | p9  | p8  | p7  | p6  | p5  | p4  | p3 | p2 | p1 | nargs |
|type |type |type |type |type |type |type |type |type |type |type |type|type|type|       |
|_____|_____|_____|_____|_____|_____|_____|_____|_____|_____|_____|____|____|____|_______|

*/

/*
 * Macro used to define unique TMEL Message Identifier based on
 * message type and action identifier.
 */
#define TMEL_MSG_UID_CREATE(m, a)	((u32)(((m & 0xff) << 8) | (a & 0xff)))

/** Helper macro to extract the messageType from TMEL_MSG_UID. */
#define TMEL_MSG_UID_MSG_TYPE(v)	((v & GENMASK(15, 8)) >> 8)

/** Helper macro to extract the actionID from TMEL_MSG_UID. */
#define TMEL_MSG_UID_ACTION_ID(v)	(v & GENMASK(7, 0))

/****************************************************************************
 *
 * All definitions of supported messageType's.
 *
 * 0x00 -> 0xF0 messageType used for production use cases.
 * 0xF1 -> 0xFF messageType reserved(can be used for test puprposes).
 *
 * <Template> : TMEL_MSG_<MSGTYPE_NAME>
 * **************************************************************************/
#define TMEL_MSG_SECBOOT		 0x00

/****************************************************************************
 *
 * All definitions of action ID's per messageType.
 *
 * 0x00 -> 0xBF actionID used for production use cases.
 * 0xC0 -> 0xFF messageType must be reserved for test use cases.
 *
 * NOTE: Test ID's shouldn't appear in this file.
 *
 * <Template> : TMEL_ACTION_<MSGTYPE_NAME>_<ACTIONID_NAME>
 * **************************************************************************/

/*
 * ----------------------------------------------------------------------------
		Action ID's for TMEL_MSG_SECBOOT
 * ------------------------------------------------------------------------
 */
#define TMEL_ACTION_SECBOOT_SEC_AUTH		     0x04
#define TMEL_ACTION_SECBOOT_SS_TEAR_DOWN	     0x0A

/****************************************************************************
 *
 * All definitions of TMEL Message UID's (messageType | actionID).
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

#define TMEL_ERROR_GENERIC		(0x1U)
#define TMEL_ERROR_NOT_SUPPORTED	(0x2U)
#define TMEL_ERROR_BAD_PARAMETER	(0x3U)
#define TMEL_ERROR_BAD_MESSAGE		(0x4U)
#define TMEL_ERROR_BAD_ADDRESS		(0x5U)
#define TMEL_ERROR_TMELCOM_FAILURE	(0x6U)
#define TMEL_ERROR_TMEL_BUSY		(0x7U)

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
#endif  /*_TMELCOM_H_ */
