// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2022-2024 Intel Corporation. */

#include<linux/slab.h>
#include "epc_cgroup.h"

/*
 * The minimal free pages, or the minimal margin between limit and usage
 * maintained by per-cgroup reclaimer.
 *
 * Set this to the low threshold used by the global reclaimer, ksgxd.
 */
#define SGX_CG_MIN_FREE_PAGE	(SGX_NR_LOW_PAGES)

/*
 * If the cgroup limit is close to SGX_CG_MIN_FREE_PAGE, maintaining the minimal
 * free pages would barely leave any page for use, causing excessive reclamation
 * and thrashing.
 *
 * Define the following limit, below which cgroup does not maintain the minimal
 * free page threshold. Set this to quadruple of the minimal so at least 75%
 * pages used without being reclaimed.
 */
#define SGX_CG_LOW_LIMIT	(SGX_CG_MIN_FREE_PAGE * 4)

/* The root SGX EPC cgroup */
static struct sgx_cgroup sgx_cg_root;

/*
 * The work queue that reclaims EPC pages in the background for cgroups.
 *
 * A cgroup schedules a work item into this queue to reclaim pages within the
 * same cgroup when its usage limit is reached and synchronous reclamation is not
 * an option, i.e., in a page fault handler.
 */
static struct workqueue_struct *sgx_cg_wq;

/*
 * Return the next descendant in a preorder walk, given a root, @root and a
 * cgroup, @cg, to start the walk from. Return @root if no descendant left for
 * this walk. Otherwise, return next descendant with its refcnt incremented.
 */
static struct sgx_cgroup *sgx_cgroup_next_descendant_pre(struct sgx_cgroup *root,
							 struct sgx_cgroup *cg)
{
	struct cgroup_subsys_state *next = &cg->cg->css;

	rcu_read_lock();
	for (;;) {
		next = css_next_descendant_pre(next, &root->cg->css);
		if (!next) {
			next = &root->cg->css;
			break;
		}

		if (css_tryget(next))
			break;
	}
	rcu_read_unlock();

	return sgx_cgroup_from_misc_cg(css_misc(next));
}

/*
 * For a given root, @root, if a given cgroup, @cg, is the next cgroup to
 * reclaim pages from, i.e., referenced by @root->next_cg, then advance
 * @root->next_cg to the next valid cgroup in a preorder walk or the root if no
 * more descendants left to walk.
 *
 * Called from sgx_cgroup_free() when @cg is to be freed so it can no longer be
 * used as 'next_cg'.
 */
static inline void sgx_cgroup_next_skip(struct sgx_cgroup *root, struct sgx_cgroup *cg)
{
	struct sgx_cgroup *p;

	spin_lock(&root->next_cg_lock);
	p = root->next_cg;
	spin_unlock(&root->next_cg_lock);

	/* Already moved by other threads, no need to update */
	if (cg != p)
		return;

	p = sgx_cgroup_next_descendant_pre(root, cg);

	spin_lock(&root->next_cg_lock);
	if (root->next_cg == cg)
		root->next_cg = p;
	spin_unlock(&root->next_cg_lock);

	/* Decrement refcnt so cgroup pointed to by p can be released */
	if (p != cg && p != root)
		sgx_put_cg(p);
}

/*
 * Return the cgroup currently referenced by @root->next_cg and advance
 * @root->next_cg to the next descendant or @root.  The returned cgroup has its
 * refcnt incremented if it is not @root and caller must release the refcnt.
 */
static inline struct sgx_cgroup *sgx_cgroup_next_get(struct sgx_cgroup *root)
{
	struct sgx_cgroup *p;

	/*
	 * Acquire a reference for the to-be-returned cgroup and advance
	 * next_cg with the lock so the same cg not returned to two threads.
	 */
	spin_lock(&root->next_cg_lock);

	p = root->next_cg;

	/* Advance the to-be-returned to next descendant if current one is dying */
	if (p != root && !css_tryget(&p->cg->css))
		p = sgx_cgroup_next_descendant_pre(root, p);

	/* Advance next_cg */
	root->next_cg = sgx_cgroup_next_descendant_pre(root, p);

	/* Decrement ref here so it can be released by cgroup subsystem */
	if (root->next_cg != root)
		sgx_put_cg(root->next_cg);

	spin_unlock(&root->next_cg_lock);

	/* p is root or refcnt incremented */
	return p;
}

static inline u64 sgx_cgroup_page_counter_read(struct sgx_cgroup *sgx_cg)
{
	return atomic64_read(&sgx_cg->cg->res[MISC_CG_RES_SGX_EPC].usage) / PAGE_SIZE;
}

static inline u64 sgx_cgroup_max_pages(struct sgx_cgroup *sgx_cg)
{
	return READ_ONCE(sgx_cg->cg->res[MISC_CG_RES_SGX_EPC].max) / PAGE_SIZE;
}

/*
 * Get the lower bound of limits of a cgroup and its ancestors. Used in
 * sgx_cgroup_should_reclaim() to determine if EPC usage of a cgroup is
 * close to its limit or its ancestors' hence reclamation is needed.
 */
static inline u64 sgx_cgroup_max_pages_to_root(struct sgx_cgroup *sgx_cg)
{
	struct misc_cg *i = sgx_cg->cg;
	u64 m = U64_MAX;

	while (i) {
		m = min(m, READ_ONCE(i->res[MISC_CG_RES_SGX_EPC].max));
		i = misc_cg_parent(i);
	}

	return m / PAGE_SIZE;
}

/**
 * sgx_cgroup_lru_empty() - check if a cgroup tree has no pages on its LRUs
 * @root:	Root of the tree to check
 *
 * Return: %true if all cgroups under the specified root have empty LRU lists.
 */
bool sgx_cgroup_lru_empty(struct misc_cg *root)
{
	struct cgroup_subsys_state *css_root;
	struct cgroup_subsys_state *pos;
	struct sgx_cgroup *sgx_cg;
	bool ret = true;

	/*
	 * Caller must ensure css_root ref acquired
	 */
	css_root = &root->css;

	rcu_read_lock();
	css_for_each_descendant_pre(pos, css_root) {
		if (!css_tryget(pos))
			continue;

		rcu_read_unlock();

		sgx_cg = sgx_cgroup_from_misc_cg(css_misc(pos));

		spin_lock(&sgx_cg->lru.lock);
		ret = list_empty(&sgx_cg->lru.reclaimable);
		spin_unlock(&sgx_cg->lru.lock);

		rcu_read_lock();
		css_put(pos);
		if (!ret)
			break;
	}

	rcu_read_unlock();

	return ret;
}

/*
 * Scan at least @nr_to_scan pages and attempt to reclaim them from the subtree of @root.
 * Charge backing store allocation to the given mm, @charge_mm.
 */
static inline void sgx_cgroup_reclaim_pages(struct sgx_cgroup *root,
					    struct mm_struct *charge_mm,
					    unsigned int nr_to_scan)
{
	struct sgx_cgroup *next_cg = NULL;
	unsigned int cnt = 0;

	while (!sgx_cgroup_lru_empty(root->cg) && cnt < nr_to_scan) {
		next_cg = sgx_cgroup_next_get(root);
		cnt += sgx_reclaim_pages(&next_cg->lru, charge_mm);
		if (next_cg != root)
			sgx_put_cg(next_cg);
	}
}

/* Check whether EPC reclaim should be performed for a given EPC cgroup.*/
static bool sgx_cgroup_should_reclaim(struct sgx_cgroup *sgx_cg)
{
	u64 cur, max;

	if (sgx_cgroup_lru_empty(sgx_cg->cg))
		return false;

	max = sgx_cgroup_max_pages_to_root(sgx_cg);

	/*
	 * Unless the limit is very low, maintain a minimal "credit" available
	 * for charge to avoid per-cgroup reclamation and to serve new
	 * allocation requests more quickly.
	 */
	if (max > SGX_CG_LOW_LIMIT)
		max -= SGX_CG_MIN_FREE_PAGE;

	cur = sgx_cgroup_page_counter_read(sgx_cg);

	return (cur >= max);
}

/**
 * sgx_cgroup_reclaim_direct() - Preemptive reclamation.
 *
 * Scan and attempt to reclaim %SGX_NR_TO_SCAN as best effort to make later
 * EPC allocation quicker.
 */
void sgx_cgroup_reclaim_direct(void)
{
	struct sgx_cgroup *sgx_cg = sgx_get_current_cg();

	if (sgx_cgroup_should_reclaim(sgx_cg))
		sgx_cgroup_reclaim_pages(sgx_cg, current->mm, SGX_NR_TO_SCAN);
	sgx_put_cg(sgx_cg);
}

/**
 * sgx_cgroup_reclaim_pages_global() - Perform one round of global reclamation.
 *
 * @charge_mm:	The mm to be charged for the backing store of reclaimed pages.
 *
 * Try to scan and attempt reclamation from root cgroup for %SGX_NR_TO_SCAN pages.
 */
void sgx_cgroup_reclaim_pages_global(struct mm_struct *charge_mm)
{
	sgx_cgroup_reclaim_pages(&sgx_cg_root, charge_mm, SGX_NR_TO_SCAN);
}

/*
 * Asynchronous work flow to reclaim pages from the cgroup when the cgroup is
 * at/near its maximum capacity.
 */
static void sgx_cgroup_reclaim_work_func(struct work_struct *work)
{
	struct sgx_cgroup *root = container_of(work, struct sgx_cgroup, reclaim_work);

	while (sgx_cgroup_should_reclaim(root)) {
		/* Indirect reclaim, no mm to charge, so NULL: */
		sgx_cgroup_reclaim_pages(root, NULL, SGX_NR_TO_SCAN);
		cond_resched();
	}
}

static int __sgx_cgroup_try_charge(struct sgx_cgroup *epc_cg)
{
	if (!misc_cg_try_charge(MISC_CG_RES_SGX_EPC, epc_cg->cg, PAGE_SIZE))
		return 0;

	/* No reclaimable pages left in the cgroup */
	if (sgx_cgroup_lru_empty(epc_cg->cg))
		return -ENOMEM;

	if (signal_pending(current))
		return -ERESTARTSYS;

	return -EBUSY;
}

/**
 * sgx_cgroup_try_charge() - try to charge cgroup for a single EPC page
 * @sgx_cg:	The EPC cgroup to be charged for the page.
 * @reclaim:	Whether or not synchronous EPC reclaim is allowed.
 * Return:
 * * %0 - If successfully charged.
 * * -errno - for failures.
 */
int sgx_cgroup_try_charge(struct sgx_cgroup *sgx_cg, enum sgx_reclaim reclaim)
{
	int ret;

	for (;;) {
		ret = __sgx_cgroup_try_charge(sgx_cg);

		if (ret != -EBUSY)
			goto out;

		if (reclaim == SGX_NO_RECLAIM) {
			queue_work(sgx_cg_wq, &sgx_cg->reclaim_work);
			ret = -EBUSY;
			goto out;
		}

		sgx_cgroup_reclaim_pages(sgx_cg, current->mm, 1);

		cond_resched();
	}

	if (sgx_cgroup_should_reclaim(sgx_cg))
		queue_work(sgx_cg_wq, &sgx_cg->reclaim_work);

out:
	return ret;
}

/**
 * sgx_cgroup_uncharge() - uncharge a cgroup for an EPC page
 * @sgx_cg:	The charged sgx cgroup.
 */
void sgx_cgroup_uncharge(struct sgx_cgroup *sgx_cg)
{
	misc_cg_uncharge(MISC_CG_RES_SGX_EPC, sgx_cg->cg, PAGE_SIZE);
}

static void sgx_cgroup_free(struct misc_cg *cg)
{
	struct sgx_cgroup *sgx_cg;
	struct misc_cg *p;

	sgx_cg = sgx_cgroup_from_misc_cg(cg);
	if (!sgx_cg)
		return;

	cancel_work_sync(&sgx_cg->reclaim_work);
	/*
	 * Notify ancestors to not reclaim from this dying cgroup.
	 * Not start from this cgroup itself because at this point no reference
	 * of this cgroup being hold, i.e., all pages in this cgroup are freed
	 * and LRU is empty, so no reclamation possible.
	 */
	p = misc_cg_parent(cg);
	while (p) {
		sgx_cgroup_next_skip(sgx_cgroup_from_misc_cg(p), sgx_cg);
		p = misc_cg_parent(p);
	}

	kfree(sgx_cg);
}

static void sgx_cgroup_misc_init(struct misc_cg *cg, struct sgx_cgroup *sgx_cg)
{
	sgx_lru_init(&sgx_cg->lru);
	INIT_WORK(&sgx_cg->reclaim_work, sgx_cgroup_reclaim_work_func);
	cg->res[MISC_CG_RES_SGX_EPC].priv = sgx_cg;
	sgx_cg->cg = cg;
	sgx_cg->next_cg = sgx_cg;
	spin_lock_init(&sgx_cg->next_cg_lock);
}

static int sgx_cgroup_alloc(struct misc_cg *cg)
{
	struct sgx_cgroup *sgx_cg;

	sgx_cg = kzalloc(sizeof(*sgx_cg), GFP_KERNEL);
	if (!sgx_cg)
		return -ENOMEM;

	sgx_cgroup_misc_init(cg, sgx_cg);

	return 0;
}

const struct misc_res_ops sgx_cgroup_ops = {
	.alloc = sgx_cgroup_alloc,
	.free = sgx_cgroup_free,
};

/*
 * Initialize the workqueue for cgroups.
 */
int __init sgx_cgroup_wq_init(void)
{
	sgx_cg_wq = alloc_workqueue("sgx_cg_wq", WQ_UNBOUND | WQ_FREEZABLE,
				    WQ_UNBOUND_MAX_ACTIVE);
	if (!sgx_cg_wq) {
		pr_err("alloc_workqueue() failed for SGX cgroup.\n");
		return -ENOMEM;
	}

	return 0;
}

/*
 * Only called during init to unwind what's done in sgx_cgroup_wq_init()
 */
void __init sgx_cgroup_wq_deinit(void)
{
	destroy_workqueue(sgx_cg_wq);
}

/*
 * Register capacity and ops for SGX cgroup and init the root cgroup.
 * Only called at the end of sgx_init() when SGX is ready to handle the ops
 * callbacks. No failures allowed in this function.
 */
void __init sgx_cgroup_init(void)
{
	unsigned int nid = first_node(sgx_numa_mask);
	unsigned int first = nid;
	u64 capacity = 0;

	sgx_cgroup_misc_init(misc_cg_root(), &sgx_cg_root);
	misc_cg_set_ops(MISC_CG_RES_SGX_EPC, &sgx_cgroup_ops);

	/* sgx_numa_mask is not empty when this is called */
	do {
		capacity += sgx_numa_nodes[nid].size;
		nid = next_node_in(nid, sgx_numa_mask);
	} while (nid != first);
	misc_cg_set_capacity(MISC_CG_RES_SGX_EPC, capacity);
}
