/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SAMPLES_KVM_MEM_PROVIDER_SAMPLE_H
#define _SAMPLES_KVM_MEM_PROVIDER_SAMPLE_H

#include <linux/ioctl.h>
#include <linux/types.h>

/*
 * A sample memory provider.
 *
 * Every fd that this module returns is a provider file. Pass it to
 * KVM_CREATE_GUEST_MEMFD.
 */

#define MPS_IOCTL_BASE		'P'

/*
 * ioctl on /dev/mem_provider_sample: create a new provider fd of @size.
 * Returns the provider fd.
 */
struct mps_new_file {
	__u32 flags;	/* Reserved, must be 0 */
	__u32 pad;
	__u64 size;	/* bytes, page aligned */
};

#define MPS_NEW_FILE \
	_IOW(MPS_IOCTL_BASE, 5, struct mps_new_file)

/* ioctl on a provider fd: read the file's state, for tests. */
struct mps_stats {
	__u64 size;
	__u64 absent_pages;	/* pages taken away by MPS_SET_PRESENT */
};

#define MPS_GET_STATS \
	_IOR(MPS_IOCTL_BASE, 10, struct mps_stats)

/*
 * ioctl on a provider fd: SET_PRESENT on [@offset, @offset + @len).
 * @value is the present value (1 = present, 0 = absent), and @offset
 * and @len are a byte range within the file, page aligned.
 */
struct mps_set_present {
	__u32 value;
	__u32 pad;
	__u64 offset;	/* bytes into the file, page aligned */
	__u64 len;	/* bytes, page aligned */
};

#define MPS_SET_PRESENT \
	_IOW(MPS_IOCTL_BASE, 12, struct mps_set_present)

#endif /* _SAMPLES_KVM_MEM_PROVIDER_SAMPLE_H */
