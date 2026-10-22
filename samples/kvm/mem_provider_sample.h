/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SAMPLES_KVM_MEM_PROVIDER_SAMPLE_H
#define _SAMPLES_KVM_MEM_PROVIDER_SAMPLE_H

#include <linux/ioctl.h>
#include <linux/types.h>

/*
 * A sample memory provider (include/linux/mem_provider.h).
 *
 * Every fd that this module returns is a provider file.  Pass it as
 * resource_fd to KVM_CREATE_GUEST_MEMFD with GUEST_MEMFD_FLAG_USE_RESOURCE,
 * and as fd to IOMMU_IOAS_MAP_FILE.  Both consumers follow every change that
 * the ioctls below make.
 */

/* Flags for struct mps_setup and struct mps_new_child */
/* The host may map the pages; otherwise they are NO_USER_MAP */
#define MPS_FLAG_MMAP_CAPABLE	(1u << 0)

/*
 * ioctl on /dev/mem_provider_sample: create a provider fd.
 *
 * If the module was loaded with addr= and len=, the fd covers that fixed
 * range, which has no struct page, and @size is ignored.  The fixed range
 * then has one owner.  SETUP fails with -EBUSY while a SETUP file uses it,
 * or once the first MPS_NEW_CHILD has made it the root of the children, which
 * lasts until the module is unloaded.  MPS_NEW_CHILD fails with -EBUSY while
 * a SETUP file uses it.  Otherwise the fd covers @size bytes from
 * alloc_contig_pages().
 */
struct mps_setup {
	__u32 flags;	/* MPS_FLAG_* */
	__u32 pad;
	__u64 size;	/* bytes, page aligned */
};

#define MPS_IOCTL_BASE		'P'
#define MPS_SETUP \
	_IOW(MPS_IOCTL_BASE, 1, struct mps_setup)

/*
 * ---- A tree of owners --------------------------------------------------
 *
 * The control device owns one region, the root.  NEW_CHILD carves part of
 * it into a new provider fd for one VM.  The owner can MOVE pages between
 * children, DONATE pages to the root, so that no child has them, and
 * RECLAIM them.  Each change revokes the range, so KVM and iommufd follow.
 *
 * The owner changes a child's pages through the control device
 * (CTL_SET_PRESENT).  A child fd is read-only to its holder: a VMM can
 * only GET_STATS, never change the memory.
 */

/*
 * ioctl on /dev/mem_provider_sample: carve [@offset, @offset + @len) of the
 * root into a new child fd.  The child gets every page of the range that no
 * other child has and that is not donated.  Ranges of children may overlap,
 * which is how a page can MOVE between them.  Returns the child fd.
 */
struct mps_new_child {
	__u32 flags;	/* MPS_FLAG_* */
	__u32 pad;
	__u64 offset;	/* bytes into the root, page aligned */
	__u64 len;	/* bytes, page aligned */
};

#define MPS_NEW_CHILD \
	_IOW(MPS_IOCTL_BASE, 5, struct mps_new_child)

/* ioctl on a child fd: read the child's state, for tests. */
struct mps_stats {
	__u64 region_offset;	/* the child's range in the root */
	__u64 region_len;
	__u64 owned_pages;	/* pages the child has */
	__u64 absent_pages;	/* pages taken away by CTL_SET_PRESENT */
};

#define MPS_GET_STATS \
	_IOR(MPS_IOCTL_BASE, 10, struct mps_stats)

/*
 * ioctl on /dev/mem_provider_sample: CTL_SET_PRESENT on child @fd, by the
 * owner. @value is the present value (1 = present, 0 = absent), and @offset
 * and @len are a byte range within the child, page aligned.
 */
struct mps_ctl_range {
	__s32 fd;
	__u32 value;
	__u64 offset;	/* bytes into the child, page aligned */
	__u64 len;	/* bytes, page aligned */
};

#define MPS_CTL_SET_PRESENT \
	_IOW(MPS_IOCTL_BASE, 12, struct mps_ctl_range)

#endif /* _SAMPLES_KVM_MEM_PROVIDER_SAMPLE_H */
