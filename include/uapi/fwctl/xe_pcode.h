/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2025 Intel Corporation
 */

#ifndef _UAPI_FWCTL_XE_PCODE_H_
#define _UAPI_FWCTL_XE_PCODE_H_

#include <linux/types.h>

/**
 * struct fwctl_info_xe_pcode - FWCTL Information struct for Xe PCODE
 *
 * @uctx_caps:  bitmap of available capabilities:
 *  - %FWCTL_XE_PCODE_LATEBINDING - Command to configure Late Bind FW such as
 * Fan Controller and Voltage Regulator
 * @rsvd: Reserved for future usage or flags
 */
struct fwctl_info_xe_pcode {
	__u32 uctx_caps;
	__u32 rsvd[3];
};

#define FWCTL_XE_PCODE_LATEBINDING	(1 << 0)

/**
 * struct fwctl_rpc_xe_pcode - FWCTL Remote Procedure Calls for Xe PCODE
 */
struct fwctl_rpc_xe_pcode {
	/** @command: The main Mailbox command */
	__u8 command;
	/** @param1: A subcommand or a parameter of the main command */
	__u16 param1;
	/** @param2: A parameter of a subcommand or a subsubcommand */
	__u16 param2;
	/** @data0: The first 32 bits of data. In general data-in as param */
	__u32 data0;
	/** @data1: The other 32 bits of data. In general data-out */
	__u32 data1;
	/** @pad: Padding the uAPI struct - Must be 0. Not sent to firmware */
	__u8 pad[3];
};

/**
 * DOC: Late Binding Commands
 *
 * FWCTL info.uctx_caps: FWCTL_XE_PCODE_LATEBINDING
 * FWCTL rpc.scope: FWCTL_RPC_CONFIGURATION
 *
 * Command	0x5C - LATE_BINDING
 * Param1	0x0 - GET_CAPABILITY_STATUS
 * Param2	0
 * Data in	None
 * Data out:
 *
 *  - Bit0: ate binding for V1 Fan Tables is supported.
 *  - Bit3: Late binding for VR parameters.
 *  - Bit16: Late binding done for V1 Fan tables
 *  - Bit17: Late binding done for power co-efficients.
 *  - Bit18: Late binding done for V2 Fan tables
 *  - Bit19: Late binding done for VR Parameters
 *
 * Command	0x5C - LATE_BINDING
 * Param1	0x1 - GET_VERSION_LOW
 * Param2	0
 * Data in - conveys the Type of the Late Binding Configuration:
 *
 *  - FAN_CONTROLLER = 1
 *  - VOLTAGE_REGULATOR = 2
 *
 * Data out - Lower 32 bits of Version Number for Late Binding configuration
 *            that has been applied successfully.
 */
#define PCODE_CMD_LATE_BINDING		0x5C
#define  PARAM1_GET_CAPABILITY_STATUS	0x0
#define  PARAM1_GET_VERSION_LOW		0x1
#define   DATA0_TYPE_FAN_CONTROLLER	1
#define   DATA0_TYPE_VOLTAGE_REGULATOR	2

#endif /* _UAPI_FWCTL_XE_PCODE_H_ */
