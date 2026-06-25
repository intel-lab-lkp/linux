/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 KylinSoft Corporation.
 * Author: Chenghao Duan <duanchenghao@kylinos.cn>
 */

#ifndef _LINUX_KHO_ABI_EVENTFD_H
#define _LINUX_KHO_ABI_EVENTFD_H

#include <linux/types.h>

/*
 * Eventfd Live Update ABI
 *
 * This header defines the ABI for preserving eventfd state across kexec.
 *
 * The state is serialized into a packed structure `struct eventfd_luo_ser`
 * which is handed over to the next kernel via the KHO mechanism.
 *
 */

/**
 * struct eventfd_luo_ser - Serialized state of an eventfd
 * @count: The current counter value
 * @flags: File flags (EFD_SEMAPHORE, EFD_CLOEXEC, EFD_NONBLOCK)
 *
 * This structure contains the minimal state needed to restore an eventfd
 * after kexec. The count represents the current value of the event counter,
 * and flags represent the file creation flags.
 */
struct eventfd_luo_ser {
	__u64 count;
	unsigned int  flags;
} __packed;

/* The compatibility string for eventfd file handler */
#define EVENTFD_LUO_FH_COMPATIBLE	"eventfd-v1"

#endif /* _LINUX_KHO_ABI_EVENTFD_H */
