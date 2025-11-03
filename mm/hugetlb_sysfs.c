// SPDX-License-Identifier: GPL-2.0-only
/*
 * HugeTLB sysfs interfaces.
 *
 * Copyright (C) 2025 KylinSoft Corporation.
 * Author: Geliang Tang <geliang@kernel.org>
 */

#include <linux/swap.h>
#include <linux/page_owner.h>
#include <linux/page-isolation.h>

#include "hugetlb_vmemmap.h"
#include "hugetlb_internal.h"

static long demote_free_hugetlb_folios(struct hstate *src, struct hstate *dst,
				       struct list_head *src_list)
{
	long rc;
	struct folio *folio, *next;
	LIST_HEAD(dst_list);
	LIST_HEAD(ret_list);

	rc = hugetlb_vmemmap_restore_folios(src, src_list, &ret_list);
	list_splice_init(&ret_list, src_list);

	/*
	 * Taking target hstate mutex synchronizes with set_max_huge_pages.
	 * Without the mutex, pages added to target hstate could be marked
	 * as surplus.
	 *
	 * Note that we already hold src->resize_lock.  To prevent deadlock,
	 * use the convention of always taking larger size hstate mutex first.
	 */
	mutex_lock(&dst->resize_lock);

	list_for_each_entry_safe(folio, next, src_list, lru) {
		int i;
		bool cma;

		if (folio_test_hugetlb_vmemmap_optimized(folio))
			continue;

		cma = folio_test_hugetlb_cma(folio);

		list_del(&folio->lru);

		split_page_owner(&folio->page, huge_page_order(src), huge_page_order(dst));
		pgalloc_tag_split(folio, huge_page_order(src), huge_page_order(dst));

		for (i = 0; i < pages_per_huge_page(src); i += pages_per_huge_page(dst)) {
			struct page *page = folio_page(folio, i);
			/* Careful: see __split_huge_page_tail() */
			struct folio *new_folio = (struct folio *)page;

			clear_compound_head(page);
			prep_compound_page(page, dst->order);

			new_folio->mapping = NULL;
			init_new_hugetlb_folio(new_folio);
			/* Copy the CMA flag so that it is freed correctly */
			if (cma)
				folio_set_hugetlb_cma(new_folio);
			list_add(&new_folio->lru, &dst_list);
		}
	}

	prep_and_add_allocated_folios(dst, &dst_list);

	mutex_unlock(&dst->resize_lock);

	return rc;
}

static long demote_pool_huge_page(struct hstate *src, nodemask_t *nodes_allowed,
				  unsigned long nr_to_demote)
	__must_hold(&hugetlb_lock)
{
	int nr_nodes, node;
	struct hstate *dst;
	long rc = 0;
	long nr_demoted = 0;

	lockdep_assert_held(&hugetlb_lock);

	/* We should never get here if no demote order */
	if (!src->demote_order) {
		pr_warn("HugeTLB: NULL demote order passed to demote_pool_huge_page.\n");
		return -EINVAL;		/* internal error */
	}
	dst = size_to_hstate(PAGE_SIZE << src->demote_order);

	for_each_node_mask_to_free(src, nr_nodes, node, nodes_allowed) {
		LIST_HEAD(list);
		struct folio *folio, *next;

		list_for_each_entry_safe(folio, next, &src->hugepage_freelists[node], lru) {
			if (folio_test_hwpoison(folio))
				continue;

			remove_hugetlb_folio(src, folio, false);
			list_add(&folio->lru, &list);

			if (++nr_demoted == nr_to_demote)
				break;
		}

		spin_unlock_irq(&hugetlb_lock);

		rc = demote_free_hugetlb_folios(src, dst, &list);

		spin_lock_irq(&hugetlb_lock);

		list_for_each_entry_safe(folio, next, &list, lru) {
			list_del(&folio->lru);
			add_hugetlb_folio(src, folio, false);

			nr_demoted--;
		}

		if (rc < 0 || nr_demoted == nr_to_demote)
			break;
	}

	/*
	 * Not absolutely necessary, but for consistency update max_huge_pages
	 * based on pool changes for the demoted page.
	 */
	src->max_huge_pages -= nr_demoted;
	dst->max_huge_pages += nr_demoted << (huge_page_order(src) - huge_page_order(dst));

	if (rc < 0)
		return rc;

	if (nr_demoted)
		return nr_demoted;
	/*
	 * Only way to get here is if all pages on free lists are poisoned.
	 * Return -EBUSY so that caller will not retry.
	 */
	return -EBUSY;
}

#define HSTATE_ATTR_RO(_name) \
	static struct kobj_attribute _name##_attr = __ATTR_RO(_name)

#define HSTATE_ATTR_WO(_name) \
	static struct kobj_attribute _name##_attr = __ATTR_WO(_name)

#define HSTATE_ATTR(_name) \
	static struct kobj_attribute _name##_attr = __ATTR_RW(_name)

static struct kobject *hugepages_kobj;
static struct kobject *hstate_kobjs[HUGE_MAX_HSTATE];

static struct hstate *kobj_to_node_hstate(struct kobject *kobj, int *nidp);

static struct hstate *kobj_to_hstate(struct kobject *kobj, int *nidp)
{
	int i;

	for (i = 0; i < HUGE_MAX_HSTATE; i++)
		if (hstate_kobjs[i] == kobj) {
			if (nidp)
				*nidp = NUMA_NO_NODE;
			return &hstates[i];
		}

	return kobj_to_node_hstate(kobj, nidp);
}

static ssize_t nr_hugepages_show_common(struct kobject *kobj,
					struct kobj_attribute *attr, char *buf)
{
	struct hstate *h;
	unsigned long nr_huge_pages;
	int nid;

	h = kobj_to_hstate(kobj, &nid);
	if (nid == NUMA_NO_NODE)
		nr_huge_pages = h->nr_huge_pages;
	else
		nr_huge_pages = h->nr_huge_pages_node[nid];

	return sysfs_emit(buf, "%lu\n", nr_huge_pages);
}

static ssize_t nr_hugepages_store_common(bool obey_mempolicy,
					 struct kobject *kobj, const char *buf,
					 size_t len)
{
	struct hstate *h;
	unsigned long count;
	int nid;
	int err;

	err = kstrtoul(buf, 10, &count);
	if (err)
		return err;

	h = kobj_to_hstate(kobj, &nid);
	return __nr_hugepages_store_common(obey_mempolicy, h, nid, count, len);
}

static ssize_t nr_hugepages_show(struct kobject *kobj,
				       struct kobj_attribute *attr, char *buf)
{
	return nr_hugepages_show_common(kobj, attr, buf);
}

static ssize_t nr_hugepages_store(struct kobject *kobj,
	       struct kobj_attribute *attr, const char *buf, size_t len)
{
	return nr_hugepages_store_common(false, kobj, buf, len);
}
HSTATE_ATTR(nr_hugepages);

#ifdef CONFIG_NUMA

/*
 * hstate attribute for optionally mempolicy-based constraint on persistent
 * huge page alloc/free.
 */
static ssize_t nr_hugepages_mempolicy_show(struct kobject *kobj,
					   struct kobj_attribute *attr,
					   char *buf)
{
	return nr_hugepages_show_common(kobj, attr, buf);
}

static ssize_t nr_hugepages_mempolicy_store(struct kobject *kobj,
	       struct kobj_attribute *attr, const char *buf, size_t len)
{
	return nr_hugepages_store_common(true, kobj, buf, len);
}
HSTATE_ATTR(nr_hugepages_mempolicy);
#endif


static ssize_t nr_overcommit_hugepages_show(struct kobject *kobj,
					struct kobj_attribute *attr, char *buf)
{
	struct hstate *h = kobj_to_hstate(kobj, NULL);
	return sysfs_emit(buf, "%lu\n", h->nr_overcommit_huge_pages);
}

static ssize_t nr_overcommit_hugepages_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	int err;
	unsigned long input;
	struct hstate *h = kobj_to_hstate(kobj, NULL);

	if (hstate_is_gigantic_no_runtime(h))
		return -EINVAL;

	err = kstrtoul(buf, 10, &input);
	if (err)
		return err;

	spin_lock_irq(&hugetlb_lock);
	h->nr_overcommit_huge_pages = input;
	spin_unlock_irq(&hugetlb_lock);

	return count;
}
HSTATE_ATTR(nr_overcommit_hugepages);

static ssize_t free_hugepages_show(struct kobject *kobj,
					struct kobj_attribute *attr, char *buf)
{
	struct hstate *h;
	unsigned long free_huge_pages;
	int nid;

	h = kobj_to_hstate(kobj, &nid);
	if (nid == NUMA_NO_NODE)
		free_huge_pages = h->free_huge_pages;
	else
		free_huge_pages = h->free_huge_pages_node[nid];

	return sysfs_emit(buf, "%lu\n", free_huge_pages);
}
HSTATE_ATTR_RO(free_hugepages);

static ssize_t resv_hugepages_show(struct kobject *kobj,
					struct kobj_attribute *attr, char *buf)
{
	struct hstate *h = kobj_to_hstate(kobj, NULL);
	return sysfs_emit(buf, "%lu\n", h->resv_huge_pages);
}
HSTATE_ATTR_RO(resv_hugepages);

static ssize_t surplus_hugepages_show(struct kobject *kobj,
					struct kobj_attribute *attr, char *buf)
{
	struct hstate *h;
	unsigned long surplus_huge_pages;
	int nid;

	h = kobj_to_hstate(kobj, &nid);
	if (nid == NUMA_NO_NODE)
		surplus_huge_pages = h->surplus_huge_pages;
	else
		surplus_huge_pages = h->surplus_huge_pages_node[nid];

	return sysfs_emit(buf, "%lu\n", surplus_huge_pages);
}
HSTATE_ATTR_RO(surplus_hugepages);

static ssize_t demote_store(struct kobject *kobj,
	       struct kobj_attribute *attr, const char *buf, size_t len)
{
	unsigned long nr_demote;
	unsigned long nr_available;
	nodemask_t nodes_allowed, *n_mask;
	struct hstate *h;
	int err;
	int nid;

	err = kstrtoul(buf, 10, &nr_demote);
	if (err)
		return err;
	h = kobj_to_hstate(kobj, &nid);

	if (nid != NUMA_NO_NODE) {
		init_nodemask_of_node(&nodes_allowed, nid);
		n_mask = &nodes_allowed;
	} else {
		n_mask = &node_states[N_MEMORY];
	}

	/* Synchronize with other sysfs operations modifying huge pages */
	mutex_lock(&h->resize_lock);
	spin_lock_irq(&hugetlb_lock);

	while (nr_demote) {
		long rc;

		/*
		 * Check for available pages to demote each time thorough the
		 * loop as demote_pool_huge_page will drop hugetlb_lock.
		 */
		if (nid != NUMA_NO_NODE)
			nr_available = h->free_huge_pages_node[nid];
		else
			nr_available = h->free_huge_pages;
		nr_available -= h->resv_huge_pages;
		if (!nr_available)
			break;

		rc = demote_pool_huge_page(h, n_mask, nr_demote);
		if (rc < 0) {
			err = rc;
			break;
		}

		nr_demote -= rc;
	}

	spin_unlock_irq(&hugetlb_lock);
	mutex_unlock(&h->resize_lock);

	if (err)
		return err;
	return len;
}
HSTATE_ATTR_WO(demote);

static ssize_t demote_size_show(struct kobject *kobj,
					struct kobj_attribute *attr, char *buf)
{
	struct hstate *h = kobj_to_hstate(kobj, NULL);
	unsigned long demote_size = (PAGE_SIZE << h->demote_order) / SZ_1K;

	return sysfs_emit(buf, "%lukB\n", demote_size);
}

static ssize_t demote_size_store(struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t count)
{
	struct hstate *h, *demote_hstate;
	unsigned long demote_size;
	unsigned int demote_order;

	demote_size = (unsigned long)memparse(buf, NULL);

	demote_hstate = size_to_hstate(demote_size);
	if (!demote_hstate)
		return -EINVAL;
	demote_order = demote_hstate->order;
	if (demote_order < HUGETLB_PAGE_ORDER)
		return -EINVAL;

	/* demote order must be smaller than hstate order */
	h = kobj_to_hstate(kobj, NULL);
	if (demote_order >= h->order)
		return -EINVAL;

	/* resize_lock synchronizes access to demote size and writes */
	mutex_lock(&h->resize_lock);
	h->demote_order = demote_order;
	mutex_unlock(&h->resize_lock);

	return count;
}
HSTATE_ATTR(demote_size);

static struct attribute *hstate_attrs[] = {
	&nr_hugepages_attr.attr,
	&nr_overcommit_hugepages_attr.attr,
	&free_hugepages_attr.attr,
	&resv_hugepages_attr.attr,
	&surplus_hugepages_attr.attr,
#ifdef CONFIG_NUMA
	&nr_hugepages_mempolicy_attr.attr,
#endif
	NULL,
};

static const struct attribute_group hstate_attr_group = {
	.attrs = hstate_attrs,
};

static struct attribute *hstate_demote_attrs[] = {
	&demote_size_attr.attr,
	&demote_attr.attr,
	NULL,
};

static const struct attribute_group hstate_demote_attr_group = {
	.attrs = hstate_demote_attrs,
};

static int hugetlb_sysfs_add_hstate(struct hstate *h, struct kobject *parent,
				    struct kobject **hstate_kobjs,
				    const struct attribute_group *hstate_attr_group)
{
	int retval;
	int hi = hstate_index(h);

	hstate_kobjs[hi] = kobject_create_and_add(h->name, parent);
	if (!hstate_kobjs[hi])
		return -ENOMEM;

	retval = sysfs_create_group(hstate_kobjs[hi], hstate_attr_group);
	if (retval) {
		kobject_put(hstate_kobjs[hi]);
		hstate_kobjs[hi] = NULL;
		return retval;
	}

	if (h->demote_order) {
		retval = sysfs_create_group(hstate_kobjs[hi],
					    &hstate_demote_attr_group);
		if (retval) {
			pr_warn("HugeTLB unable to create demote interfaces for %s\n", h->name);
			sysfs_remove_group(hstate_kobjs[hi], hstate_attr_group);
			kobject_put(hstate_kobjs[hi]);
			hstate_kobjs[hi] = NULL;
			return retval;
		}
	}

	return 0;
}

#ifdef CONFIG_NUMA
static bool hugetlb_sysfs_initialized __ro_after_init;

/*
 * node_hstate/s - associate per node hstate attributes, via their kobjects,
 * with node devices in node_devices[] using a parallel array.  The array
 * index of a node device or _hstate == node id.
 * This is here to avoid any static dependency of the node device driver, in
 * the base kernel, on the hugetlb module.
 */
struct node_hstate {
	struct kobject		*hugepages_kobj;
	struct kobject		*hstate_kobjs[HUGE_MAX_HSTATE];
};
static struct node_hstate node_hstates[MAX_NUMNODES];

/*
 * A subset of global hstate attributes for node devices
 */
static struct attribute *per_node_hstate_attrs[] = {
	&nr_hugepages_attr.attr,
	&free_hugepages_attr.attr,
	&surplus_hugepages_attr.attr,
	NULL,
};

static const struct attribute_group per_node_hstate_attr_group = {
	.attrs = per_node_hstate_attrs,
};

/*
 * kobj_to_node_hstate - lookup global hstate for node device hstate attr kobj.
 * Returns node id via non-NULL nidp.
 */
static struct hstate *kobj_to_node_hstate(struct kobject *kobj, int *nidp)
{
	int nid;

	for (nid = 0; nid < nr_node_ids; nid++) {
		struct node_hstate *nhs = &node_hstates[nid];
		int i;
		for (i = 0; i < HUGE_MAX_HSTATE; i++)
			if (nhs->hstate_kobjs[i] == kobj) {
				if (nidp)
					*nidp = nid;
				return &hstates[i];
			}
	}

	BUG();
	return NULL;
}

/*
 * Unregister hstate attributes from a single node device.
 * No-op if no hstate attributes attached.
 */
void hugetlb_unregister_node(struct node *node)
{
	struct hstate *h;
	struct node_hstate *nhs = &node_hstates[node->dev.id];

	if (!nhs->hugepages_kobj)
		return;		/* no hstate attributes */

	for_each_hstate(h) {
		int idx = hstate_index(h);
		struct kobject *hstate_kobj = nhs->hstate_kobjs[idx];

		if (!hstate_kobj)
			continue;
		if (h->demote_order)
			sysfs_remove_group(hstate_kobj, &hstate_demote_attr_group);
		sysfs_remove_group(hstate_kobj, &per_node_hstate_attr_group);
		kobject_put(hstate_kobj);
		nhs->hstate_kobjs[idx] = NULL;
	}

	kobject_put(nhs->hugepages_kobj);
	nhs->hugepages_kobj = NULL;
}


/*
 * Register hstate attributes for a single node device.
 * No-op if attributes already registered.
 */
void hugetlb_register_node(struct node *node)
{
	struct hstate *h;
	struct node_hstate *nhs = &node_hstates[node->dev.id];
	int err;

	if (!hugetlb_sysfs_initialized)
		return;

	if (nhs->hugepages_kobj)
		return;		/* already allocated */

	nhs->hugepages_kobj = kobject_create_and_add("hugepages",
							&node->dev.kobj);
	if (!nhs->hugepages_kobj)
		return;

	for_each_hstate(h) {
		err = hugetlb_sysfs_add_hstate(h, nhs->hugepages_kobj,
						nhs->hstate_kobjs,
						&per_node_hstate_attr_group);
		if (err) {
			pr_err("HugeTLB: Unable to add hstate %s for node %d\n",
				h->name, node->dev.id);
			hugetlb_unregister_node(node);
			break;
		}
	}
}

/*
 * hugetlb init time:  register hstate attributes for all registered node
 * devices of nodes that have memory.  All on-line nodes should have
 * registered their associated device by this time.
 */
static void __init hugetlb_register_all_nodes(void)
{
	int nid;

	for_each_online_node(nid)
		hugetlb_register_node(node_devices[nid]);
}
#else	/* !CONFIG_NUMA */

static struct hstate *kobj_to_node_hstate(struct kobject *kobj, int *nidp)
{
	BUG();
	if (nidp)
		*nidp = -1;
	return NULL;
}

static void hugetlb_register_all_nodes(void) { }

#endif

void __init hugetlb_sysfs_init(void)
{
	struct hstate *h;
	int err;

	hugepages_kobj = kobject_create_and_add("hugepages", mm_kobj);
	if (!hugepages_kobj)
		return;

	for_each_hstate(h) {
		err = hugetlb_sysfs_add_hstate(h, hugepages_kobj,
					 hstate_kobjs, &hstate_attr_group);
		if (err)
			pr_err("HugeTLB: Unable to add hstate %s\n", h->name);
	}

#ifdef CONFIG_NUMA
	hugetlb_sysfs_initialized = true;
#endif
	hugetlb_register_all_nodes();
}
