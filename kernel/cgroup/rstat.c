// SPDX-License-Identifier: GPL-2.0-only
#include "cgroup-internal.h"

#include <linux/sched/cputime.h>

#include <linux/bpf.h>
#include <linux/btf.h>
#include <linux/btf_ids.h>

#include <trace/events/cgroup.h>

static DEFINE_SPINLOCK(rstat_base_lock);
static DEFINE_PER_CPU(struct llist_head, rstat_backlog_list);

static void cgroup_base_stat_flush(struct cgroup *cgrp, int cpu);

/*
 * Determines whether a given css can participate in rstat.
 * css's that are cgroup::self use rstat for base stats.
 * Other css's associated with a subsystem use rstat only when
 * they define the ss->css_rstat_flush callback.
 */
static inline bool css_uses_rstat(struct cgroup_subsys_state *css)
{
	return css_is_self(css) || css->ss->css_rstat_flush != NULL;
}

static struct css_rstat_cpu *css_rstat_cpu(
		struct cgroup_subsys_state *css, int cpu)
{
	return per_cpu_ptr(css->rstat_cpu, cpu);
}

static struct cgroup_rstat_base_cpu *cgroup_rstat_base_cpu(
		struct cgroup *cgrp, int cpu)
{
	return per_cpu_ptr(cgrp->rstat_base_cpu, cpu);
}

static spinlock_t *ss_rstat_lock(struct cgroup_subsys *ss)
{
	if (ss)
		return &ss->rstat_ss_lock;

	return &rstat_base_lock;
}

static inline struct llist_head *ss_lhead_cpu(struct cgroup_subsys *ss, int cpu)
{
	if (ss)
		return per_cpu_ptr(ss->lhead, cpu);
	return per_cpu_ptr(&rstat_backlog_list, cpu);
}

/**
 * css_rstat_updated - keep track of updated rstat_cpu
 * @css: target cgroup subsystem state
 * @cpu: cpu on which rstat_cpu was updated
 *
 * Atomically inserts the css in the ss's llist for the given cpu. This is
 * reentrant safe i.e. safe against softirq, hardirq and nmi. The ss's llist
 * will be processed at the flush time to create the update tree.
 */
__bpf_kfunc void css_rstat_updated(struct cgroup_subsys_state *css, int cpu)
{
	struct llist_head *lhead;
	struct css_rstat_cpu *rstatc;
	struct css_rstat_cpu __percpu *rstatc_pcpu;
	struct llist_node *self;

	/*
	 * Since bpf programs can call this function, prevent access to
	 * uninitialized rstat pointers.
	 */
	if (!css_uses_rstat(css))
		return;

	lockdep_assert_preemption_disabled();

	/*
	 * For archs withnot nmi safe cmpxchg or percpu ops support, ignore
	 * the requests from nmi context.
	 */
	if ((!IS_ENABLED(CONFIG_ARCH_HAVE_NMI_SAFE_CMPXCHG) ||
	     !IS_ENABLED(CONFIG_ARCH_HAS_NMI_SAFE_THIS_CPU_OPS)) && in_nmi())
		return;

	rstatc = css_rstat_cpu(css, cpu);
	/* If already on list return. */
	if (llist_on_list(&rstatc->lnode))
		return;

	/*
	 * This function can be renentered by irqs and nmis for the same cgroup
	 * and may try to insert the same per-cpu lnode into the llist. Note
	 * that llist_add() does not protect against such scenarios.
	 *
	 * To protect against such stacked contexts of irqs/nmis, we use the
	 * fact that lnode points to itself when not on a list and then use
	 * this_cpu_cmpxchg() to atomically set to NULL to select the winner
	 * which will call llist_add(). The losers can assume the insertion is
	 * successful and the winner will eventually add the per-cpu lnode to
	 * the llist.
	 */
	self = &rstatc->lnode;
	rstatc_pcpu = css->rstat_cpu;
	if (this_cpu_cmpxchg(rstatc_pcpu->lnode.next, self, NULL) != self)
		return;

	lhead = ss_lhead_cpu(css->ss, cpu);
	llist_add(&rstatc->lnode, lhead);
}

static void __css_process_update_tree(struct cgroup_subsys_state *css, int cpu)
{
	/* put @css and all ancestors on the corresponding updated lists */
	while (true) {
		struct css_rstat_cpu *rstatc = css_rstat_cpu(css, cpu);
		struct cgroup_subsys_state *parent = css->parent;
		struct css_rstat_cpu *prstatc;

		/*
		 * Both additions and removals are bottom-up.  If a cgroup
		 * is already in the tree, all ancestors are.
		 */
		if (rstatc->updated_next)
			break;

		/* Root has no parent to link it to, but mark it busy */
		if (!parent) {
			rstatc->updated_next = css;
			break;
		}

		prstatc = css_rstat_cpu(parent, cpu);
		rstatc->updated_next = prstatc->updated_children;
		prstatc->updated_children = css;

		css = parent;
	}
}

static void css_process_update_tree(struct cgroup_subsys *ss, int cpu)
{
	struct llist_head *lhead = ss_lhead_cpu(ss, cpu);
	struct llist_node *lnode;

	while ((lnode = llist_del_first_init(lhead))) {
		struct css_rstat_cpu *rstatc;

		rstatc = container_of(lnode, struct css_rstat_cpu, lnode);
		__css_process_update_tree(rstatc->owner, cpu);
	}
}

/**
 * css_rstat_push_children - push children css's into the given list
 * @head: current head of the list (= subtree root)
 * @child: first child of the root
 * @cpu: target cpu
 * Return: A new singly linked list of css's to be flushed
 *
 * Iteratively traverse down the css_rstat_cpu updated tree level by
 * level and push all the parents first before their next level children
 * into a singly linked list via the rstat_flush_next pointer built from the
 * tail backward like "pushing" css's into a stack. The root is pushed by
 * the caller.
 */
static struct cgroup_subsys_state *css_rstat_push_children(
		struct cgroup_subsys_state *head,
		struct cgroup_subsys_state *child, int cpu)
{
	struct cgroup_subsys_state *cnext = child;	/* Next head of child css level */
	struct cgroup_subsys_state *ghead = NULL;	/* Head of grandchild css level */
	struct cgroup_subsys_state *parent, *grandchild;
	struct css_rstat_cpu *crstatc;

	css_rstat_cpu(child, cpu)->rstat_flush_next = NULL;

	/*
	 * Notation: -> updated_next pointer
	 *	     => rstat_flush_next pointer
	 *
	 * Assuming the following sample updated_children lists:
	 *  P: C1 -> C2 -> P
	 *  C1: G11 -> G12 -> C1
	 *  C2: G21 -> G22 -> C2
	 *
	 * After 1st iteration:
	 *  head => C2 => C1 => NULL
	 *  ghead => G21 => G11 => NULL
	 *
	 * After 2nd iteration:
	 *  head => G12 => G11 => G22 => G21 => C2 => C1 => NULL
	 */
next_level:
	while (cnext) {
		child = cnext;
		cnext = css_rstat_cpu(child, cpu)->rstat_flush_next;
		parent = child->parent;

		/* updated_next is parent cgroup terminated if !NULL */
		while (child != parent) {
			css_rstat_cpu(child, cpu)->rstat_flush_next = head;
			head = child;
			crstatc = css_rstat_cpu(child, cpu);
			grandchild = crstatc->updated_children;
			if (grandchild != child) {
				/* Push the grand child to the next level */
				crstatc->updated_children = child;
				css_rstat_cpu(grandchild, cpu)->rstat_flush_next = ghead;
				ghead = grandchild;
			}
			child = crstatc->updated_next;
			crstatc->updated_next = NULL;
		}
	}

	if (ghead) {
		cnext = ghead;
		ghead = NULL;
		goto next_level;
	}
	return head;
}

/**
 * css_rstat_updated_list - build a list of updated css's to be flushed
 * @root: root of the css subtree to traverse
 * @cpu: target cpu
 * Return: A singly linked list of css's to be flushed
 *
 * Walks the updated rstat_cpu tree on @cpu from @root.  During traversal,
 * each returned css is unlinked from the updated tree.
 *
 * The only ordering guarantee is that, for a parent and a child pair
 * covered by a given traversal, the child is before its parent in
 * the list.
 *
 * Note that updated_children is self terminated and points to a list of
 * child css's if not empty. Whereas updated_next is like a sibling link
 * within the children list and terminated by the parent css. An exception
 * here is the css root whose updated_next can be self terminated.
 */
static struct cgroup_subsys_state *css_rstat_updated_list(
		struct cgroup_subsys_state *root, int cpu)
{
	struct css_rstat_cpu *rstatc = css_rstat_cpu(root, cpu);
	struct cgroup_subsys_state *head = NULL, *parent, *child;

	css_process_update_tree(root->ss, cpu);

	/* Return NULL if this subtree is not on-list */
	if (!rstatc->updated_next)
		return NULL;

	/*
	 * Unlink @root from its parent. As the updated_children list is
	 * singly linked, we have to walk it to find the removal point.
	 */
	parent = root->parent;
	if (parent) {
		struct css_rstat_cpu *prstatc;
		struct cgroup_subsys_state **nextp;

		prstatc = css_rstat_cpu(parent, cpu);
		nextp = &prstatc->updated_children;
		while (*nextp != root) {
			struct css_rstat_cpu *nrstatc;

			nrstatc = css_rstat_cpu(*nextp, cpu);
			WARN_ON_ONCE(*nextp == parent);
			nextp = &nrstatc->updated_next;
		}
		*nextp = rstatc->updated_next;
	}

	rstatc->updated_next = NULL;

	/* Push @root to the list first before pushing the children */
	head = root;
	css_rstat_cpu(root, cpu)->rstat_flush_next = NULL;
	child = rstatc->updated_children;
	rstatc->updated_children = root;
	if (child != root)
		head = css_rstat_push_children(head, child, cpu);

	return head;
}

/*
 * A hook for bpf stat collectors to attach to and flush their stats.
 * Together with providing bpf kfuncs for css_rstat_updated() and
 * css_rstat_flush(), this enables a complete workflow where bpf progs that
 * collect cgroup stats can integrate with rstat for efficient flushing.
 *
 * A static noinline declaration here could cause the compiler to optimize away
 * the function. A global noinline declaration will keep the definition, but may
 * optimize away the callsite. Therefore, __weak is needed to ensure that the
 * call is still emitted, by telling the compiler that we don't know what the
 * function might eventually be.
 */

__bpf_hook_start();

__weak noinline void bpf_rstat_flush(struct cgroup *cgrp,
				     struct cgroup *parent, int cpu)
{
}

__bpf_hook_end();

/*
 * Helper functions for locking.
 *
 * This makes it easier to diagnose locking issues and contention in
 * production environments.  The parameter @cpu_in_loop indicate lock
 * was released and re-taken when collection data from the CPUs. The
 * value -1 is used when obtaining the main lock else this is the CPU
 * number processed last.
 */
static inline void __css_rstat_lock(struct cgroup_subsys_state *css,
		int cpu_in_loop)
	__acquires(ss_rstat_lock(css->ss))
{
	struct cgroup *cgrp = css->cgroup;
	spinlock_t *lock;
	bool contended;

	lock = ss_rstat_lock(css->ss);
	contended = !spin_trylock_irq(lock);
	if (contended) {
		trace_cgroup_rstat_lock_contended(cgrp, cpu_in_loop, contended);
		spin_lock_irq(lock);
	}
	trace_cgroup_rstat_locked(cgrp, cpu_in_loop, contended);
}

static inline void __css_rstat_unlock(struct cgroup_subsys_state *css,
				      int cpu_in_loop)
	__releases(ss_rstat_lock(css->ss))
{
	struct cgroup *cgrp = css->cgroup;
	spinlock_t *lock;

	lock = ss_rstat_lock(css->ss);
	trace_cgroup_rstat_unlock(cgrp, cpu_in_loop, false);
	spin_unlock_irq(lock);
}

/**
 * css_rstat_flush - flush stats in @css's rstat subtree
 * @css: target cgroup subsystem state
 *
 * Collect all per-cpu stats in @css's subtree into the global counters
 * and propagate them upwards. After this function returns, all rstat
 * nodes in the subtree have up-to-date ->stat.
 *
 * This also gets all rstat nodes in the subtree including @css off the
 * ->updated_children lists.
 *
 * This function may block.
 */
__bpf_kfunc void css_rstat_flush(struct cgroup_subsys_state *css)
{
	int cpu;
	bool is_self = css_is_self(css);

	/*
	 * Since bpf programs can call this function, prevent access to
	 * uninitialized rstat pointers.
	 */
	if (!css_uses_rstat(css))
		return;

	might_sleep();
	for_each_possible_cpu(cpu) {
		struct cgroup_subsys_state *pos;
		pos = css_rstat_updated_list(css, cpu);
		for (; pos; pos = css_rstat_cpu(pos, cpu)->rstat_flush_next) {
			if (is_self) {
				cgroup_base_stat_flush(pos->cgroup, cpu);
				bpf_rstat_flush(pos->cgroup,
						cgroup_parent(pos->cgroup), cpu);
			} else {
				__css_rstat_lock(css, cpu);
				pos->ss->css_rstat_flush(pos, cpu);
				__css_rstat_unlock(css, cpu);
			}
		}
		if (!cond_resched())
			cpu_relax();
	}
}

int css_rstat_init(struct cgroup_subsys_state *css)
{
	struct cgroup *cgrp = css->cgroup;
	int cpu;
	bool is_self = css_is_self(css);

	if (is_self) {
		/* the root cgrp has rstat_base_cpu preallocated */
		if (!cgrp->rstat_base_cpu) {
			cgrp->rstat_base_cpu = alloc_percpu(struct cgroup_rstat_base_cpu);
			if (!cgrp->rstat_base_cpu)
				return -ENOMEM;
		}
	} else if (css->ss->css_rstat_flush == NULL)
		return 0;

	/* the root cgrp's self css has rstat_cpu preallocated */
	if (!css->rstat_cpu) {
		css->rstat_cpu = alloc_percpu(struct css_rstat_cpu);
		if (!css->rstat_cpu) {
			if (is_self)
				free_percpu(cgrp->rstat_base_cpu);

			return -ENOMEM;
		}
	}

	/* ->updated_children list is self terminated */
	for_each_possible_cpu(cpu) {
		struct css_rstat_cpu *rstatc = css_rstat_cpu(css, cpu);

		rstatc->owner = rstatc->updated_children = css;
		init_llist_node(&rstatc->lnode);
	}

	return 0;
}

void css_rstat_exit(struct cgroup_subsys_state *css)
{
	int cpu;

	if (!css_uses_rstat(css))
		return;

	css_rstat_flush(css);

	/* sanity check */
	for_each_possible_cpu(cpu) {
		struct css_rstat_cpu *rstatc = css_rstat_cpu(css, cpu);

		if (WARN_ON_ONCE(rstatc->updated_children != css) ||
		    WARN_ON_ONCE(rstatc->updated_next))
			return;
	}

	if (css_is_self(css)) {
		struct cgroup *cgrp = css->cgroup;

		free_percpu(cgrp->rstat_base_cpu);
		cgrp->rstat_base_cpu = NULL;
	}

	free_percpu(css->rstat_cpu);
	css->rstat_cpu = NULL;
}

/**
 * ss_rstat_init - subsystem-specific rstat initialization
 * @ss: target subsystem
 *
 * If @ss is NULL, the static locks associated with the base stats
 * are initialized. If @ss is non-NULL, the subsystem-specific locks
 * are initialized.
 */
int __init ss_rstat_init(struct cgroup_subsys *ss)
{
	int cpu;

	if (ss) {
		ss->lhead = alloc_percpu(struct llist_head);
		if (!ss->lhead)
			return -ENOMEM;
	}

	spin_lock_init(ss_rstat_lock(ss));
	for_each_possible_cpu(cpu)
		init_llist_head(ss_lhead_cpu(ss, cpu));

	return 0;
}

/*
 * Functions for cgroup basic resource statistics implemented on top of
 * rstat.
 */
static void cgroup_base_stat_add(struct cgroup_base_stat *dst_bstat,
				 struct cgroup_base_stat *src_bstat)
{
	atomic64_add(atomic64_read(&src_bstat->cputime.utime),
		     &dst_bstat->cputime.utime);
	atomic64_add(atomic64_read(&src_bstat->cputime.stime),
		     &dst_bstat->cputime.stime);
	atomic_long_add(atomic_long_read(&src_bstat->cputime.sum_exec_runtime),
			&dst_bstat->cputime.sum_exec_runtime);
#ifdef CONFIG_SCHED_CORE
	atomic64_add(atomic64_read(&src_bstat->forceidle_sum),
		     &dst_bstat->forceidle_sum);
#endif
}

static void cgroup_base_stat_sub(struct cgroup_base_stat *dst_bstat,
				 struct cgroup_base_stat *src_bstat)
{
	atomic64_sub(atomic64_read(&src_bstat->cputime.utime),
		     &dst_bstat->cputime.utime);
	atomic64_sub(atomic64_read(&src_bstat->cputime.stime),
		     &dst_bstat->cputime.stime);
	atomic_long_sub(atomic_long_read(&src_bstat->cputime.sum_exec_runtime),
			&dst_bstat->cputime.sum_exec_runtime);
#ifdef CONFIG_SCHED_CORE
	atomic64_sub(atomic64_read(&src_bstat->forceidle_sum),
		     &dst_bstat->forceidle_sum);
#endif
	atomic64_sub(atomic64_read(&src_bstat->ntime), &dst_bstat->ntime);
}

static void cgroup_base_stat_copy(struct cgroup_base_stat *dst_bstat,
				  struct cgroup_base_stat *src_bstat)
{
	atomic64_set(&dst_bstat->cputime.stime,
		     atomic64_read(&src_bstat->cputime.stime));
	atomic64_set(&dst_bstat->cputime.utime,
		     atomic64_read(&src_bstat->cputime.utime));
	atomic_long_set(&dst_bstat->cputime.sum_exec_runtime,
			atomic_long_read(&src_bstat->cputime.sum_exec_runtime));
#ifdef CONFIG_SCHED_CORE
	atomic64_set(&dst_bstat->forceidle_sum,
		     atomic64_read(&src_bstat->forceidle_sum));
#endif
	atomic64_set(&dst_bstat->ntime, atomic64_read(&src_bstat->ntime));
}

static void cgroup_base_stat_flush(struct cgroup *cgrp, int cpu)
{
	struct cgroup_rstat_base_cpu *rstatbc = cgroup_rstat_base_cpu(cgrp, cpu);
	struct cgroup *parent = cgroup_parent(cgrp);
	struct cgroup_rstat_base_cpu *prstatbc;
	struct cgroup_base_stat delta;

	/* Root-level stats are sourced from system-wide CPU stats */
	if (!parent)
		return;

	cgroup_base_stat_copy(&delta, &rstatbc->bstat);

	/* propagate per-cpu delta to cgroup and per-cpu global statistics */
	cgroup_base_stat_sub(&delta, &rstatbc->last_bstat);
	cgroup_base_stat_add(&cgrp->bstat, &delta);
	cgroup_base_stat_add(&rstatbc->last_bstat, &delta);
	cgroup_base_stat_add(&rstatbc->subtree_bstat, &delta);

	/* propagate cgroup and per-cpu global delta to parent (unless that's root) */
	if (cgroup_parent(parent)) {
		cgroup_base_stat_copy(&delta, &cgrp->bstat);
		cgroup_base_stat_sub(&delta, &cgrp->last_bstat);
		cgroup_base_stat_add(&parent->bstat, &delta);
		cgroup_base_stat_add(&cgrp->last_bstat, &delta);

		cgroup_base_stat_copy(&delta, &rstatbc->subtree_bstat);
		prstatbc = cgroup_rstat_base_cpu(parent, cpu);
		cgroup_base_stat_sub(&delta, &rstatbc->last_subtree_bstat);
		cgroup_base_stat_add(&prstatbc->subtree_bstat, &delta);
		cgroup_base_stat_add(&rstatbc->last_subtree_bstat, &delta);
	}
}

void __cgroup_account_cputime(struct cgroup *cgrp, u64 delta_exec)
{
	struct cgroup_rstat_base_cpu *rstatbc =
		get_cpu_ptr(cgrp->rstat_base_cpu);
	atomic_long_add(delta_exec, &rstatbc->bstat.cputime.sum_exec_runtime);
	put_cpu_ptr(rstatbc);
}

void __cgroup_account_cputime_field(struct cgroup *cgrp,
				    enum cpu_usage_stat index, u64 delta_exec)
{
	struct cgroup_rstat_base_cpu *rstatbc;

	rstatbc = get_cpu_ptr(cgrp->rstat_base_cpu);

	switch (index) {
	case CPUTIME_NICE:
		atomic64_add(delta_exec, &rstatbc->bstat.ntime);
		fallthrough;
	case CPUTIME_USER:
		atomic64_add(delta_exec, &rstatbc->bstat.cputime.utime);
		break;
	case CPUTIME_SYSTEM:
	case CPUTIME_IRQ:
	case CPUTIME_SOFTIRQ:
		atomic64_add(delta_exec, &rstatbc->bstat.cputime.stime);
		break;
#ifdef CONFIG_SCHED_CORE
	case CPUTIME_FORCEIDLE:
		atomic64_add(delta_exec, &rstatbc->bstat.forceidle_sum);
		break;
#endif
	default:
		break;
	}

	put_cpu_ptr(rstatbc);
}

/*
 * compute the cputime for the root cgroup by getting the per cpu data
 * at a global level, then categorizing the fields in a manner consistent
 * with how it is done by __cgroup_account_cputime_field for each bit of
 * cpu time attributed to a cgroup.
 */
static void root_cgroup_cputime(struct cgroup_base_stat *bstat)
{
	struct atomic_task_cputime *cputime = &bstat->cputime;
	int i;

	memset(bstat, 0, sizeof(*bstat));
	for_each_possible_cpu(i) {
		struct kernel_cpustat kcpustat;
		u64 *cpustat = kcpustat.cpustat;
		u64 user = 0;
		u64 sys = 0;

		kcpustat_cpu_fetch(&kcpustat, i);

		user += cpustat[CPUTIME_USER];
		user += cpustat[CPUTIME_NICE];
		atomic64_add(user, &cputime->utime);

		sys += cpustat[CPUTIME_SYSTEM];
		sys += cpustat[CPUTIME_IRQ];
		sys += cpustat[CPUTIME_SOFTIRQ];
		atomic64_add(sys, &cputime->stime);

		atomic_long_add(sys + user, &cputime->sum_exec_runtime);

#ifdef CONFIG_SCHED_CORE
		atomic64_add(cpustat[CPUTIME_FORCEIDLE], &bstat->forceidle_sum);
#endif
		atomic64_add(cpustat[CPUTIME_NICE], &bstat->ntime);
	}
}


static void cgroup_force_idle_show(struct seq_file *seq, struct cgroup_base_stat *bstat)
{
#ifdef CONFIG_SCHED_CORE
	u64 forceidle_time = atomic64_read(&bstat->forceidle_sum);

	do_div(forceidle_time, NSEC_PER_USEC);
	seq_printf(seq, "core_sched.force_idle_usec %llu\n", forceidle_time);
#endif
}

void cgroup_base_stat_cputime_show(struct seq_file *seq)
{
	struct cgroup *cgrp = seq_css(seq)->cgroup;
	struct cgroup_base_stat bstat;
	unsigned long long sum_exec_runtime;
	u64 utime, stime, ntime;

	if (cgroup_parent(cgrp)) {
		css_rstat_flush(&cgrp->self);
		cgroup_base_stat_copy(&bstat, &cgrp->bstat);
		struct task_cputime cputime = {
			.stime = atomic64_read(&bstat.cputime.stime),
			.utime = atomic64_read(&bstat.cputime.utime),
			.sum_exec_runtime = atomic_long_read(
				&bstat.cputime.sum_exec_runtime)
		};
		cputime_adjust(&cputime, &cgrp->prev_cputime, &cputime.utime,
			       &cputime.stime);
		atomic64_set(&bstat.cputime.utime, cputime.utime);
		atomic64_set(&bstat.cputime.stime, cputime.stime);
	} else
		root_cgroup_cputime(&bstat);

	sum_exec_runtime = atomic_long_read(&bstat.cputime.sum_exec_runtime);

	do_div(sum_exec_runtime, NSEC_PER_USEC);

	utime = atomic64_read(&bstat.cputime.utime);

	do_div(utime, NSEC_PER_USEC);

	stime = atomic64_read(&bstat.cputime.stime);

	do_div(stime, NSEC_PER_USEC);

	ntime = atomic64_read(&bstat.ntime);

	do_div(ntime, NSEC_PER_USEC);

	seq_printf(seq,
		   "usage_usec %llu\n"
		   "user_usec %llu\n"
		   "system_usec %llu\n"
		   "nice_usec %llu\n",
		   sum_exec_runtime,
		   utime,
		   stime,
		   ntime);

	cgroup_force_idle_show(seq, &bstat);
}

/* Add bpf kfuncs for css_rstat_updated() and css_rstat_flush() */
BTF_KFUNCS_START(bpf_rstat_kfunc_ids)
BTF_ID_FLAGS(func, css_rstat_updated)
BTF_ID_FLAGS(func, css_rstat_flush, KF_SLEEPABLE)
BTF_KFUNCS_END(bpf_rstat_kfunc_ids)

static const struct btf_kfunc_id_set bpf_rstat_kfunc_set = {
	.owner          = THIS_MODULE,
	.set            = &bpf_rstat_kfunc_ids,
};

static int __init bpf_rstat_kfunc_init(void)
{
	return register_btf_kfunc_id_set(BPF_PROG_TYPE_TRACING,
					 &bpf_rstat_kfunc_set);
}
late_initcall(bpf_rstat_kfunc_init);
