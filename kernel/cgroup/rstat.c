// SPDX-License-Identifier: GPL-2.0-only
#include "cgroup-internal.h"

#include <linux/sched/cputime.h>

#include <linux/bpf.h>
#include <linux/btf.h>
#include <linux/btf_ids.h>

#include <trace/events/cgroup.h>

static DEFINE_PER_CPU(raw_spinlock_t, rstat_base_cpu_lock);

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

static raw_spinlock_t *ss_rstat_cpu_lock(struct cgroup_subsys *ss, int cpu)
{
	if (ss) {
		/*
		 * Depending on config, the subsystem per-cpu lock type may be an
		 * empty struct. In enviromnents where this is the case, allocation
		 * of this field is not performed in ss_rstat_init(). Avoid a
		 * cpu-based offset relative to NULL by returning early. When the
		 * lock type is zero in size, the corresponding lock functions are
		 * no-ops so passing them NULL is acceptable.
		 */
		if (sizeof(*ss->rstat_ss_cpu_lock) == 0)
			return NULL;

		return per_cpu_ptr(ss->rstat_ss_cpu_lock, cpu);
	}

	return per_cpu_ptr(&rstat_base_cpu_lock, cpu);
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
unsigned long _css_rstat_cpu_lock(struct cgroup_subsys_state *css, int cpu,
		const bool fast_path)
{
	struct cgroup *cgrp = css->cgroup;
	raw_spinlock_t *cpu_lock;
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
	cpu_lock = ss_rstat_cpu_lock(css->ss, cpu);
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
void _css_rstat_cpu_unlock(struct cgroup_subsys_state *css, int cpu,
		unsigned long flags, const bool fast_path)
{
	struct cgroup *cgrp = css->cgroup;
	raw_spinlock_t *cpu_lock;

	if (fast_path)
		trace_cgroup_rstat_cpu_unlock_fastpath(cgrp, cpu, false);
	else
		trace_cgroup_rstat_cpu_unlock(cgrp, cpu, false);

	cpu_lock = ss_rstat_cpu_lock(css->ss, cpu);
	raw_spin_unlock_irqrestore(cpu_lock, flags);
}

/**
 * css_rstat_updated - keep track of updated rstat_cpu
 * @css: target cgroup subsystem state
 * @cpu: cpu on which rstat_cpu was updated
 *
 * @css's rstat_cpu on @cpu was updated. Put it on the parent's matching
 * rstat_cpu->updated_children list. See the comment on top of
 * css_rstat_cpu definition for details.
 */
__bpf_kfunc void css_rstat_updated(struct cgroup_subsys_state *css, int cpu)
{
	unsigned long flags;

	/*
	 * Since bpf programs can call this function, prevent access to
	 * uninitialized rstat pointers.
	 */
	if (!css_uses_rstat(css))
		return;

	/*
	 * Speculative already-on-list test. This may race leading to
	 * temporary inaccuracies, which is fine.
	 *
	 * Because @parent's updated_children is terminated with @parent
	 * instead of NULL, we can tell whether @css is on the list by
	 * testing the next pointer for NULL.
	 */
	if (data_race(css_rstat_cpu(css, cpu)->updated_next))
		return;

	flags = _css_rstat_cpu_lock(css, cpu, true);

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

	_css_rstat_cpu_unlock(css, cpu, flags, true);
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

	/* Return NULL if this subtree is not on-list */
	if (!rstatc->updated_next)
		goto unlock_ret;

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
unlock_ret:
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
		unsigned long flags;

		flags = _css_rstat_cpu_lock(css, cpu, false);
		pos = css_rstat_updated_list(css, cpu);
		for (; pos; pos = css_rstat_cpu(pos, cpu)->rstat_flush_next) {
			if (is_self) {
				cgroup_base_stat_flush(pos->cgroup, cpu);
				bpf_rstat_flush(pos->cgroup,
						cgroup_parent(pos->cgroup), cpu);
			} else
				pos->ss->css_rstat_flush(pos, cpu);
		}
		_css_rstat_cpu_unlock(css, cpu, flags, false);
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

		rstatc->updated_children = css;

		if (is_self) {
			struct cgroup_rstat_base_cpu *rstatbc;

			rstatbc = cgroup_rstat_base_cpu(cgrp, cpu);
		}
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

	/*
	 * Depending on config, the subsystem per-cpu lock type may be an empty
	 * struct. Avoid allocating a size of zero in this case.
	 */
	if (ss && sizeof(*ss->rstat_ss_cpu_lock)) {
		ss->rstat_ss_cpu_lock = alloc_percpu(raw_spinlock_t);
		if (!ss->rstat_ss_cpu_lock)
			return -ENOMEM;
	}

	for_each_possible_cpu(cpu)
		raw_spin_lock_init(ss_rstat_cpu_lock(ss, cpu));

	return 0;
}

/*
 * Functions for cgroup basic resource statistics implemented on top of
 * rstat.
 */
static void cgroup_base_stat_add(struct cgroup_base_stat *dst_bstat,
				struct cgroup_base_stat *src_bstat)
{
	atomic64_add(atomic64_read(&src_bstat->cputime.utime), &dst_bstat->cputime.utime);
	atomic64_add(atomic64_read(&src_bstat->cputime.stime), &dst_bstat->cputime.stime);
	atomic_long_add(atomic_long_read(&src_bstat->cputime.sum_exec_runtime),
			&dst_bstat->cputime.sum_exec_runtime);
#ifdef CONFIG_SCHED_CORE
	atomic64_add(atomic64_read(&src_bstat->forceidle_sum), &dst_bstat->forceidle_sum);
#endif
	atomic64_add(atomic64_read(&src_bstat->ntime), &dst_bstat->ntime);
}

static void cgroup_base_stat_sub(struct cgroup_base_stat *dst_bstat,
				struct cgroup_base_stat *src_bstat)
{
	atomic64_sub(atomic64_read(&src_bstat->cputime.utime), &dst_bstat->cputime.utime);
	atomic64_sub(atomic64_read(&src_bstat->cputime.stime), &dst_bstat->cputime.stime);
	atomic_long_sub(atomic_long_read(&src_bstat->cputime.sum_exec_runtime),
			&dst_bstat->cputime.sum_exec_runtime);
#ifdef CONFIG_SCHED_CORE
	atomic64_sub(atomic64_read(&src_bstat->forceidle_sum), &dst_bstat->forceidle_sum);
#endif
	atomic64_sub(atomic64_read(&src_bstat->ntime), &dst_bstat->ntime);
}

static void cgroup_base_stat_copy(struct cgroup_base_stat *dst_bstat,
				struct cgroup_base_stat *src_bstat)
{
		atomic64_set(&dst_bstat->cputime.stime, atomic64_read(&src_bstat->cputime.stime));
		atomic64_set(&dst_bstat->cputime.utime, atomic64_read(&src_bstat->cputime.utime));
		atomic_long_set(&dst_bstat->cputime.sum_exec_runtime,
			atomic_long_read(&src_bstat->cputime.sum_exec_runtime));
#ifdef CONFIG_SCHED_CORE
		atomic64_set(&dst_bstat->forceidle_sum, atomic64_read(&src_bstat->forceidle_sum));
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

	/* fetch the current per-cpu values */
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
	struct cgroup_rstat_base_cpu *rstatbc;

	rstatbc = get_cpu_ptr(cgrp->rstat_base_cpu);
	atomic_long_add(delta_exec, &rstatbc->bstat.cputime.sum_exec_runtime);
	css_rstat_updated(&cgrp->self, smp_processor_id());
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

		atomic64_add(sys + user, &cputime->sum_exec_runtime);

#ifdef CONFIG_SCHED_CORE
		atomic64_add(cpustat[CPUTIME_FORCEIDLE], &bstat->forceidle_sum);
#endif
		atomic64_add(cpustat[CPUTIME_NICE], &bstat->ntime);
	}
}


static void cgroup_force_idle_show(struct seq_file *seq, struct cgroup_base_stat *bstat)
{
#ifdef CONFIG_SCHED_CORE
	s64 forceidle_time = atomic64_read(&bstat->forceidle_sum);

	do_div(forceidle_time, NSEC_PER_USEC);
	seq_printf(seq, "core_sched.force_idle_usec %llu\n", forceidle_time);
#endif
}

void cgroup_base_stat_cputime_show(struct seq_file *seq)
{
	struct cgroup *cgrp = seq_css(seq)->cgroup;
	struct cgroup_base_stat bstat;
	unsigned long long sum_exec_runtime;
	s64 utime, stime, ntime;

	if (cgroup_parent(cgrp)) {
		css_rstat_flush(&cgrp->self);
		cgroup_base_stat_copy(&bstat, &cgrp->bstat);
		struct task_cputime cputime = {
			.stime = atomic64_read(&bstat.cputime.stime),
			.utime = atomic64_read(&bstat.cputime.utime),
			.sum_exec_runtime = atomic_long_read(
				&bstat.cputime.sum_exec_runtime)
		};
		cputime_adjust(&cputime, &cgrp->prev_cputime,
			       &cputime.utime, &cputime.stime);
		atomic64_set(&bstat.cputime.utime, cputime.utime);
		atomic64_set(&bstat.cputime.stime, cputime.stime);
	} else
		root_cgroup_cputime(&bstat);
	sum_exec_runtime = atomic_long_read(&bstat.cputime.sum_exec_runtime);

	do_div(sum_exec_runtime, NSEC_PER_USEC);

	utime = atomic64_read(&bstat.cputime.utime);

	do_div(utime, NSEC_PER_USEC);

	stime =	atomic64_read(&bstat.cputime.stime);

	do_div(stime, NSEC_PER_USEC);

	ntime = atomic64_read(&bstat.ntime);

	do_div(ntime, NSEC_PER_USEC);

	seq_printf(seq, "usage_usec %llu\n"
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
