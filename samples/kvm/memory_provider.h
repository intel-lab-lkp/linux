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

/* ioctl on a provider fd: read the file's state, for tests. */
struct mp_stats {
	__u64 size;
	__u64 absent_pages;	/* pages taken away by MP_SET_PRESENT */
	__u64 readonly_pages;	/* pages marked read-only by MP_SET_READONLY */
};

#define MP_GET_STATS \
	_IOR(MP_IOCTL_BASE, 10, struct mp_stats)

/*
 * ioctl on a provider fd: SET_READONLY on [@offset, @offset + @len).
 * @value is the read-only value (1 = read-only, 0 = writable), and @offset
 * and @len are a byte range within the file, page aligned.
 */
struct mp_set_readonly {
	__u32 value;
	__u32 pad;
	__u64 offset;	/* bytes into the file, page aligned */
	__u64 len;	/* bytes, page aligned */
};

#define MP_SET_READONLY \
	_IOW(MP_IOCTL_BASE, 11, struct mp_set_readonly)

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
