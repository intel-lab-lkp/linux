// SPDX-License-Identifier: GPL-2.0-only
#include "cgroup-internal.h"

#include <linux/sched/cputime.h>

#include <linux/bpf.h>
#include <linux/btf.h>
#include <linux/btf_ids.h>

#include <trace/events/cgroup.h>

static DEFINE_SPINLOCK(cgroup_rstat_base_lock);
static DEFINE_PER_CPU(raw_spinlock_t, cgroup_rstat_base_cpu_lock);

static spinlock_t cgroup_rstat_subsys_lock[CGROUP_SUBSYS_COUNT];
static DEFINE_PER_CPU(raw_spinlock_t,
		cgroup_rstat_subsys_cpu_lock[CGROUP_SUBSYS_COUNT]);

static void cgroup_base_stat_flush(struct cgroup *cgrp, int cpu);

static struct cgroup_rstat_cpu *cgroup_rstat_cpu(
		struct cgroup_subsys_state *css, int cpu)
{
	return per_cpu_ptr(css->rstat_cpu, cpu);
}

static struct cgroup_rstat_base_cpu *cgroup_rstat_base_cpu(
		struct cgroup_subsys_state *css, int cpu)
{
	return per_cpu_ptr(css->rstat_base_cpu, cpu);
}

static inline bool is_base_css(struct cgroup_subsys_state *css)
{
	return css->ss == NULL;
}

/*
 * Helper functions for rstat per CPU locks.
 *
 * This makes it easier to diagnose locking issues and contention in
 * production environments. The parameter @fast_path determine the
 * tracepoints being added, allowing us to diagnose "flush" related
 * operations without handling high-frequency fast-path "update" events.
 */
static __always_inline
unsigned long _cgroup_rstat_cpu_lock(raw_spinlock_t *cpu_lock, int cpu,
				     struct cgroup *cgrp, const bool fast_path)
{
	unsigned long flags;
	bool contended;

	/*
	 * The _irqsave() is needed because the locks used for flushing are
	 * spinlock_t which is a sleeping lock on PREEMPT_RT. Acquiring this lock
	 * with the _irq() suffix only disables interrupts on a non-PREEMPT_RT
	 * kernel. The raw_spinlock_t below disables interrupts on both
	 * configurations. The _irqsave() ensures that interrupts are always
	 * disabled and later restored.
	 */
	contended = !raw_spin_trylock_irqsave(cpu_lock, flags);
	if (contended) {
		if (fast_path)
			trace_cgroup_rstat_cpu_lock_contended_fastpath(cgrp, cpu, contended);
		else
			trace_cgroup_rstat_cpu_lock_contended(cgrp, cpu, contended);

		raw_spin_lock_irqsave(cpu_lock, flags);
	}

	if (fast_path)
		trace_cgroup_rstat_cpu_locked_fastpath(cgrp, cpu, contended);
	else
		trace_cgroup_rstat_cpu_locked(cgrp, cpu, contended);

	return flags;
}

static __always_inline
void _cgroup_rstat_cpu_unlock(raw_spinlock_t *cpu_lock, int cpu,
			      struct cgroup *cgrp, unsigned long flags,
			      const bool fast_path)
{
	if (fast_path)
		trace_cgroup_rstat_cpu_unlock_fastpath(cgrp, cpu, false);
	else
		trace_cgroup_rstat_cpu_unlock(cgrp, cpu, false);

	raw_spin_unlock_irqrestore(cpu_lock, flags);
}

/**
 * cgroup_rstat_updated - keep track of updated rstat_cpu
 * @css: target cgroup subsystem state
 * @cpu: cpu on which rstat_cpu was updated
 *
 * @css's rstat_cpu on @cpu was updated. Put it on the parent's matching
 * rstat_cpu->updated_children list.  See the comment on top of
 * cgroup_rstat_cpu definition for details.
 */
__bpf_kfunc void cgroup_rstat_updated(
		struct cgroup_subsys_state *css, int cpu)
{
	struct cgroup *cgrp = css->cgroup;
	raw_spinlock_t *cpu_lock;
	unsigned long flags;

	/*
	 * Speculative already-on-list test. This may race leading to
	 * temporary inaccuracies, which is fine.
	 *
	 * Because @parent's updated_children is terminated with @parent
	 * instead of NULL, we can tell whether @css is on the list by
	 * testing the next pointer for NULL.
	 */
	if (data_race(cgroup_rstat_cpu(css, cpu)->updated_next))
		return;

	if (is_base_css(css))
		cpu_lock = per_cpu_ptr(&cgroup_rstat_base_cpu_lock, cpu);
	else
		cpu_lock = per_cpu_ptr(cgroup_rstat_subsys_cpu_lock, cpu) +
			css->ss->id;

	flags = _cgroup_rstat_cpu_lock(cpu_lock, cpu, cgrp, true);

	/* put @css and all ancestors on the corresponding updated lists */
	while (true) {
		struct cgroup_rstat_cpu *rstatc = cgroup_rstat_cpu(css, cpu);
		struct cgroup_subsys_state *parent = css->parent;
		struct cgroup_rstat_cpu *prstatc;

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

		prstatc = cgroup_rstat_cpu(parent, cpu);
		rstatc->updated_next = prstatc->updated_children;
		prstatc->updated_children = css;

		css = parent;
	}

	_cgroup_rstat_cpu_unlock(cpu_lock, cpu, cgrp, flags, true);
}

/**
 * cgroup_rstat_push_children - push children cgroups into the given list
 * @head: current head of the list (= subtree root)
 * @child: first child of the root
 * @cpu: target cpu
 * Return: A new singly linked list of cgroups to be flush
 *
 * Iteratively traverse down the cgroup_rstat_cpu updated tree level by
 * level and push all the parents first before their next level children
 * into a singly linked list built from the tail backward like "pushing"
 * cgroups into a stack. The root is pushed by the caller.
 */
static struct cgroup_subsys_state *cgroup_rstat_push_children(
		struct cgroup_subsys_state *head,
		struct cgroup_subsys_state *child, int cpu)
{
	struct cgroup_subsys_state *chead = child;	/* Head of child css level */
	struct cgroup_subsys_state *ghead = NULL;	/* Head of grandchild css level */
	struct cgroup_subsys_state *parent, *grandchild;
	struct cgroup_rstat_cpu *crstatc;

	child->rstat_flush_next = NULL;

next_level:
	while (chead) {
		child = chead;
		chead = child->rstat_flush_next;
		parent = child->parent;

		/* updated_next is parent cgroup terminated */
		while (child != parent) {
			child->rstat_flush_next = head;
			head = child;
			crstatc = cgroup_rstat_cpu(child, cpu);
			grandchild = crstatc->updated_children;
			if (grandchild != child) {
				/* Push the grand child to the next level */
				crstatc->updated_children = child;
				grandchild->rstat_flush_next = ghead;
				ghead = grandchild;
			}
			child = crstatc->updated_next;
			crstatc->updated_next = NULL;
		}
	}

	if (ghead) {
		chead = ghead;
		ghead = NULL;
		goto next_level;
	}
	return head;
}

/**
 * cgroup_rstat_updated_list - return a list of updated cgroups to be flushed
 * @root: root of the css subtree to traverse
 * @cpu: target cpu
 * Return: A singly linked list of cgroups to be flushed
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
 * here is the cgroup root whose updated_next can be self terminated.
 */
static struct cgroup_subsys_state *cgroup_rstat_updated_list(
		struct cgroup_subsys_state *root, int cpu)
{
	struct cgroup *cgrp = root->cgroup;
	struct cgroup_rstat_cpu *rstatc = cgroup_rstat_cpu(root, cpu);
	struct cgroup_subsys_state *head = NULL, *parent, *child;
	raw_spinlock_t *cpu_lock;
	unsigned long flags;

	if (is_base_css(root))
		cpu_lock = per_cpu_ptr(&cgroup_rstat_base_cpu_lock, cpu);
	else
		cpu_lock = per_cpu_ptr(cgroup_rstat_subsys_cpu_lock, cpu) +
			root->ss->id;

	flags = _cgroup_rstat_cpu_lock(cpu_lock, cpu, cgrp, false);

	/* Return NULL if this subtree is not on-list */
	if (!rstatc->updated_next)
		goto unlock_ret;

	/*
	 * Unlink @root from its parent. As the updated_children list is
	 * singly linked, we have to walk it to find the removal point.
	 */
	parent = root->parent;
	if (parent) {
		struct cgroup_rstat_cpu *prstatc;
		struct cgroup_subsys_state **nextp;

		prstatc = cgroup_rstat_cpu(parent, cpu);
		nextp = &prstatc->updated_children;
		while (*nextp != root) {
			struct cgroup_rstat_cpu *nrstatc;

			nrstatc = cgroup_rstat_cpu(*nextp, cpu);
			WARN_ON_ONCE(*nextp == parent);
			nextp = &nrstatc->updated_next;
		}
		*nextp = rstatc->updated_next;
	}

	rstatc->updated_next = NULL;

	/* Push @root to the list first before pushing the children */
	head = root;
	root->rstat_flush_next = NULL;
	child = rstatc->updated_children;
	rstatc->updated_children = root;
	if (child != root)
		head = cgroup_rstat_push_children(head, child, cpu);
unlock_ret:
	_cgroup_rstat_cpu_unlock(cpu_lock, cpu, cgrp, flags, false);
	return head;
}

/*
 * A hook for bpf stat collectors to attach to and flush their stats.
 * Together with providing bpf kfuncs for cgroup_rstat_updated() and
 * cgroup_rstat_flush(), this enables a complete workflow where bpf progs that
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
static inline void __cgroup_rstat_lock(spinlock_t *lock,
		struct cgroup *cgrp, int cpu_in_loop)
	__acquires(lock)
{
	bool contended;

	contended = !spin_trylock_irq(lock);
	if (contended) {
		trace_cgroup_rstat_lock_contended(cgrp, cpu_in_loop, contended);
		spin_lock_irq(lock);
	}
	trace_cgroup_rstat_locked(cgrp, cpu_in_loop, contended);
}

static inline void __cgroup_rstat_unlock(spinlock_t *lock,
		struct cgroup *cgrp, int cpu_in_loop)
	__releases(lock)
{
	trace_cgroup_rstat_unlock(cgrp, cpu_in_loop, false);
	spin_unlock_irq(lock);
}

/* see cgroup_rstat_flush() */
static void cgroup_rstat_flush_locked(struct cgroup_subsys_state *css,
		spinlock_t *lock)
	__releases(lock) __acquires(lock)
{
	struct cgroup *cgrp = css->cgroup;
	int cpu;

	lockdep_assert_held(&lock);

	for_each_possible_cpu(cpu) {
		struct cgroup_subsys_state *pos;

		pos = cgroup_rstat_updated_list(css, cpu);
		for (; pos; pos = pos->rstat_flush_next) {
			if (!pos->ss)
				cgroup_base_stat_flush(pos->cgroup, cpu);
			else
				pos->ss->css_rstat_flush(pos, cpu);

			bpf_rstat_flush(pos->cgroup, cgroup_parent(pos->cgroup), cpu);
		}

		/* play nice and yield if necessary */
		if (need_resched() || spin_needbreak(lock)) {
			__cgroup_rstat_unlock(lock, cgrp, cpu);
			if (!cond_resched())
				cpu_relax();
			__cgroup_rstat_lock(lock, cgrp, cpu);
		}
	}
}

/**
 * cgroup_rstat_flush - flush stats in @css's rstat subtree
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
__bpf_kfunc void cgroup_rstat_flush(struct cgroup_subsys_state *css)
{
	struct cgroup *cgrp = css->cgroup;
	spinlock_t *lock;

	if (is_base_css(css))
		lock = &cgroup_rstat_base_lock;
	else
		lock = &cgroup_rstat_subsys_lock[css->ss->id];

	might_sleep();

	__cgroup_rstat_lock(lock, cgrp, -1);
	cgroup_rstat_flush_locked(css, lock);
	__cgroup_rstat_unlock(lock, cgrp, -1);
}

/**
 * cgroup_rstat_flush_hold - flush stats in @css's rstat subtree and hold
 * @css: target subsystem state
 *
 * Flush stats in @css's rstat subtree and prevent further flushes. Must be
 * paired with cgroup_rstat_flush_release().
 *
 * This function may block.
 */
void cgroup_rstat_flush_hold(struct cgroup_subsys_state *css)
{
	struct cgroup *cgrp = css->cgroup;
	spinlock_t *lock;

	if (is_base_css(css))
		lock = &cgroup_rstat_base_lock;
	else
		lock = &cgroup_rstat_subsys_lock[css->ss->id];

	might_sleep();
	__cgroup_rstat_lock(lock, cgrp, -1);
	cgroup_rstat_flush_locked(css, lock);
}

/**
 * cgroup_rstat_flush_release - release cgroup_rstat_flush_hold()
 * @css: css that was previously used for the call to flush hold
 */
void cgroup_rstat_flush_release(struct cgroup_subsys_state *css)
{
	struct cgroup *cgrp = css->cgroup;
	spinlock_t *lock;

	if (is_base_css(css))
		lock = &cgroup_rstat_base_lock;
	else
		lock = &cgroup_rstat_subsys_lock[css->ss->id];

	__cgroup_rstat_unlock(lock, cgrp, -1);
}

int cgroup_rstat_init(struct cgroup_subsys_state *css)
{
	int cpu;

	/* the root cgrp's self css has rstat_cpu preallocated */
	if (!css->rstat_cpu) {
		if (is_base_css(css)) {
			css->rstat_base_cpu = alloc_percpu(struct cgroup_rstat_base_cpu);
			if (!css->rstat_base_cpu)
				return -ENOMEM;

			for_each_possible_cpu(cpu) {
				struct cgroup_rstat_base_cpu *rstatc;

				rstatc = cgroup_rstat_base_cpu(css, cpu);
				rstatc->self.updated_children = css;
				u64_stats_init(&rstatc->bsync);
			}
		} else {
			css->rstat_cpu = alloc_percpu(struct cgroup_rstat_cpu);
			if (!css->rstat_cpu)
				return -ENOMEM;

			for_each_possible_cpu(cpu) {
				struct cgroup_rstat_cpu *rstatc;

				rstatc = cgroup_rstat_cpu(css, cpu);
				rstatc->updated_children = css;
			}
		}

	}

	return 0;
}

void cgroup_rstat_exit(struct cgroup_subsys_state *css)
{
	int cpu;

	cgroup_rstat_flush(css);

	/* sanity check */
	for_each_possible_cpu(cpu) {
		struct cgroup_rstat_cpu *rstatc = cgroup_rstat_cpu(css, cpu);

		if (WARN_ON_ONCE(rstatc->updated_children != css) ||
		    WARN_ON_ONCE(rstatc->updated_next))
			return;
	}

	free_percpu(css->rstat_cpu);
	css->rstat_cpu = NULL;
}

void __init cgroup_rstat_boot(void)
{
	struct cgroup_subsys *ss;
	int cpu, ssid;

	for_each_subsys(ss, ssid) {
		spin_lock_init(&cgroup_rstat_subsys_lock[ssid]);
	}

	for_each_possible_cpu(cpu) {
		raw_spin_lock_init(per_cpu_ptr(&cgroup_rstat_base_cpu_lock, cpu));

		for_each_subsys(ss, ssid) {
			raw_spin_lock_init(
					per_cpu_ptr(cgroup_rstat_subsys_cpu_lock, cpu) + ssid);
		}
	}
}

/*
 * Functions for cgroup basic resource statistics implemented on top of
 * rstat.
 */
static void cgroup_base_stat_add(struct cgroup_base_stat *dst_bstat,
				 struct cgroup_base_stat *src_bstat)
{
	dst_bstat->cputime.utime += src_bstat->cputime.utime;
	dst_bstat->cputime.stime += src_bstat->cputime.stime;
	dst_bstat->cputime.sum_exec_runtime += src_bstat->cputime.sum_exec_runtime;
#ifdef CONFIG_SCHED_CORE
	dst_bstat->forceidle_sum += src_bstat->forceidle_sum;
#endif
	dst_bstat->ntime += src_bstat->ntime;
}

static void cgroup_base_stat_sub(struct cgroup_base_stat *dst_bstat,
				 struct cgroup_base_stat *src_bstat)
{
	dst_bstat->cputime.utime -= src_bstat->cputime.utime;
	dst_bstat->cputime.stime -= src_bstat->cputime.stime;
	dst_bstat->cputime.sum_exec_runtime -= src_bstat->cputime.sum_exec_runtime;
#ifdef CONFIG_SCHED_CORE
	dst_bstat->forceidle_sum -= src_bstat->forceidle_sum;
#endif
	dst_bstat->ntime -= src_bstat->ntime;
}

static void cgroup_base_stat_flush(struct cgroup *cgrp, int cpu)
{
	struct cgroup_rstat_base_cpu *rstatc = cgroup_rstat_base_cpu(
			&cgrp->self, cpu);
	struct cgroup *parent = cgroup_parent(cgrp);
	struct cgroup_rstat_base_cpu *prstatc;
	struct cgroup_base_stat delta;
	unsigned seq;

	/* Root-level stats are sourced from system-wide CPU stats */
	if (!parent)
		return;

	/* fetch the current per-cpu values */
	do {
		seq = __u64_stats_fetch_begin(&rstatc->bsync);
		delta = rstatc->bstat;
	} while (__u64_stats_fetch_retry(&rstatc->bsync, seq));

	/* propagate per-cpu delta to cgroup and per-cpu global statistics */
	cgroup_base_stat_sub(&delta, &rstatc->last_bstat);
	cgroup_base_stat_add(&cgrp->bstat, &delta);
	cgroup_base_stat_add(&rstatc->last_bstat, &delta);
	cgroup_base_stat_add(&rstatc->subtree_bstat, &delta);

	/* propagate cgroup and per-cpu global delta to parent (unless that's root) */
	if (cgroup_parent(parent)) {
		delta = cgrp->bstat;
		cgroup_base_stat_sub(&delta, &cgrp->last_bstat);
		cgroup_base_stat_add(&parent->bstat, &delta);
		cgroup_base_stat_add(&cgrp->last_bstat, &delta);

		delta = rstatc->subtree_bstat;
		prstatc = cgroup_rstat_base_cpu(&parent->self, cpu);
		cgroup_base_stat_sub(&delta, &rstatc->last_subtree_bstat);
		cgroup_base_stat_add(&prstatc->subtree_bstat, &delta);
		cgroup_base_stat_add(&rstatc->last_subtree_bstat, &delta);
	}
}

static struct cgroup_rstat_base_cpu *
cgroup_base_stat_cputime_account_begin(struct cgroup *cgrp, unsigned long *flags)
{
	struct cgroup_rstat_base_cpu *rstatc;

	rstatc = get_cpu_ptr(cgrp->self.rstat_base_cpu);
	*flags = u64_stats_update_begin_irqsave(&rstatc->bsync);
	return rstatc;
}

static void cgroup_base_stat_cputime_account_end(struct cgroup *cgrp,
						 struct cgroup_rstat_base_cpu *rstatc,
						 unsigned long flags)
{
	u64_stats_update_end_irqrestore(&rstatc->bsync, flags);
	cgroup_rstat_updated(&cgrp->self, smp_processor_id());
	put_cpu_ptr(rstatc);
}

void __cgroup_account_cputime(struct cgroup *cgrp, u64 delta_exec)
{
	struct cgroup_rstat_base_cpu *rstatc;
	unsigned long flags;

	rstatc = cgroup_base_stat_cputime_account_begin(cgrp, &flags);
	rstatc->bstat.cputime.sum_exec_runtime += delta_exec;
	cgroup_base_stat_cputime_account_end(cgrp, rstatc, flags);
}

void __cgroup_account_cputime_field(struct cgroup *cgrp,
				    enum cpu_usage_stat index, u64 delta_exec)
{
	struct cgroup_rstat_base_cpu *rstatc;
	unsigned long flags;

	rstatc = cgroup_base_stat_cputime_account_begin(cgrp, &flags);

	switch (index) {
	case CPUTIME_NICE:
		rstatc->bstat.ntime += delta_exec;
		fallthrough;
	case CPUTIME_USER:
		rstatc->bstat.cputime.utime += delta_exec;
		break;
	case CPUTIME_SYSTEM:
	case CPUTIME_IRQ:
	case CPUTIME_SOFTIRQ:
		rstatc->bstat.cputime.stime += delta_exec;
		break;
#ifdef CONFIG_SCHED_CORE
	case CPUTIME_FORCEIDLE:
		rstatc->bstat.forceidle_sum += delta_exec;
		break;
#endif
	default:
		break;
	}

	cgroup_base_stat_cputime_account_end(cgrp, rstatc, flags);
}

/*
 * compute the cputime for the root cgroup by getting the per cpu data
 * at a global level, then categorizing the fields in a manner consistent
 * with how it is done by __cgroup_account_cputime_field for each bit of
 * cpu time attributed to a cgroup.
 */
static void root_cgroup_cputime(struct cgroup_base_stat *bstat)
{
	struct task_cputime *cputime = &bstat->cputime;
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
		cputime->utime += user;

		sys += cpustat[CPUTIME_SYSTEM];
		sys += cpustat[CPUTIME_IRQ];
		sys += cpustat[CPUTIME_SOFTIRQ];
		cputime->stime += sys;

		cputime->sum_exec_runtime += user;
		cputime->sum_exec_runtime += sys;

#ifdef CONFIG_SCHED_CORE
		bstat->forceidle_sum += cpustat[CPUTIME_FORCEIDLE];
#endif
		bstat->ntime += cpustat[CPUTIME_NICE];
	}
}


static void cgroup_force_idle_show(struct seq_file *seq, struct cgroup_base_stat *bstat)
{
#ifdef CONFIG_SCHED_CORE
	u64 forceidle_time = bstat->forceidle_sum;

	do_div(forceidle_time, NSEC_PER_USEC);
	seq_printf(seq, "core_sched.force_idle_usec %llu\n", forceidle_time);
#endif
}

void cgroup_base_stat_cputime_show(struct seq_file *seq)
{
	struct cgroup *cgrp = seq_css(seq)->cgroup;
	u64 usage, utime, stime, ntime;

	if (cgroup_parent(cgrp)) {
		cgroup_rstat_flush_hold(&cgrp->self);
		usage = cgrp->bstat.cputime.sum_exec_runtime;
		cputime_adjust(&cgrp->bstat.cputime, &cgrp->prev_cputime,
			       &utime, &stime);
		ntime = cgrp->bstat.ntime;
		cgroup_rstat_flush_release(&cgrp->self);
	} else {
		/* cgrp->bstat of root is not actually used, reuse it */
		root_cgroup_cputime(&cgrp->bstat);
		usage = cgrp->bstat.cputime.sum_exec_runtime;
		utime = cgrp->bstat.cputime.utime;
		stime = cgrp->bstat.cputime.stime;
		ntime = cgrp->bstat.ntime;
	}

	do_div(usage, NSEC_PER_USEC);
	do_div(utime, NSEC_PER_USEC);
	do_div(stime, NSEC_PER_USEC);
	do_div(ntime, NSEC_PER_USEC);

	seq_printf(seq, "usage_usec %llu\n"
			"user_usec %llu\n"
			"system_usec %llu\n"
			"nice_usec %llu\n",
			usage, utime, stime, ntime);

	cgroup_force_idle_show(seq, &cgrp->bstat);
}

/* Add bpf kfuncs for cgroup_rstat_updated() and cgroup_rstat_flush() */
BTF_KFUNCS_START(bpf_rstat_kfunc_ids)
BTF_ID_FLAGS(func, cgroup_rstat_updated)
BTF_ID_FLAGS(func, cgroup_rstat_flush, KF_SLEEPABLE)
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
