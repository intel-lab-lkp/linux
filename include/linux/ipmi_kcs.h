/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2015-2018, Intel Corporation.
 */

#ifndef __IPMI_KCS_H__
#define __IPMI_KCS_H__

/* Different phases of the KCS BMC module.
 *  KCS_PHASE_IDLE:
 *            BMC should not be expecting nor sending any data.
 *  KCS_PHASE_WRITE_START:
 *            BMC is receiving a WRITE_START command from system software.
 *  KCS_PHASE_WRITE_DATA:
 *            BMC is receiving a data byte from system software.
 *  KCS_PHASE_WRITE_END_CMD:
 *            BMC is waiting a last data byte from system software.
 *  KCS_PHASE_WRITE_DONE:
 *            BMC has received the whole request from system software.
 *  KCS_PHASE_WAIT_READ:
 *            BMC is waiting the response from the upper IPMI service.
 *  KCS_PHASE_READ:
 *            BMC is transferring the response to system software.
 *  KCS_PHASE_ABORT_ERROR1:
 *            BMC is waiting error status request from system software.
 *  KCS_PHASE_ABORT_ERROR2:
 *            BMC is waiting for idle status afer error from system software.
 *  KCS_PHASE_ERROR:
 *            BMC has detected a protocol violation at the interface level.
 */
enum kcs_ipmi_phases {
	KCS_PHASE_IDLE,

	KCS_PHASE_WRITE_START,
	KCS_PHASE_WRITE_DATA,
	KCS_PHASE_WRITE_END_CMD,
	KCS_PHASE_WRITE_DONE,

	KCS_PHASE_WAIT_READ,
	KCS_PHASE_READ,

	KCS_PHASE_ABORT_ERROR1,
	KCS_PHASE_ABORT_ERROR2,
	KCS_PHASE_ERROR
};

/* IPMI 2.0 - Table 9-4, KCS Interface Status Codes */
enum kcs_ipmi_errors {
	KCS_NO_ERROR                = 0x00,
	KCS_ABORTED_BY_COMMAND      = 0x01,
	KCS_ILLEGAL_CONTROL_CODE    = 0x02,
	KCS_LENGTH_ERROR            = 0x06,
	KCS_UNSPECIFIED_ERROR       = 0xFF
};

#define KCS_ZERO_DATA     0

/* IPMI 2.0 - Table 9-1, KCS Interface Status Register Bits */
#define KCS_STATUS_STATE(state) (state << 6)
#define KCS_STATUS_STATE_MASK   GENMASK(7, 6)
#define KCS_STATUS_CMD_DAT      BIT(3)
#define KCS_STATUS_SMS_ATN      BIT(2)
#define KCS_STATUS_IBF          BIT(1)
#define KCS_STATUS_OBF          BIT(0)

/* IPMI 2.0 - Table 9-2, KCS Interface State Bits */
enum kcs_states {
	IDLE_STATE  = 0,
	READ_STATE  = 1,
	WRITE_STATE = 2,
	ERROR_STATE = 3,
};

/* IPMI 2.0 - Table 9-3, KCS Interface Control Codes */
#define KCS_CMD_GET_STATUS_ABORT  0x60
#define KCS_CMD_WRITE_START       0x61
#define KCS_CMD_WRITE_END         0x62
#define KCS_CMD_READ_BYTE         0x68

#endif /* __IPMI_KCS_H__ */
