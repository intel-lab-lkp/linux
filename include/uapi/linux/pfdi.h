/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/*
 * Arm Platform Fault Detection Interface (PFDI) userspace ABI
 *
 * Copyright (C) 2026 Arm Ltd.
 */

#ifndef _UAPI_LINUX_PFDI_H
#define _UAPI_LINUX_PFDI_H

#include <linux/ioctl.h>
#include <linux/types.h>

/*
 * SMC function IDs for PFDI v1.0.
 * These follow the SMCCC fast-call, 64-bit, Standard Secure Service encoding.
 */
#define PFDI_FN_BASE			0xC40002D0U
#define PFDI_FN(n)			(PFDI_FN_BASE + (n))

#define PFDI_FN_VERSION			PFDI_FN(0x00)
#define PFDI_FN_FEATURES		PFDI_FN(0x01)
#define PFDI_FN_PE_TEST_ID		PFDI_FN(0x02)
#define PFDI_FN_PE_TEST_PART_COUNT	PFDI_FN(0x03)
#define PFDI_FN_PE_TEST_RUN		PFDI_FN(0x04)
#define PFDI_FN_PE_TEST_RESULT		PFDI_FN(0x05)
#define PFDI_FN_FW_CHECK		PFDI_FN(0x06)
#define PFDI_FN_FORCE_ERROR		PFDI_FN(0x07)

#define PFDI_VERSION_MAJOR_SHIFT	16
#define PFDI_VERSION_MAJOR_MASK		0x7FFFU
#define PFDI_VERSION_MINOR_MASK		0xFFFFU

#define PFDI_VERSION_MAJOR(ver) \
	(((ver) >> PFDI_VERSION_MAJOR_SHIFT) & PFDI_VERSION_MAJOR_MASK)
#define PFDI_VERSION_MINOR(ver)		((ver) & PFDI_VERSION_MINOR_MASK)
#define PFDI_VERSION(maj, min) \
	((((maj) & PFDI_VERSION_MAJOR_MASK) << PFDI_VERSION_MAJOR_SHIFT) | \
	 ((min) & PFDI_VERSION_MINOR_MASK))

/* PFDI return values */
#define PFDI_RET_TEST_COUNT_ZERO	(-8)
#define PFDI_RET_UNKNOWN		(-7)
#define PFDI_RET_NOT_RUN		(-6)
#define PFDI_RET_ERROR			(-5)
#define PFDI_RET_FAULT_FOUND		(-4)
#define PFDI_RET_INVALID_PARAM		(-3)
#ifndef SMCCC_RET_NOT_SUPPORTED
#define SMCCC_RET_NOT_SUPPORTED		(-1)
#endif
#ifndef SMCCC_RET_SUCCESS
#define SMCCC_RET_SUCCESS		0
#endif

struct pfdi_test_range {
	__s64 start;
	__s64 end;
};

struct pfdi_test_run {
	struct pfdi_test_range range;
	__s64 ft_id;
};

struct pfdi_force_error {
	__u32 smc_fid;
	/* Keep the ioctl layout identical for native and compat userspace. */
	__u32 reserved;
	__s64 error_id;
};

#define PFDI_IOCTL_VERSION \
	_IOR('J', 1, __u32)
#define PFDI_IOCTL_FEATURES \
	_IOW('J', 2, __u32)
#define PFDI_IOCTL_PE_TEST_ID \
	_IOR('J', 3, __u64)
#define PFDI_IOCTL_PE_TEST_PART_COUNT \
	_IOR('J', 4, __s64)
#define PFDI_IOCTL_PE_TEST_RUN \
	_IOWR('J', 5, struct pfdi_test_run)
#define PFDI_IOCTL_PE_TEST_RESULT \
	_IOR('J', 6, __s64)
#define PFDI_IOCTL_FW_CHECK \
	_IO('J', 7)
#define PFDI_IOCTL_FORCE_ERROR \
	_IOW('J', 8, struct pfdi_force_error)

#endif /* _UAPI_LINUX_PFDI_H */
