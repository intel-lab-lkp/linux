/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Intel On Demand (SDSi) Interface for SPDM based attestation.
 * Copyright (c) 2019, Intel Corporation.
 * All rights reserved.
 *
 * Author: David E. Box <david.e.box@linux.intel.com>
 */

#ifndef __SDSI_H
#define __SDSI_H

#include <linux/types.h>

/**
 * struct sdsi_spdm_info - Define platform information
 * @api_version:	Version of the firmware document, which this driver
 *			can communicate
 * @driver_version:	Driver version, which will help user to send right
 *			commands. Even if the firmware is capable, driver may
 *			not be ready
 * @dev_no:		Returns the auxiliary device number the corresponding
 *			sdsi instance
 * @max_request_size:	Returns the maximum allowed size for SPDM request
 *			messages
 * @max_response_size:	Returns the maximum size of an SPDM response message
 *
 * Used to return output of IOCTL SDSI_SPDM_INFO. This
 * information can be used by the user space, to get the driver, firmware
 * support and also number of commands to send in a single IOCTL request.
 */
struct sdsi_spdm_info {
	__u32 api_version;
	__u16 driver_version;
	__u16 dev_no;
	__u16 max_request_size;
	__u16 max_response_size;
};

/**
 * struct sdsi_spdm_message - The SPDM message sent and received from the device
 * @spdm_version:		Supported SPDM version
 * @request_response_code:	The SPDM message code for requests and responses
 * @param1:			Parameter 1
 * @param2:			Parameter 2
 * @buffer:			SDPM message specific buffer
 *
 */
struct sdsi_spdm_message {
	__u8 spdm_version;
	__u8 request_response_code;
	__u8 param1;
	__u8 param2;
	__u8 buffer[4092];
};

/**
 * struct sdsi_spdm_command - The SPDM command
 * @ size:		The size of the SPDM message
 * @ message:		The SPDM message
 *
 * Used to return output of IOCTL SDSI_SPDM_COMMAND.
 */
struct sdsi_spdm_command {
	__u32 size;
	struct sdsi_spdm_message message;
};

#define SDSI_IF_MAGIC		0xFC
#define SDSI_IF_SPDM_INFO	_IOR(SDSI_IF_MAGIC, 0, struct sdsi_spdm_info *)
#define SDSI_IF_SPDM_COMMAND	_IOWR(SDSI_IF_MAGIC, 1, struct sdsi_spdm_command *)
#endif
