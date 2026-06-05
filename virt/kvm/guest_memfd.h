/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __KVM_GUEST_MEMFD_H__
#define __KVM_GUEST_MEMFD_H__ 1

#include <linux/kvm_host.h>
#include <linux/fs.h>
#include <linux/mempolicy.h>

/*
 * A guest_memfd instance can be associated multiple VMs, each with its own
 * "view" of the underlying physical memory.
 *
 * The gmem's inode is effectively the raw underlying physical storage, and is
 * used to track properties of the physical memory, while each gmem file is
 * effectively a single VM's view of that storage, and is used to track assets
 * specific to its associated VM, e.g. memslots=>gmem bindings.
 */
struct gmem_file {
	struct kvm *kvm;
	struct xarray bindings;
	struct list_head entry;
};

struct gmem_inode {
	struct shared_policy policy;
	struct inode vfs_inode;
	struct list_head gmem_file_list;

	u64 flags;
};

static inline struct gmem_inode *GMEM_I(struct inode *inode)
{
	return container_of(inode, struct gmem_inode, vfs_inode);
}

struct file *__kvm_gmem_create_file(struct kvm *kvm, loff_t size, u64 flags);

#endif /* __KVM_GUEST_MEMFD_H__ */
