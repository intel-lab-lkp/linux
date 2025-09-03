/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef _LINUX_GUNYAH_ERRNO_H
#define _LINUX_GUNYAH_ERRNO_H

#include <linux/errno.h>

enum gunyah_error {
	GUNYAH_ERROR_OK				= 0,
	GUNYAH_ERROR_UNIMPLEMENTED		= -1,
	GUNYAH_ERROR_RETRY			= -2,

	GUNYAH_ERROR_ARG_INVAL			= 1,
	GUNYAH_ERROR_ARG_SIZE			= 2,
	GUNYAH_ERROR_ARG_ALIGN			= 3,

	GUNYAH_ERROR_NOMEM			= 10,

	GUNYAH_ERROR_ADDR_OVFL			= 20,
	GUNYAH_ERROR_ADDR_UNFL			= 21,
	GUNYAH_ERROR_ADDR_INVAL			= 22,

	GUNYAH_ERROR_DENIED			= 30,
	GUNYAH_ERROR_BUSY			= 31,
	GUNYAH_ERROR_IDLE			= 32,

	GUNYAH_ERROR_IRQ_BOUND			= 40,
	GUNYAH_ERROR_IRQ_UNBOUND		= 41,

	GUNYAH_ERROR_CSPACE_CAP_NULL		= 50,
	GUNYAH_ERROR_CSPACE_CAP_REVOKED		= 51,
	GUNYAH_ERROR_CSPACE_WRONG_OBJ_TYPE	= 52,
	GUNYAH_ERROR_CSPACE_INSUF_RIGHTS	= 53,
	GUNYAH_ERROR_CSPACE_FULL		= 54,

	GUNYAH_ERROR_MSGQUEUE_EMPTY		= 60,
	GUNYAH_ERROR_MSGQUEUE_FULL		= 61,
};

/**
 * gunyah_error_remap() - Remap Gunyah hypervisor errors into a Linux error code
 * @gunyah_error: Gunyah hypercall return value
 */
static inline int gunyah_error_remap(enum gunyah_error gunyah_error)
{
	switch (gunyah_error) {
	case GUNYAH_ERROR_OK:
		return 0;
	case GUNYAH_ERROR_NOMEM:
		return -ENOMEM;
	case GUNYAH_ERROR_DENIED:
	case GUNYAH_ERROR_CSPACE_CAP_NULL:
	case GUNYAH_ERROR_CSPACE_CAP_REVOKED:
	case GUNYAH_ERROR_CSPACE_WRONG_OBJ_TYPE:
	case GUNYAH_ERROR_CSPACE_INSUF_RIGHTS:
	case GUNYAH_ERROR_CSPACE_FULL:
		return -EACCES;
	case GUNYAH_ERROR_BUSY:
	case GUNYAH_ERROR_IDLE:
		return -EBUSY;
	case GUNYAH_ERROR_IRQ_BOUND:
	case GUNYAH_ERROR_IRQ_UNBOUND:
	case GUNYAH_ERROR_MSGQUEUE_FULL:
	case GUNYAH_ERROR_MSGQUEUE_EMPTY:
		return -EIO;
	case GUNYAH_ERROR_UNIMPLEMENTED:
	case GUNYAH_ERROR_RETRY:
		return -EOPNOTSUPP;
	default:
		return -EINVAL;
	}
}

#endif
