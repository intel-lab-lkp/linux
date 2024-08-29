// SPDX-License-Identifier: GPL-2.0
#include <linux/guest_memfd.h>
#include <linux/kvm_host.h>
#include <linux/pagemap.h>

#include "kvm_mm.h"

struct kvm_gmem {
	struct kvm *kvm;
	struct xarray bindings;
	struct list_head entry;
};

static inline struct kvm_gmem *inode_to_kvm_gmem(struct inode *inode)
{
	struct list_head *gmem_list = &inode->i_mapping->i_private_list;

	return list_first_entry_or_null(gmem_list, struct kvm_gmem, entry);
}

/**
 * folio_file_pfn - like folio_file_page, but return a pfn.
 * @folio: The folio which contains this index.
 * @index: The index we want to look up.
 *
 * Return: The pfn for this index.
 */
static inline kvm_pfn_t folio_file_pfn(struct folio *folio, pgoff_t index)
{
	return folio_pfn(folio) + (index & (folio_nr_pages(folio) - 1));
}

static int kvm_gmem_prepare_inaccessible(struct inode *inode, struct folio *folio)
{
#ifdef CONFIG_HAVE_KVM_ARCH_GMEM_PREPARE
	kvm_pfn_t pfn = folio_file_pfn(folio, 0);
	gfn_t gfn = slot->base_gfn + folio_index(folio) - slot->gmem.pgoff;
	int rc = kvm_arch_gmem_prepare(kvm, gfn, pfn, folio_order(folio));
	if (rc) {
		pr_warn_ratelimited("gmem: Failed to prepare folio for index %lx GFN %llx PFN %llx error %d.\n",
				    index, gfn, pfn, rc);
		return rc;
	}
#endif

	return 0;
}

static void __kvm_gmem_invalidate_begin(struct kvm_gmem *gmem, pgoff_t start,
				      pgoff_t end)
{
	bool flush = false, found_memslot = false;
	struct kvm_memory_slot *slot;
	struct kvm *kvm = gmem->kvm;
	unsigned long index;

	xa_for_each_range(&gmem->bindings, index, slot, start, end - 1) {
		pgoff_t pgoff = slot->gmem.pgoff;

		struct kvm_gfn_range gfn_range = {
			.start = slot->base_gfn + max(pgoff, start) - pgoff,
			.end = slot->base_gfn + min(pgoff + slot->npages, end) - pgoff,
			.slot = slot,
			.may_block = true,
		};

		if (!found_memslot) {
			found_memslot = true;

			KVM_MMU_LOCK(kvm);
			kvm_mmu_invalidate_begin(kvm);
		}

		flush |= kvm_mmu_unmap_gfn_range(kvm, &gfn_range);
	}

	if (flush)
		kvm_flush_remote_tlbs(kvm);

	if (found_memslot)
		KVM_MMU_UNLOCK(kvm);
}

static int kvm_gmem_invalidate_begin(struct inode *inode, pgoff_t offset, unsigned long nr)
{
	struct kvm_gmem *gmem;

	list_for_each_entry(gmem, &inode->i_mapping->i_private_list, entry)
		__kvm_gmem_invalidate_begin(gmem, offset, offset + nr);

	return 0;
}

static void __kvm_gmem_invalidate_end(struct kvm_gmem *gmem, pgoff_t start,
				    pgoff_t end)
{
	struct kvm *kvm = gmem->kvm;

	if (xa_find(&gmem->bindings, &start, end - 1, XA_PRESENT)) {
		KVM_MMU_LOCK(kvm);
		kvm_mmu_invalidate_end(kvm);
		KVM_MMU_UNLOCK(kvm);
	}
}

static void kvm_gmem_invalidate_end(struct inode *inode, pgoff_t offset, unsigned long nr)
{
	struct kvm_gmem *gmem;

	list_for_each_entry(gmem, &inode->i_mapping->i_private_list, entry)
		__kvm_gmem_invalidate_end(gmem, offset, offset + nr);
}

static void __kvm_gmem_release(struct inode *inode, struct kvm_gmem *gmem)
{
	struct kvm_memory_slot *slot;
	struct kvm *kvm = gmem->kvm;
	unsigned long index;

	/*
	 * Prevent concurrent attempts to *unbind* a memslot.  This is the last
	 * reference to the file and thus no new bindings can be created, but
	 * dereferencing the slot for existing bindings needs to be protected
	 * against memslot updates, specifically so that unbind doesn't race
	 * and free the memslot (kvm_gmem_get_file() will return NULL).
	 */
	mutex_lock(&kvm->slots_lock);

	filemap_invalidate_lock(inode->i_mapping);

	xa_for_each(&gmem->bindings, index, slot)
		rcu_assign_pointer(slot->gmem.file, NULL);

	synchronize_rcu();

	/*
	 * All in-flight operations are gone and new bindings can be created.
	 * Zap all SPTEs pointed at by this file.  Do not free the backing
	 * memory, as its lifetime is associated with the inode, not the file.
	 */
	__kvm_gmem_invalidate_begin(gmem, 0, -1ul);
	__kvm_gmem_invalidate_end(gmem, 0, -1ul);

	list_del(&gmem->entry);

	filemap_invalidate_unlock(inode->i_mapping);

	mutex_unlock(&kvm->slots_lock);

	xa_destroy(&gmem->bindings);
	kfree(gmem);

	kvm_put_kvm(kvm);
}

static int kvm_gmem_release(struct inode *inode)
{
	struct kvm_gmem *gmem;

	list_for_each_entry(gmem, &inode->i_mapping->i_private_list, entry)
		__kvm_gmem_release(inode, gmem);

	return 0;
}

static inline struct file *kvm_gmem_get_file(struct kvm_memory_slot *slot)
{
	/*
	 * Do not return slot->gmem.file if it has already been closed;
	 * there might be some time between the last fput() and when
	 * kvm_gmem_release() clears slot->gmem.file, and you do not
	 * want to spin in the meanwhile.
	 */
	return get_file_active(&slot->gmem.file);
}

static const struct guest_memfd_operations kvm_gmem_ops = {
	.invalidate_begin = kvm_gmem_invalidate_begin,
	.invalidate_end = kvm_gmem_invalidate_end,
	.prepare_inaccessible = kvm_gmem_prepare_inaccessible,
	.release = kvm_gmem_release,
};

static int __kvm_gmem_create(struct kvm *kvm, loff_t size, u64 flags)
{
	const char *anon_name = "[kvm-gmem]";
	struct kvm_gmem *gmem;
	struct inode *inode;
	struct file *file;
	int fd, err;

	fd = get_unused_fd_flags(0);
	if (fd < 0)
		return fd;

	gmem = kzalloc(sizeof(*gmem), GFP_KERNEL);
	if (!gmem) {
		err = -ENOMEM;
		goto err_fd;
	}

	file = guest_memfd_alloc(anon_name, &kvm_gmem_ops, size,
				 GUEST_MEMFD_FLAG_CLEAR_INACCESSIBLE);
	if (IS_ERR(file)) {
		err = PTR_ERR(file);
		goto err_gmem;
	}

	inode = file->f_inode;
	WARN_ON(file->f_mapping != inode->i_mapping);

	kvm_get_kvm(kvm);
	gmem->kvm = kvm;
	xa_init(&gmem->bindings);
	list_add(&gmem->entry, &inode->i_mapping->i_private_list);

	fd_install(fd, file);
	return fd;

err_gmem:
	kfree(gmem);
err_fd:
	put_unused_fd(fd);
	return err;
}

int kvm_gmem_create(struct kvm *kvm, struct kvm_create_guest_memfd *args)
{
	loff_t size = args->size;
	u64 flags = args->flags;
	u64 valid_flags = 0;

	if (flags & ~valid_flags)
		return -EINVAL;

	if (size <= 0 || !PAGE_ALIGNED(size))
		return -EINVAL;

	return __kvm_gmem_create(kvm, size, flags);
}

static inline struct kvm_gmem *file_to_kvm_gmem(struct file *file)
{
	if (!is_guest_memfd(file, &kvm_gmem_ops))
		return NULL;

	return inode_to_kvm_gmem(file_inode(file));
}

int kvm_gmem_bind(struct kvm *kvm, struct kvm_memory_slot *slot,
		  unsigned int fd, loff_t offset)
{
	loff_t size = slot->npages << PAGE_SHIFT;
	unsigned long start, end;
	struct kvm_gmem *gmem;
	struct inode *inode;
	struct file *file;
	int r = -EINVAL;

	BUILD_BUG_ON(sizeof(gfn_t) != sizeof(slot->gmem.pgoff));

	file = fget(fd);
	if (!file)
		return -EBADF;

	gmem = file_to_kvm_gmem(file);
	if (!gmem || gmem->kvm != kvm)
		goto err;

	inode = file_inode(file);

	if (offset < 0 || !PAGE_ALIGNED(offset) ||
	    offset + size > i_size_read(inode))
		goto err;

	filemap_invalidate_lock(inode->i_mapping);

	start = offset >> PAGE_SHIFT;
	end = start + slot->npages;

	if (!xa_empty(&gmem->bindings) &&
	    xa_find(&gmem->bindings, &start, end - 1, XA_PRESENT)) {
		filemap_invalidate_unlock(inode->i_mapping);
		goto err;
	}

	/*
	 * No synchronize_rcu() needed, any in-flight readers are guaranteed to
	 * be see either a NULL file or this new file, no need for them to go
	 * away.
	 */
	rcu_assign_pointer(slot->gmem.file, file);
	slot->gmem.pgoff = start;

	xa_store_range(&gmem->bindings, start, end - 1, slot, GFP_KERNEL);
	filemap_invalidate_unlock(inode->i_mapping);

	/*
	 * Drop the reference to the file, even on success.  The file pins KVM,
	 * not the other way 'round.  Active bindings are invalidated if the
	 * file is closed before memslots are destroyed.
	 */
	r = 0;
err:
	fput(file);
	return r;
}

void kvm_gmem_unbind(struct kvm_memory_slot *slot)
{
	unsigned long start = slot->gmem.pgoff;
	unsigned long end = start + slot->npages;
	struct kvm_gmem *gmem;
	struct file *file;

	/*
	 * Nothing to do if the underlying file was already closed (or is being
	 * closed right now), kvm_gmem_release() invalidates all bindings.
	 */
	file = kvm_gmem_get_file(slot);
	if (!file)
		return;

	gmem = file_to_kvm_gmem(file);
	if (WARN_ON_ONCE(!gmem))
		return;

	filemap_invalidate_lock(file->f_mapping);
	xa_store_range(&gmem->bindings, start, end - 1, NULL, GFP_KERNEL);
	rcu_assign_pointer(slot->gmem.file, NULL);
	synchronize_rcu();
	filemap_invalidate_unlock(file->f_mapping);

	fput(file);
}

/* Returns a locked folio on success.  */
static struct folio *
__kvm_gmem_get_pfn(struct file *file, struct kvm_memory_slot *slot,
		   gfn_t gfn, kvm_pfn_t *pfn, bool accessible,
		   int *max_order)
{
	pgoff_t index = gfn - slot->base_gfn + slot->gmem.pgoff;
	struct kvm_gmem *gmem = file->private_data;
	struct folio *folio;

	if (file != slot->gmem.file) {
		WARN_ON_ONCE(slot->gmem.file);
		return ERR_PTR(-EFAULT);
	}

	gmem = file_to_kvm_gmem(file);
	if (WARN_ON_ONCE(!gmem))
		return ERR_PTR(-EFAULT);

	if (xa_load(&gmem->bindings, index) != slot) {
		WARN_ON_ONCE(xa_load(&gmem->bindings, index));
		return ERR_PTR(-EIO);
	}

	if (accessible)
		grab_flags = GUEST_MEMFD_GRAB_ACCESSIBLE;
	else
		grab_flags = GUEST_MEMFD_GRAB_INACCESSIBLE;

	folio = guest_memfd_grab_folio(file, index, grab_flags);
	if (IS_ERR(folio))
		return folio;

	if (folio_test_hwpoison(folio)) {
		folio_unlock(folio);
		guest_memfd_put_folio(folio, accessible ? 1 : 0);
		return ERR_PTR(-EHWPOISON);
	}

	*pfn = folio_file_pfn(folio, index);
	if (max_order)
		*max_order = 0;

	return folio;
}

int kvm_gmem_get_pfn(struct kvm *kvm, struct kvm_memory_slot *slot,
		     gfn_t gfn, kvm_pfn_t *pfn, int *max_order)
{
	struct file *file = kvm_gmem_get_file(slot);
	struct folio *folio;
	int r = 0;

	if (!file)
		return -EFAULT;

	folio = __kvm_gmem_get_pfn(file, slot, gfn, pfn, false, max_order);
	if (IS_ERR(folio)) {
		r = PTR_ERR(folio);
		goto out;
	}

	folio_unlock(folio);
	if (r < 0)
		guest_memfd_put_folio(folio, 0);
	else
		guest_memfd_unsafe_folio(folio);

out:
	fput(file);
	return r;
}
EXPORT_SYMBOL_GPL(kvm_gmem_get_pfn);

#ifdef CONFIG_KVM_GENERIC_PRIVATE_MEM
long kvm_gmem_populate(struct kvm *kvm, gfn_t start_gfn, void __user *src, long npages,
		       kvm_gmem_populate_cb post_populate, void *opaque)
{
	struct file *file;
	struct kvm_memory_slot *slot;
	void __user *p;

	int ret = 0, max_order;
	long i;

	lockdep_assert_held(&kvm->slots_lock);
	if (npages < 0)
		return -EINVAL;

	slot = gfn_to_memslot(kvm, start_gfn);
	if (!kvm_slot_can_be_private(slot))
		return -EINVAL;

	file = kvm_gmem_get_file(slot);
	if (!file)
		return -EFAULT;

	filemap_invalidate_lock(file->f_mapping);

	npages = min_t(ulong, slot->npages - (start_gfn - slot->base_gfn), npages);
	for (i = 0; i < npages; i += (1 << max_order)) {
		struct folio *folio;
		gfn_t gfn = start_gfn + i;
		kvm_pfn_t pfn;

		if (signal_pending(current)) {
			ret = -EINTR;
			break;
		}

		folio = __kvm_gmem_get_pfn(file, slot, gfn, &pfn, true, &max_order);
		if (IS_ERR(folio)) {
			ret = PTR_ERR(folio);
			break;
		}

		folio_unlock(folio);
		WARN_ON(!IS_ALIGNED(gfn, 1 << max_order) ||
			(npages - i) < (1 << max_order));

		ret = -EINVAL;
		while (!kvm_range_has_memory_attributes(kvm, gfn, gfn + (1 << max_order),
							KVM_MEMORY_ATTRIBUTE_PRIVATE,
							KVM_MEMORY_ATTRIBUTE_PRIVATE)) {
			if (!max_order)
				goto put_folio_and_exit;
			max_order--;
		}

		p = src ? src + i * PAGE_SIZE : NULL;
		ret = post_populate(kvm, gfn, pfn, p, max_order, opaque);

put_folio_and_exit:
		guest_memfd_put_folio(folio, 1);
		if (ret)
			break;
	}

	filemap_invalidate_unlock(file->f_mapping);

	fput(file);
	return ret && !i ? ret : i;
}
EXPORT_SYMBOL_GPL(kvm_gmem_populate);
#endif
