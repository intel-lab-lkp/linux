/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SAMPLES_KVM_MEMORY_PROVIDER_H
#define _SAMPLES_KVM_MEMORY_PROVIDER_H

#include <linux/ioctl.h>
#include <linux/types.h>

/*
 * A sample memory provider.
 *
 * Every fd that this module returns is a provider file. Pass it to
 * KVM_CREATE_GUEST_MEMFD.
 */

#define MP_IOCTL_BASE		'P'

/*
 * ioctl on /dev/memory_provider: create a new provider fd of @size.
 * Returns the provider fd.
 */
struct mp_new_file {
	__u32 flags;	/* Reserved, must be 0 */
	__u32 pad;
	__u64 size;	/* bytes, page aligned */
};

#define MP_NEW_FILE \
	_IOW(MP_IOCTL_BASE, 5, struct mp_new_file)

#endif /* _SAMPLES_KVM_MEMORY_PROVIDER_H */
