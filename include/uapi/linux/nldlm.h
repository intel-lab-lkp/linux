/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause) */
/* Do not edit directly, auto-generated from: */
/*	Documentation/netlink/specs/nldlm.yaml */
/* YNL-GEN uapi header */

#ifndef _UAPI_LINUX_NLDLM_H
#define _UAPI_LINUX_NLDLM_H

#define NLDLM_FAMILY_NAME	"nldlm"
#define NLDLM_FAMILY_VERSION	1

/**
 * enum nldlm_protocol - The transport layer protocol that DLM will use.
 */
enum nldlm_protocol {
	NLDLM_PROTOCOL_TCP,
	NLDLM_PROTOCOL_SCTP,
};

/**
 * enum nldlm_log_level - The internal DLM logger log-level.
 * @NLDLM_LOG_LEVEL_NONE: disable all DLM logging
 * @NLDLM_LOG_LEVEL_INFO: enable INFO log-level only
 * @NLDLM_LOG_LEVEL_DEBUG: enable INFO and DEBUG log-level
 */
enum nldlm_log_level {
	NLDLM_LOG_LEVEL_NONE,
	NLDLM_LOG_LEVEL_INFO,
	NLDLM_LOG_LEVEL_DEBUG,
};

/**
 * enum nldlm_ls_ctrl_action - The lockspace control actions. Use in
 *   combination with ls-add-member and ls-del-member. First stop lockspace
 *   then update the members, then start the lockspace to trigger recovery with
 *   new membership updates.
 * @NLDLM_LS_CTRL_ACTION_STOP: stopping the lockspace
 * @NLDLM_LS_CTRL_ACTION_START: starting the lockspace
 */
enum nldlm_ls_ctrl_action {
	NLDLM_LS_CTRL_ACTION_STOP,
	NLDLM_LS_CTRL_ACTION_START,
};

/**
 * enum nldlm_ls_event_result - The result op of the lockspace event ntf-new-ls
 *   and ntf-release-ls. The kernel is waiting for this reply when those events
 *   occur.
 * @NLDLM_LS_EVENT_RESULT_SUCCESS: the DLM kernel lockspace event action was
 *   successful
 * @NLDLM_LS_EVENT_RESULT_FAILURE: the DLM kernel lockspace event action failed
 */
enum nldlm_ls_event_result {
	NLDLM_LS_EVENT_RESULT_SUCCESS,
	NLDLM_LS_EVENT_RESULT_FAILURE,
};

enum {
	NLDLM_A_CFG_OUR_NODEID = 1,
	NLDLM_A_CFG_CLUSTER_NAME,
	NLDLM_A_CFG_PROTOCOL,
	NLDLM_A_CFG_PORT,
	NLDLM_A_CFG_RECOVER_TIMEOUT,
	NLDLM_A_CFG_INACTIVE_TIMEOUT,
	NLDLM_A_CFG_LOG_LEVEL,
	NLDLM_A_CFG_DEFAULT_MARK,
	NLDLM_A_CFG_RECOVER_CALLBACKS,

	__NLDLM_A_CFG_MAX,
	NLDLM_A_CFG_MAX = (__NLDLM_A_CFG_MAX - 1)
};

enum {
	NLDLM_A_LS_NAME = 1,

	__NLDLM_A_LS_MAX,
	NLDLM_A_LS_MAX = (__NLDLM_A_LS_MAX - 1)
};

enum {
	NLDLM_A_LS_MEMBER_LS_NAME = 1,
	NLDLM_A_LS_MEMBER_NODEID,
	NLDLM_A_LS_MEMBER_WEIGHT,

	__NLDLM_A_LS_MEMBER_MAX,
	NLDLM_A_LS_MEMBER_MAX = (__NLDLM_A_LS_MEMBER_MAX - 1)
};

enum {
	NLDLM_A_LS_CTRL_LS_NAME = 1,
	NLDLM_A_LS_CTRL_ACTION,

	__NLDLM_A_LS_CTRL_MAX,
	NLDLM_A_LS_CTRL_MAX = (__NLDLM_A_LS_CTRL_MAX - 1)
};

enum {
	NLDLM_A_LS_EVENT_RESULT_LS_NAME = 1,
	NLDLM_A_LS_EVENT_RESULT_LS_GLOBAL_ID,
	NLDLM_A_LS_EVENT_RESULT_RESULT,

	__NLDLM_A_LS_EVENT_RESULT_MAX,
	NLDLM_A_LS_EVENT_RESULT_MAX = (__NLDLM_A_LS_EVENT_RESULT_MAX - 1)
};

enum {
	NLDLM_A_NODE_ID = 1,
	NLDLM_A_NODE_MARK,
	NLDLM_A_NODE_ADDRS,

	__NLDLM_A_NODE_MAX,
	NLDLM_A_NODE_MAX = (__NLDLM_A_NODE_MAX - 1)
};

enum {
	NLDLM_A_ADDR_FAMILY = 1,
	NLDLM_A_ADDR_ADDR4,
	NLDLM_A_ADDR_ADDR6,

	__NLDLM_A_ADDR_MAX,
	NLDLM_A_ADDR_MAX = (__NLDLM_A_ADDR_MAX - 1)
};

enum {
	NLDLM_CMD_GET_NODE = 1,
	NLDLM_CMD_ADD_NODE,
	NLDLM_CMD_DEL_NODE,
	NLDLM_CMD_GET_LS,
	NLDLM_CMD_GET_LS_MEMBER,
	NLDLM_CMD_LS_ADD_MEMBER,
	NLDLM_CMD_LS_DEL_MEMBER,
	NLDLM_CMD_LS_CTRL,
	NLDLM_CMD_NTF_NEW_LS,
	NLDLM_CMD_NTF_RELEASE_LS,
	NLDLM_CMD_LS_EVENT_DONE,
	NLDLM_CMD_GET_CFG,
	NLDLM_CMD_SET_OUR_NODEID,
	NLDLM_CMD_SET_CLUSTER_NAME,
	NLDLM_CMD_SET_PROTOCOL,
	NLDLM_CMD_SET_PORT,
	NLDLM_CMD_SET_RECOVER_TIMEOUT,
	NLDLM_CMD_SET_INACTIVE_TIMEOUT,
	NLDLM_CMD_SET_LOG_LEVEL,
	NLDLM_CMD_SET_DEFAULT_MARK,
	NLDLM_CMD_SET_RECOVER_CALLBACKS,

	__NLDLM_CMD_MAX,
	NLDLM_CMD_MAX = (__NLDLM_CMD_MAX - 1)
};

#define NLDLM_MCGRP_LS_EVENT	"ls-event"

#endif /* _UAPI_LINUX_NLDLM_H */
