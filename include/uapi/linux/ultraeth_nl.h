/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause) */
/* Do not edit directly, auto-generated from: */
/*	Documentation/netlink/specs/ultraeth.yaml */
/* YNL-GEN uapi header */

#ifndef _UAPI_LINUX_ULTRAETH_NL_H
#define _UAPI_LINUX_ULTRAETH_NL_H

#define ULTRAETH_FAMILY_NAME	"ultraeth"
#define ULTRAETH_FAMILY_VERSION	1

enum {
	ULTRAETH_A_CONTEXT_ID = 1,

	__ULTRAETH_A_CONTEXT_MAX,
	ULTRAETH_A_CONTEXT_MAX = (__ULTRAETH_A_CONTEXT_MAX - 1)
};

enum {
	ULTRAETH_A_CONTEXTS_CONTEXT = 1,

	__ULTRAETH_A_CONTEXTS_MAX,
	ULTRAETH_A_CONTEXTS_MAX = (__ULTRAETH_A_CONTEXTS_MAX - 1)
};

enum {
	ULTRAETH_CMD_CONTEXT_GET = 1,
	ULTRAETH_CMD_CONTEXT_NEW,
	ULTRAETH_CMD_CONTEXT_DEL,

	__ULTRAETH_CMD_MAX,
	ULTRAETH_CMD_MAX = (__ULTRAETH_CMD_MAX - 1)
};

#endif /* _UAPI_LINUX_ULTRAETH_NL_H */
