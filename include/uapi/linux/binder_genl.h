/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause) */
/* Do not edit directly, auto-generated from: */
/*	Documentation/netlink/specs/binder_genl.yaml */
/* YNL-GEN uapi header */

#ifndef _UAPI_LINUX_BINDER_GENL_H
#define _UAPI_LINUX_BINDER_GENL_H

#define BINDER_GENL_FAMILY_NAME		"binder_genl"
#define BINDER_GENL_FAMILY_VERSION	1

/**
 * enum binder_genl_flag - Used with "set" and "reply" command below, defining
 *   what kind of binder transactions reported to the user space administration
 *   process.
 */
enum binder_genl_flag {
	BINDER_GENL_FLAG_FAILED = 1,
	BINDER_GENL_FLAG_DELAYED = 2,
	BINDER_GENL_FLAG_SPAM = 4,
	BINDER_GENL_FLAG_OVERRIDE = 8,
};

enum {
	BINDER_GENL_A_ATTR_PID = 1,
	BINDER_GENL_A_ATTR_FLAGS,
	BINDER_GENL_A_ATTR_REPORT,

	__BINDER_GENL_A_ATTR_MAX,
	BINDER_GENL_A_ATTR_MAX = (__BINDER_GENL_A_ATTR_MAX - 1)
};

enum {
	BINDER_GENL_CMD_SET = 1,
	BINDER_GENL_CMD_REPLY,
	BINDER_GENL_CMD_REPORT,

	__BINDER_GENL_CMD_MAX,
	BINDER_GENL_CMD_MAX = (__BINDER_GENL_CMD_MAX - 1)
};

#endif /* _UAPI_LINUX_BINDER_GENL_H */
