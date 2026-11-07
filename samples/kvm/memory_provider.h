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

/*
 * ioctl on a provider fd: SET_PRESENT on [@offset, @offset + @len).
 * @value is the present value (1 = present, 0 = absent), and @offset
 * and @len are a byte range within the file, page aligned.
 */
struct mp_set_present {
	__u32 value;
	__u32 pad;
	__u64 offset;	/* bytes into the file, page aligned */
	__u64 len;	/* bytes, page aligned */
};

#define MP_SET_PRESENT \
	_IOW(MP_IOCTL_BASE, 12, struct mp_set_present)

#endif /* _SAMPLES_KVM_MEMORY_PROVIDER_H */
