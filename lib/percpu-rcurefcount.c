// SPDX-License-Identifier: GPL-2.0-only

#include <linux/moduleparam.h>
#include <linux/percpu-rcurefcount.h>

static LLIST_HEAD(pcpu_rcuref_head);

/*
 * The refcount management of percpu rcuref is same as
 * normal percpu refcount, with the only difference that,
 * instead of a explicit shutdown percpu_ref_kill() operation
 * by the user, the initial ref is managed by a kworker.
 *
 * The ref can be initialized to start either in managed or
 * unmanaged mode. In managed mode, the ref is a set of percpu
 * counters. There is an extra reference acquired for the llist
 * node and provides the notion of initial ref in percpu refcount.
 *
 * During normal operation, users ref get() and put() operations
 * increment/decrement the percpu counters. There is no check
 * for drop-to-zero while in percpu mode.
 *
 * Periodically, the manager kworker thread scans all percpu
 * rcurefs. It switches ref to centralized atomic counter mode
 * and checks whether the object has no references left. The ref is
 * dropped if there are no references. Otherwise, the ref is switched
 * back to percpu mode again. During this ref scan, there is a
 * window where ref operates in atomic mode. This window spans
 * one RCU grace period.
 *
 * There is a provision to start a percpu rcuref in unmanaged mode.
 * This is provided for cases, where there is a need to avoid
 * dependency on kworker and RCU grace period. In addition,
 * unmanaged mode can be used for a ref, for which the release
 * function initially does not wait for RCU grace period, for
 * example when the enclosing object initialization fails, and
 * there is a rollback operation in error paths. Later, when
 * object initialization is complete, ref can be switched to
 * percpu managed mode.
 */
/**
 * percpu_rcuref_init - initialize a percpu rcuref count
 * @rcuref: percpu_rcuref to initialize
 * @release: function which will be called when refcount hits 0
 * @gfp: allocation mask to use
 *
 * Initializes @rcuref.  @rcuref starts out in percpu mode with a refcount of 2.
 * The initial ref is managed by the pcpu rcuref release worker kthread.
 * The second reference is for the user.
 *
 * Note that @release must not sleep - it can block release of other
 * pcpu rcurefs.
 */
int percpu_rcuref_init(struct percpu_rcuref *rcuref, percpu_ref_func_t *release, gfp_t gfp)
{
	int ret;

	ret = percpu_ref_init(&rcuref->pcpu_ref, release,
			      PERCPU_REF_ALLOW_REINIT, gfp);
	if (ret)
		return ret;
	percpu_ref_get(&rcuref->pcpu_ref);
	llist_add(&rcuref->node, &pcpu_rcuref_head);
	return 0;
}
EXPORT_SYMBOL_GPL(percpu_rcuref_init);

/**
 * percpu_rcuref_init_unmanaged - initialize a percpu rcuref count in
 *                                unmanaged (atomic) mode.
 * @rcuref: percpu_rcuref to initialize
 * @release: function which will be called when refcount hits 0
 * @gfp: allocation mask to use
 *
 * Initializes @rcuref.  @rcuref starts out in unmanaged/atomic mode
 * with a refcount of 1.
 * The initial ref is passed to the user and ref management is
 * auto, the last put operation releases the ref.
 * The ref may be initialized in this mode, to avoid dependency
 * on workqueue and RCU, for early boot code; and for cases where
 * a ref starts as non-RCU release and switches to RCU grace period
 * based release of the reference. The percpu_rcuref_manage() call
 * can be used to switch this ref to managed mode, while the ref
 * is active. This operation is non-reversible, and the ref remains
 * in managed mode, for its lifeline, until it is released by the
 * pcpu release kworker.
 *
 * Note that @release must not sleep - if the ref is switched to
 * managed mode, it can block release of other pcpu rcurefs.
 */
int percpu_rcuref_init_unmanaged(struct percpu_rcuref *rcuref,
				 percpu_ref_func_t *release, gfp_t gfp)
{
	int ret;

	ret = percpu_ref_init(&rcuref->pcpu_ref, release, PERCPU_REF_INIT_ATOMIC, gfp);
	if (!ret)
		init_llist_node(&rcuref->node);
	return ret;
}
EXPORT_SYMBOL_GPL(percpu_rcuref_init_unmanaged);

/**
 * percpu_rcuref_manage - Switch an unmanaged ref to percpu mode.
 *
 * @rcuref: percpu_rcuref to initialize
 * @release: function which will be called when refcount hits 0
 * @gfp: allocation mask to use
 *
 */
int percpu_rcuref_manage(struct percpu_rcuref *rcuref)
{
	if (WARN_ONCE(!percpu_rcuref_tryget(rcuref), "Percpu rcuref is not active\n"))
		return -1;
	if (WARN_ONCE(llist_on_list(&rcuref->node), "Percpu rcuref already managed\n")) {
		percpu_rcuref_put(rcuref);
		return -2;
	}
	percpu_ref_switch_to_percpu(&rcuref->pcpu_ref);
	/* Ensure ordering of percpu mode switch and node scan */
	smp_mb();
	llist_add(&rcuref->node, &pcpu_rcuref_head);
	return 0;
}
EXPORT_SYMBOL_GPL(percpu_rcuref_manage);

/**
 * percpu_rcuref_is_zero - test whether a percpu rcuref count reached zero
 * @rcuref: percpu_rcuref to test
 *
 * Returns %true if @ref reached zero.
 */
bool percpu_rcuref_is_zero(struct percpu_rcuref *rcuref)
{
	return percpu_ref_is_zero(&rcuref->pcpu_ref);
}
EXPORT_SYMBOL_GPL(percpu_rcuref_is_zero);

/**
 * percpu_rcuref_exit - undo percpu_rcuref_init()
 * @rcuref: percpu_rcuref to exit
 *
 * This function exits @rcuref.  The caller is responsible for ensuring that
 * @rcuref is no longer in active use.  The usual places to invoke this
 * function from are the @rcuref->release() callback or in init failure path
 * where percpu_rcuref_init() succeeded but other parts of the initialization
 * of the embedding object failed.
 */
void percpu_rcuref_exit(struct percpu_rcuref *rcuref)
{
	percpu_ref_exit(&rcuref->pcpu_ref);
	init_llist_node(&rcuref->node);
}

#define DEFAULT_PCPU_RCUREF_SCAN_INTERVAL_MS    5000
/* Interval duration between two ref scans. */
static ulong ref_scan_interval = DEFAULT_PCPU_RCUREF_SCAN_INTERVAL_MS;
module_param(ref_scan_interval, ulong, 0444);

#define DEFAULT_PCPU_RCUREF_MAX_SCAN_COUNT      100
/* Number of pcpu refs scanned in one iteration of worker execution. */
static int max_ref_scan_count = DEFAULT_PCPU_RCUREF_MAX_SCAN_COUNT;
module_param(max_ref_scan_count, int, 0444);

static void percpu_rcuref_release_work_fn(struct work_struct *work);

/*
 * Sentinel llist nodes, for lockless list traveral and deletions by
 * the pcpu rcuref release worker, while nodes are added from normal
 * from percpu_rcuref_init() and percpu_rcuref_manage().
 *
 * Sentinel node marks the head of list traversal for the current
 * iteration of kworker execution.
 */
struct pcpu_rcuref_sen_node {
	bool inuse;
	struct llist_node node;
};

/*
 * We need two sentinel nodes for lockless list manipulations from release
 * worker - first node will be used in current reclaim iteration.The second
 * node will be used in next iteration. Next iteration marks the first node
 * as free, for use in following iteration.
 */
#define PCPU_RCUREF_SEN_NODES_COUNT     2

/* Track last processed percpu rcuref node */
static struct llist_node *last_pcu_rcuref_node;

static struct pcpu_rcuref_sen_node
	pcpu_rcuref_sen_nodes[PCPU_RCUREF_SEN_NODES_COUNT];

static DECLARE_DELAYED_WORK(percpu_rcuref_release_work,
			    percpu_rcuref_release_work_fn);

static bool percpu_rcuref_is_sen_node(struct llist_node *node)
{
	return &pcpu_rcuref_sen_nodes[0].node <= node &&
		node <= &pcpu_rcuref_sen_nodes[PCPU_RCUREF_SEN_NODES_COUNT - 1].node;
}

static struct llist_node *percpu_rcuref_get_sen_node(void)
{
	int i;
	struct pcpu_rcuref_sen_node *sn;

	for (i = 0; i < PCPU_RCUREF_SEN_NODES_COUNT; i++) {
		sn = &pcpu_rcuref_sen_nodes[i];
		if (!sn->inuse) {
			sn->inuse = true;
			return &sn->node;
		}
	}

	return NULL;
}

static void percpu_rcuref_put_sen_node(struct llist_node *node)
{
	struct pcpu_rcuref_sen_node *sn = container_of(node, struct pcpu_rcuref_sen_node, node);

	sn->inuse = false;
}

static void percpu_rcuref_put_all_sen_nodes_except(struct llist_node *node)
{
	int i;

	for (i = 0; i < PCPU_RCUREF_SEN_NODES_COUNT; i++) {
		if (&pcpu_rcuref_sen_nodes[i].node == node)
			continue;
		pcpu_rcuref_sen_nodes[i].inuse = false;
		init_llist_node(&pcpu_rcuref_sen_nodes[i].node);
	}
}

static struct workqueue_struct *percpu_rcuref_wq;

static void percpu_rcuref_release_work_fn(struct work_struct *work)
{
	struct llist_node *pos, *first, *head, *prev, *next;
	struct percpu_rcuref *rcuref;
	struct llist_node *sen_node;
	int count = 0;
	bool held;

	first = READ_ONCE(pcpu_rcuref_head.first);
	if (!first)
		goto queue_release_work;

	if (last_pcu_rcuref_node == NULL || last_pcu_rcuref_node->next == NULL) {
retry_sentinel_get:
		sen_node = percpu_rcuref_get_sen_node();
		/*
		 * All sentinel nodes are in use? This should not happen, as we
		 * require only one sentinel for the start of list traversal and
		 * other sentinel node is freed during the traversal.
		 */
		if (WARN_ONCE(!sen_node, "Percpu RCU ref sentinel nodes exhausted\n")) {
			/* Use first node as the sentinel node */
			head = first->next;
			if (!head) {
				struct llist_node *ign_node = NULL;
				/*
				 * We exhausted sentinel nodes. However, there aren't
				 * enough nodes in the llist. So, we have leaked
				 * sentinel nodes. Reclaim sentinels and retry.
				 */
				if (percpu_rcuref_is_sen_node(first))
					ign_node = first;
				percpu_rcuref_put_all_sen_nodes_except(ign_node);
				goto retry_sentinel_get;
			}
			prev = first;
		} else {
			llist_add(sen_node, &pcpu_rcuref_head);
			prev = sen_node;
			head = prev->next;
		}
	} else {
		prev = last_pcu_rcuref_node;
		head = prev->next;
	}

	last_pcu_rcuref_node = NULL;
	llist_for_each_safe(pos, next, head) {
		/* Free sentinel node which is present in the list */
		if (percpu_rcuref_is_sen_node(pos)) {
			prev->next = pos->next;
			percpu_rcuref_put_sen_node(pos);
			continue;
		}

		rcuref = container_of(pos, struct percpu_rcuref, node);
		percpu_ref_switch_to_atomic_sync(&rcuref->pcpu_ref);
		/*
		 * Drop the ref while in RCU read critical section, to
		 * prevent obj free while we manipulating node.
		 */
		rcu_read_lock();
		percpu_ref_put(&rcuref->pcpu_ref);
		held = percpu_ref_tryget(&rcuref->pcpu_ref);
		if (!held) {
			prev->next = pos->next;
			init_llist_node(pos);
		}
		rcu_read_unlock();
		if (!held)
			continue;
		percpu_ref_switch_to_percpu(&rcuref->pcpu_ref);
		count++;
		if (count == max_ref_scan_count) {
			last_pcu_rcuref_node = pos;
			break;
		}
		prev = pos;
	}

queue_release_work:
	queue_delayed_work(percpu_rcuref_wq, &percpu_rcuref_release_work,
			   ref_scan_interval);
}

static __init int percpu_rcuref_setup(void)
{
	percpu_rcuref_wq = alloc_workqueue("percpu_rcuref",
				WQ_UNBOUND | WQ_MEM_RECLAIM | WQ_FREEZABLE, 0);
	if (!percpu_rcuref_wq)
		return -ENOMEM;

	queue_delayed_work(percpu_rcuref_wq, &percpu_rcuref_release_work,
			   ref_scan_interval);
	return 0;
}
early_initcall(percpu_rcuref_setup);
