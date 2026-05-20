/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2026 Intel Corporation
 */

#ifndef _XE_SYSCTRL_MAILBOX_TYPES_H_
#define _XE_SYSCTRL_MAILBOX_TYPES_H_

#include <linux/types.h>

#include "abi/xe_sysctrl_abi.h"

/**
 * enum xe_sysctrl_group - System Controller command groups
 *
 * @XE_SYSCTRL_GROUP_GFSP: GFSP group
 */
enum xe_sysctrl_group {
	XE_SYSCTRL_GROUP_GFSP			= 0x01,
};

/**
 * enum xe_sysctrl_gfsp_cmd - Commands supported by GFSP group
 *
 * @XE_SYSCTRL_CMD_GET_SOC_ERROR: Retrieve basic error information
 * @XE_SYSCTRL_CMD_GET_PENDING_EVENT: Retrieve pending event
 * @XE_SYSCTRL_CMD_PAGE_OFFLINE: Instruct firmware to offline/decline a page
 * @XE_SYSCTRL_CMD_GET_OFFLINE_LIST: Retrieve list of all offlined pages from flash
 * @XE_SYSCTRL_CMD_GET_OFFLINE_QUEUE: Retrieve list of offlined queued pages from firmware
 */
enum xe_sysctrl_gfsp_cmd {
	XE_SYSCTRL_CMD_GET_SOC_ERROR		= 0x01,
	XE_SYSCTRL_CMD_GET_PENDING_EVENT	= 0x07,
	XE_SYSCTRL_CMD_PAGE_OFFLINE		= 0x08,
	XE_SYSCTRL_CMD_GET_OFFLINE_LIST		= 0x09,
	XE_SYSCTRL_CMD_GET_OFFLINE_QUEUE	= 0x0A,
};

/**
 * struct xe_sysctrl_mailbox_command - System Controller mailbox command
 */
struct xe_sysctrl_mailbox_command {
	/** @header: Application message header containing command information */
	struct xe_sysctrl_app_msg_hdr header;

	/** @data_in: Pointer to input payload data (can be NULL if no input data) */
	void *data_in;

	/** @data_in_len: Size of input payload in bytes (0 if no input data) */
	size_t data_in_len;

	/** @data_out: Pointer to output buffer for response data (can be NULL if no response) */
	void *data_out;

	/** @data_out_len: Size of output buffer in bytes (0 if no response expected) */
	size_t data_out_len;
};

/* Modify as needed */
#define XE_SYSCTRL_FLOOD			16

#define XE_SYSCTRL_MB_FRAME_SIZE	16
#define XE_SYSCTRL_MB_MAX_FRAMES	64
#define XE_SYSCTRL_MB_MAX_MESSAGE_SIZE	\
	(XE_SYSCTRL_MB_FRAME_SIZE * XE_SYSCTRL_MB_MAX_FRAMES)

#define XE_SYSCTRL_MB_DEFAULT_TIMEOUT_MS	500

#endif
