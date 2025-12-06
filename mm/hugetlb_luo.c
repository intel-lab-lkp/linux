// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2025 Amazon.com Inc. or its affiliates.
 * Copyright (C) 2025 Pratyush Yadav <pratyush@kernel.org>
 */

/* The documentation for this is in mm/memfd_luo.c */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/file.h>
#include <linux/liveupdate.h>
#include <linux/kexec_handover.h>
#include <linux/hugetlb.h>
#include <linux/hugetlb_cgroup.h>
#include <linux/vmalloc.h>
#include <linux/kho/abi/hugetlb.h>
#include <linux/spinlock.h>

#include "hugetlb_internal.h"
#include "hugetlb_vmemmap.h"

struct hugetlb_flb_obj {
	/* Serializes access to ser and its hstates. */
	spinlock_t lock;
	struct hugetlb_ser *ser;
};

struct hugemfd_private {
	struct hugemfd_folio_ser *folios_ser;
	unsigned long nr_folios;
};

static int hugetlb_flb_preserve(struct liveupdate_flb_op_args *args)
{
	struct hugetlb_ser *hugetlb_ser;
	struct hugetlb_flb_obj *obj;
	u8 nr_hstates = 0;
	struct hstate *h;

	obj = kmalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj)
		return -ENOMEM;

	hugetlb_ser = kho_alloc_preserve(sizeof(*hugetlb_ser));
	if (!hugetlb_ser) {
		kfree(obj);
		return -ENOMEM;
	}

	spin_lock_init(&obj->lock);
	obj->ser = hugetlb_ser;

	for_each_hstate(h) {
		struct hugetlb_hstate_ser *hser = &hugetlb_ser->hstates[nr_hstates];

		hser->nr_pages = 0;
		hser->order = h->order;
		nr_hstates++;
	}

	hugetlb_ser->nr_hstates = nr_hstates;

	args->obj = obj;
	args->data = virt_to_phys(hugetlb_ser);

	return 0;
}

static void hugetlb_flb_unpreserve(struct liveupdate_flb_op_args *args)
{
	kho_unpreserve_free(phys_to_virt(args->data));
	kfree(args->obj);
}

static void hugetlb_flb_finish(struct liveupdate_flb_op_args *args)
{
	/* No live state on the retrieve side. */
}

static int hugetlb_flb_retrieve(struct liveupdate_flb_op_args *args)
{
	/*
	 * The FLB is only needed for boot-time calculation of how many
	 * hugepages are needed. This is done by early boot handlers already.
	 * Free the serialized state now.
	 */
	kho_restore_free(phys_to_virt(args->data));

	/*
	 * HACK: But since LUO FLB still needs an obj, use ZERO_SIZE_PTR to
	 * satisfy it.
	 */
	args->obj = ZERO_SIZE_PTR;
	return 0;
}

static struct liveupdate_flb_ops hugetlb_luo_flb_ops = {
	.preserve = hugetlb_flb_preserve,
	.unpreserve = hugetlb_flb_unpreserve,
	.finish = hugetlb_flb_finish,
	.retrieve = hugetlb_flb_retrieve,
};

static struct liveupdate_flb hugetlb_luo_flb = {
	.ops = &hugetlb_luo_flb_ops,
	.compatible = HUGETLB_FLB_COMPATIBLE,
};

static struct hugetlb_hstate_ser
*hugetlb_flb_get_hser(struct hugetlb_ser *hugetlb_ser, unsigned int order)
{
	for (u8 i = 0; i < hugetlb_ser->nr_hstates; i++) {
		if (hugetlb_ser->hstates[i].order == order)
			return &hugetlb_ser->hstates[i];
	}

	return NULL;
}

static int hugetlb_flb_add_folio(struct hstate *h)
{
	struct hugetlb_ser *hugetlb_ser;
	struct hugetlb_hstate_ser *hser;
	struct hugetlb_flb_obj *obj;
	int err;

	err = liveupdate_flb_get_outgoing(&hugetlb_luo_flb, (void **)&obj);
	if (err)
		return err;

	hugetlb_ser = obj->ser;

	guard(spinlock)(&obj->lock);
	hser = hugetlb_flb_get_hser(hugetlb_ser, h->order);
	if (!hser)
		return -ENOENT;

	hser->nr_pages++;
	return 0;
}

static int hugetlb_flb_del_folio(struct hstate *h)
{
	struct hugetlb_ser *hugetlb_ser;
	struct hugetlb_hstate_ser *hser;
	struct hugetlb_flb_obj *obj;
	int err;

	err = liveupdate_flb_get_outgoing(&hugetlb_luo_flb, (void **)&obj);
	if (err)
		return err;

	hugetlb_ser = obj->ser;

	guard(spinlock)(&obj->lock);
	hser = hugetlb_flb_get_hser(hugetlb_ser, h->order);
	if (!hser)
		return -ENOENT;

	hser->nr_pages--;
	return 0;
}

unsigned long __init hstate_liveupdate_pages(struct hstate *h)
{
	struct hugetlb_hstate_ser *hser;
	struct hugetlb_ser *hugetlb_ser;
	u64 data;
	int err;

	err = liveupdate_flb_incoming_early(&hugetlb_luo_flb, &data);
	if (err)
		/* If FLB can't be fetched, assume no pages from liveupdate. */
		return 0;

	hugetlb_ser = phys_to_virt(data);

	/* NOTE: No need for locking since this is read-only on incoming side. */
	hser = hugetlb_flb_get_hser(hugetlb_ser, h->order);
	return hser ? hser->nr_pages : 0;
}

static bool hugemfd_can_preserve(struct liveupdate_file_handler *handler,
				 struct file *file)
{
	struct inode *inode = file_inode(file);

	return is_file_hugepages(file) && !inode->i_nlink;
}

static void hugemfd_unpreserve_folio(struct hstate *h, struct folio *folio)
{
	hugetlb_flb_del_folio(h);
	kho_unpreserve_folio(folio);
}

static int hugemfd_preserve_folio(struct hstate *h, struct folio *folio,
				  struct hugemfd_folio_ser *folio_ser)
{
	int err;

	err = kho_preserve_folio(folio);
	if (err)
		return err;

	err = hugetlb_flb_add_folio(h);
	if (err)
		goto err_unpreserve;

	folio_ser->pfn = folio_pfn(folio);
	folio_ser->index = folio->index;
	return 0;

err_unpreserve:
	kho_unpreserve_folio(folio);
	return err;
}

static int
hugemfd_preserve_folios(struct hugemfd_ser *memfd_ser, struct file *file,
			unsigned long *nr_foliosp,
			struct hugemfd_folio_ser **out_folios_ser)
{
	struct hugemfd_folio_ser *folios_ser;
	struct inode *inode = file_inode(file);
	struct hstate *h = hstate_inode(inode);
	unsigned int max_folios;
	long i, nr_folios, size;
	struct folio **folios;
	pgoff_t offset;
	int err;

	size = i_size_read(inode);

	if (!size) {
		*nr_foliosp = 0;
		*out_folios_ser = NULL;
		memset(&memfd_ser->folios, 0, sizeof(memfd_ser->folios));
		return 0;
	}

	/* Calculate number of folios in the file based on its size. */
	max_folios = size / huge_page_size(h);
	folios = kvmalloc_array(max_folios, sizeof(*folios), GFP_KERNEL);
	if (!folios)
		return -ENOMEM;

	/*
	 * Pin the folios so they don't move around behind our back. This also
	 * ensures none of the folios are in CMA -- which ensures they don't
	 * fall in KHO scratch memory. It also moves swapped out folios back to
	 * memory.
	 *
	 * A side effect of doing this is that it allocates a folio for all
	 * indices in the file. This might waste memory on sparse memfds. If
	 * that is really a problem in the future, we can have a
	 * memfd_pin_folios() variant that does not allocate a page on empty
	 * slots.
	 */
	nr_folios = memfd_pin_folios(file, 0, size - 1, folios, max_folios, &offset);
	if (nr_folios < 0) {
		err = nr_folios;
		goto err_free_folios;
	}

	folios_ser = vcalloc(nr_folios, sizeof(*folios_ser));
	if (!folios_ser) {
		err = -ENOMEM;
		goto err_unpin;
	}

	for (i = 0; i < nr_folios; i++) {
		err = hugemfd_preserve_folio(h, folios[i], &folios_ser[i]);
		if (err)
			goto err_unpreserve;
	}

	err = kho_preserve_vmalloc(folios_ser, &memfd_ser->folios);
	if (err)
		goto err_unpreserve;

	kvfree(folios);

	memfd_ser->nr_folios = nr_folios;
	*nr_foliosp = nr_folios;
	*out_folios_ser = folios_ser;
	return 0;

err_unpreserve:
	for (i = i - 1; i >= 0; i--)
		hugemfd_unpreserve_folio(h, folios[i]);
	vfree(folios_ser);
err_unpin:
	unpin_folios(folios, nr_folios);
err_free_folios:
	kvfree(folios);
	return err;
}

static int hugemfd_preserve(struct liveupdate_file_op_args *args)
{
	struct file *file = args->file;
	struct inode *inode = file_inode(file);
	struct hstate *h = hstate_inode(inode);
	struct hugemfd_folio_ser *folios_ser;
	struct hugemfd_private *private;
	struct hugemfd_ser *memfd_ser;
	unsigned long nr_folios;
	int err;

	private = kmalloc(sizeof(*private), GFP_KERNEL);
	if (!private)
		return -ENOMEM;

	memfd_ser = kho_alloc_preserve(sizeof(*memfd_ser));
	if (!memfd_ser) {
		err = -ENOMEM;
		goto err_free_private;
	}

	inode_lock(inode);

	hugetlb_i_freeze(inode, true);

	memfd_ser->size = i_size_read(inode);
	memfd_ser->pos = file->f_pos;
	memfd_ser->order = h->order;

	err = hugemfd_preserve_folios(memfd_ser, file, &nr_folios, &folios_ser);
	if (err)
		goto err_unlock;

	inode_unlock(inode);

	private->folios_ser = folios_ser;
	private->nr_folios = nr_folios;
	args->private_data = private;
	args->serialized_data = virt_to_phys(memfd_ser);

	return 0;

err_unlock:
	hugetlb_i_freeze(inode, false);
	inode_unlock(inode);
	kho_unpreserve_free(memfd_ser);
err_free_private:
	kfree(private);
	return err;
}

static void hugemfd_unpreserve_folios(struct hugemfd_ser *memfd_ser,
				      struct hugemfd_folio_ser *folios_ser,
				      unsigned long nr_folios,
				      struct hstate *h)
{
	if (!nr_folios)
		return;

	kho_unpreserve_vmalloc(&memfd_ser->folios);

	for (long i = 0; i < nr_folios; i++) {
		struct folio *folio = pfn_folio(folios_ser[i].pfn);

		hugemfd_unpreserve_folio(h, folio);
		unpin_folio(folio);
	}

	vfree(folios_ser);
}

static void hugemfd_unpreserve(struct liveupdate_file_op_args *args)
{
	struct hugemfd_ser *memfd_ser = phys_to_virt(args->serialized_data);
	struct hugemfd_private *private = args->private_data;
	struct inode *inode = file_inode(args->file);
	struct hstate *h = hstate_inode(inode);

	inode_lock(inode);
	hugemfd_unpreserve_folios(memfd_ser, private->folios_ser,
				  private->nr_folios, h);
	hugetlb_i_freeze(inode, false);
	kho_unpreserve_free(memfd_ser);
	kfree(private);
	inode_unlock(inode);
}

static int hugemfd_freeze(struct liveupdate_file_op_args *args)
{
	struct hugemfd_ser *memfd_ser = phys_to_virt(args->serialized_data);

	/*
	 * The pos might have changed since prepare. Everything else stays the
	 * same.
	 */
	memfd_ser->pos = args->file->f_pos;
	return 0;
}

static void hugemfd_finish(struct liveupdate_file_op_args *args)
{
	struct hugemfd_ser *memfd_ser = phys_to_virt(args->serialized_data);
	struct hugemfd_folio_ser *folios_ser;
	LIST_HEAD(folio_list);
	struct hstate *h;

	if (args->retrieved)
		return;

	folios_ser = kho_restore_vmalloc(&memfd_ser->folios);
	if (WARN_ON_ONCE(!folios_ser))
		return;

	h = size_to_hstate(PAGE_SIZE << memfd_ser->order);
	if (!h) {
		pr_warn("no hstate found for order %u\n", memfd_ser->order);
		goto err_free_all;
	}

	/* Return the folios back to the hstate. */
	for (u64 i = 0; i < memfd_ser->nr_folios; i++) {
		struct folio *folio;

		folio = kho_restore_folio(PFN_PHYS(folios_ser[i].pfn));
		if (!folio)
			continue;

		if (!folio_ref_freeze(folio, 1)) {
			pr_warn("unexpected refcount on PFN 0x%lx\n",
				folio_pfn(folio));
			continue;
		}

		init_new_hugetlb_folio(folio);
		list_add(&folio->lru, &folio_list);
	}

	prep_and_add_allocated_folios(h, &folio_list);
	vfree(folios_ser);
	return;

err_free_all:
	for (u64 i = 0; i < memfd_ser->nr_folios; i++) {
		struct folio *folio;

		folio = kho_restore_folio(PFN_PHYS(folios_ser[i].pfn));
		if (folio)
			folio_put(folio);
	}
	vfree(folios_ser);
}

static int hugemfd_setup_rsrv(struct inode *inode)
{
	struct hstate *h = hstate_inode(inode);
	long chg, regions_needed, add = -1;
	/*
	 * NOTE: Setting up the reservations for the whole file works right now
	 * because during preserve all the folios are filled in when pinning.
	 * Whenever that changes, this needs to be updated as well.
	 */
	long from = 0, to = inode->i_size >> huge_page_shift(h);
	struct resv_map *resv_map;
	struct hugetlb_cgroup *h_cg = NULL;
	int err;

	resv_map = inode_resv_map(inode);
	chg = region_chg(resv_map, from, to, &regions_needed);
	if (chg < 0)
		return chg;

	if (hugetlb_cgroup_charge_cgroup_rsvd(hstate_index(h),
					      chg * pages_per_huge_page(h),
					      &h_cg) < 0) {
		err = -ENOMEM;
		goto err_region_abort;
	}

	/*
	 * No need for hugetlb_acct_memory() to update h->resv_huge_pages since
	 * the reserved pages we added here will get used immediately after in
	 * hugemfd_retrieve_folios().
	 *
	 * No need for subpool reservations as well since the memfds come from
	 * the internal mounts of hugetlbfs and that doesn't have subpools.
	 */
	add = region_add(resv_map, from, to, regions_needed, h, h_cg);
	if (add < 0) {
		err = add;
		goto err_uncharge_cgroup;
	}

	hugetlb_cgroup_put_rsvd_cgroup(h_cg);

	return 0;

err_uncharge_cgroup:
	hugetlb_cgroup_uncharge_cgroup_rsvd(hstate_index(h),
					    chg * pages_per_huge_page(h), h_cg);
err_region_abort:
	region_abort(resv_map, from, to, regions_needed);
	return err;
}

static struct folio *hugemfd_retrieve_folio(struct hugemfd_folio_ser *folio_ser)
{
	struct folio *folio;

	folio = kho_restore_folio(PFN_PHYS(folio_ser->pfn));
	if (!folio)
		return NULL;

	init_new_hugetlb_folio(folio);
	__folio_mark_uptodate(folio);
	folio_ref_freeze(folio, 1);

	return folio;
}

static void hugemfd_add_folios(struct hstate *h, struct list_head *folio_list)
{
	unsigned long flags;
	struct folio *folio, *tmp_f;

	/* Send list for bulk vmemmap optimization processing */
	hugetlb_vmemmap_optimize_folios(h, folio_list);

	spin_lock_irqsave(&hugetlb_lock, flags);
	list_for_each_entry_safe(folio, tmp_f, folio_list, lru) {
		account_new_hugetlb_folio(h, folio);
		folio_clear_hugetlb_freed(folio);
		list_move(&folio->lru, &h->hugepage_activelist);
	}
	spin_unlock_irqrestore(&hugetlb_lock, flags);
}

static int hugemfd_retrieve_folios(struct file *file,
				   struct hugemfd_ser *memfd_ser)
{
	struct hugemfd_folio_ser *folios_ser;
	struct inode *inode = file_inode(file);
	struct hstate *h = hstate_inode(inode);
	int err, hidx = hstate_index(h);
	gfp_t gfp = htlb_alloc_mask(h) | __GFP_RETRY_MAYFAIL;
	struct address_space *mapping;
	struct hugetlb_cgroup *h_cg;
	struct folio *folio;
	LIST_HEAD(list);
	u64 nr_folios;

	if (!memfd_ser->size)
		return 0;

	folios_ser = kho_restore_vmalloc(&memfd_ser->folios);
	if (!folios_ser)
		return -ENOMEM;

	nr_folios = memfd_ser->nr_folios;
	mapping = inode->i_mapping;

	/* First prepare the folios and add them to the hstate. */
	for (u64 i = 0; i < nr_folios; i++) {
		struct hugemfd_folio_ser *folio_ser = &folios_ser[i];

		folio = hugemfd_retrieve_folio(folio_ser);
		if (!folio) {
			err = -EINVAL;
			goto err_free_folios_ser;
		}

		list_add(&folio->lru, &list);
	}

	hugemfd_add_folios(h, &list);

	/* Now that all the folios are prepared, add them to the file. */
	for (u64 i = 0; i < nr_folios; i++) {
		folio = pfn_folio(folios_ser[i].pfn);
		folio_ref_unfreeze(folio, 1);

		err = hugetlb_add_to_page_cache(folio, mapping,
						folios_ser[i].index >> memfd_ser->order);
		if (err) {
			pr_err("failed to add to page cache: %pe\n", ERR_PTR(err));
			goto err_free_folios_ser;
		}

		spin_lock_irq(&hugetlb_lock);
		err = hugetlb_cgroup_charge_cgroup(hidx, pages_per_huge_page(h),
						   &h_cg);
		if (err) {
			spin_unlock_irq(&hugetlb_lock);
			folio_unlock(folio);
			goto err_free_folios_ser;
		}
		hugetlb_cgroup_commit_charge(hidx, pages_per_huge_page(h), h_cg, folio);
		spin_unlock_irq(&hugetlb_lock);

		err = mem_cgroup_charge_hugetlb(folio, gfp);
		if (err) {
			folio_unlock(folio);
			goto err_free_folios_ser;
		}

		folio_unlock(folio);
		folio_put(folio);
	}

	vfree(folios_ser);
	return 0;

err_free_folios_ser:
	/*
	 * NOTE: The folios of the file might be in use for DMA or other
	 * things. It is unsafe to free them. Leak them, and let userspace get
	 * the error code and decide what to do.
	 */
	vfree(folios_ser);
	return err;
}

/*
 * NOTE: Leaking the file in the error paths is intentional here. The memory
 * might be in use by devices, and it is unsafe to release it. Return the error
 * to userspace and let it decide how to recover, usually by rebooting the
 * system.
 */
static int hugemfd_retrieve(struct liveupdate_file_op_args *args)
{
	struct hugemfd_ser *memfd_ser;
	struct file *file;
	int err;

	memfd_ser = phys_to_virt(args->serialized_data);

	file = hugetlb_file_setup("", 0, VM_NORESERVE, HUGETLB_ANONHUGE_INODE,
				  memfd_ser->order + PAGE_SHIFT);
	if (IS_ERR(file)) {
		err = PTR_ERR(file);
		goto err_free_memfd_ser;
	}

	vfs_setpos(file, memfd_ser->pos, MAX_LFS_FILESIZE);
	file->f_inode->i_size = memfd_ser->size;

	err = hugemfd_setup_rsrv(file_inode(file));
	if (err)
		goto err_free_memfd_ser;

	if (memfd_ser->nr_folios) {
		err = hugemfd_retrieve_folios(file, memfd_ser);
		if (err)
			goto err_free_memfd_ser;
	}

	args->file = file;
	kho_restore_free(memfd_ser);
	return 0;

err_free_memfd_ser:
	kho_restore_free(memfd_ser);
	return err;
}

static const struct liveupdate_file_ops hugemfd_luo_ops = {
	.can_preserve = hugemfd_can_preserve,
	.preserve = hugemfd_preserve,
	.unpreserve = hugemfd_unpreserve,
	.freeze = hugemfd_freeze,
	.finish = hugemfd_finish,
	.retrieve = hugemfd_retrieve,
	.owner = THIS_MODULE,
};

static struct liveupdate_file_handler hugemfd_handler = {
	.ops = &hugemfd_luo_ops,
	.compatible = HUGE_MEMFD_COMPATIBLE,
};

void __init hugetlb_luo_init(void)
{
	int err;

	if (!liveupdate_enabled())
		return;

	err = liveupdate_register_file_handler(&hugemfd_handler);
	if (err) {
		pr_err("could not register file handler: %pe\n", ERR_PTR(err));
		return;
	}

	err = liveupdate_register_flb(&hugemfd_handler, &hugetlb_luo_flb);
	if (err) {
		pr_err("could not register hugetlb FLB handler: %pe\n", ERR_PTR(err));
		liveupdate_unregister_file_handler(&hugemfd_handler);
		return;
	}
}
