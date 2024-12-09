// SPDX-License-Identifier: GPL-2.0
/*
 * BPF extensible scheduler class: Documentation/scheduler/sched-ext.rst
 *
 * Built-in idle CPU tracking policy.
 *
 * Copyright (c) 2022 Meta Platforms, Inc. and affiliates.
 * Copyright (c) 2022 Tejun Heo <tj@kernel.org>
 * Copyright (c) 2022 David Vernet <dvernet@meta.com>
 * Copyright (c) 2024 Andrea Righi <arighi@nvidia.com>
 */
static DEFINE_STATIC_KEY_FALSE(scx_builtin_idle_enabled);

#ifdef CONFIG_SMP
#ifdef CONFIG_CPUMASK_OFFSTACK
#define CL_ALIGNED_IF_ONSTACK
#else
#define CL_ALIGNED_IF_ONSTACK __cacheline_aligned_in_smp
#endif

struct idle_cpumask {
	cpumask_var_t cpu;
	cpumask_var_t smt;
};

static DEFINE_STATIC_KEY_FALSE(scx_selcpu_topo_llc);
static DEFINE_STATIC_KEY_FALSE(scx_builtin_idle_per_node);

/*
 * cpumasks to track idle CPUs within each NUMA node.
 *
 * If SCX_OPS_BUILTIN_IDLE_PER_NODE is not specified, a single flat cpumask
 * from node 0 is used to track all idle CPUs system-wide.
 */
static struct idle_cpumask **idle_masks CL_ALIGNED_IF_ONSTACK;

static struct cpumask *get_idle_cpumask_node(int node)
{
	if (!static_branch_maybe(CONFIG_NUMA, &scx_builtin_idle_per_node))
		return idle_masks[0]->cpu;

	if (node < 0 || node >= num_possible_nodes())
		return NULL;
	return idle_masks[node]->cpu;
}

static struct cpumask *get_idle_smtmask_node(int node)
{
	if (!static_branch_maybe(CONFIG_NUMA, &scx_builtin_idle_per_node))
		return idle_masks[0]->smt;

	if (node < 0 || node >= num_possible_nodes())
		return NULL;
	return idle_masks[node]->smt;
}

static struct cpumask *get_curr_idle_cpumask(void)
{
	int node = cpu_to_node(smp_processor_id());

	return get_idle_cpumask_node(node);
}

static struct cpumask *get_curr_idle_smtmask(void)
{
	int node = cpu_to_node(smp_processor_id());

	if (sched_smt_active())
		return get_idle_smtmask_node(node);
	else
		return get_idle_cpumask_node(node);
}

static void idle_masks_init(void)
{
	int node;

	idle_masks = kcalloc(num_possible_nodes(), sizeof(*idle_masks), GFP_KERNEL);
	BUG_ON(!idle_masks);

	for_each_node_state(node, N_POSSIBLE) {
		idle_masks[node] = kzalloc_node(sizeof(**idle_masks), GFP_KERNEL, node);
		BUG_ON(!idle_masks[node]);

		BUG_ON(!alloc_cpumask_var_node(&idle_masks[node]->cpu, GFP_KERNEL, node));
		BUG_ON(!alloc_cpumask_var_node(&idle_masks[node]->smt, GFP_KERNEL, node));
	}
}

static bool test_and_clear_cpu_idle(int cpu)
{
	int node = cpu_to_node(cpu);
	struct cpumask *idle_cpu = get_idle_cpumask_node(node);

#ifdef CONFIG_SCHED_SMT
	/*
	 * SMT mask should be cleared whether we can claim @cpu or not. The SMT
	 * cluster is not wholly idle either way. This also prevents
	 * scx_pick_idle_cpu() from getting caught in an infinite loop.
	 */
	if (sched_smt_active()) {
		const struct cpumask *smt = cpu_smt_mask(cpu);
		struct cpumask *idle_smt = get_idle_smtmask_node(node);

		/*
		 * If offline, @cpu is not its own sibling and
		 * scx_pick_idle_cpu() can get caught in an infinite loop as
		 * @cpu is never cleared from the idle SMT mask. Ensure that
		 * @cpu is eventually cleared.
		 *
		 * NOTE: Use cpumask_intersects() and cpumask_test_cpu() to
		 * reduce memory writes, which may help alleviate cache
		 * coherence pressure.
		 */
		if (cpumask_intersects(smt, idle_smt))
			cpumask_andnot(idle_smt, idle_smt, smt);
		else if (cpumask_test_cpu(cpu, idle_smt))
			__cpumask_clear_cpu(cpu, idle_smt);
	}
#endif
	return cpumask_test_and_clear_cpu(cpu, idle_cpu);
}

static s32 scx_pick_idle_cpu_from_node(int node, const struct cpumask *cpus_allowed, u64 flags)
{
	int cpu;

retry:
	if (sched_smt_active()) {
		cpu = cpumask_any_and_distribute(get_idle_smtmask_node(node), cpus_allowed);
		if (cpu < nr_cpu_ids)
			goto found;

		if (flags & SCX_PICK_IDLE_CORE)
			return -EBUSY;
	}

	cpu = cpumask_any_and_distribute(get_idle_cpumask_node(node), cpus_allowed);
	if (cpu < nr_cpu_ids)
		goto found;

	return -EBUSY;

found:
	if (test_and_clear_cpu_idle(cpu))
		return cpu;
	goto retry;

}

static s32 scx_pick_idle_cpu(const struct cpumask *cpus_allowed, s32 prev_cpu, u64 flags)
{
	const struct cpumask *node_mask;
	s32 cpu;

	/*
	 * Only node 0 is used if per-node idle cpumasks are disabled.
	 */
	if (!static_branch_maybe(CONFIG_NUMA, &scx_builtin_idle_per_node))
		return scx_pick_idle_cpu_from_node(0, cpus_allowed, flags);

	/*
	 * Traverse all nodes in order of increasing distance, starting from
	 * prev_cpu's node.
	 */
	rcu_read_lock();
	for_each_numa_hop_mask(node_mask, cpu_to_node(prev_cpu)) {
		/*
		 * scx_pick_idle_cpu_from_node() can be expensive and redundant
		 * if none of the CPUs in the NUMA node can be used (according
		 * to cpus_allowed).
		 *
		 * Therefore, check if the NUMA node is usable in advance to
		 * save some CPU cycles.
		 */
		if (!cpumask_intersects(node_mask, cpus_allowed))
			continue;

		/*
		 * It would be nice to have a "node" iterator, instead of the
		 * cpumask, to get rid of the cpumask_first() to determine the
		 * node.
		 */
		cpu = cpumask_first(node_mask);
		if (cpu >= nr_cpu_ids)
			continue;

		cpu = scx_pick_idle_cpu_from_node(cpu_to_node(cpu), cpus_allowed, flags);
		if (cpu >= 0)
			goto out_unlock;
	}
	cpu = -EBUSY;

out_unlock:
	rcu_read_unlock();
	return cpu;
}

/*
 * Return the amount of CPUs in the same LLC domain of @cpu (or zero if the LLC
 * domain is not defined).
 */
static unsigned int llc_weight(s32 cpu)
{
	struct sched_domain *sd;

	sd = rcu_dereference(per_cpu(sd_llc, cpu));
	if (!sd)
		return 0;

	return sd->span_weight;
}

/*
 * Return the cpumask representing the LLC domain of @cpu (or NULL if the LLC
 * domain is not defined).
 */
static struct cpumask *llc_span(s32 cpu)
{
	struct sched_domain *sd;

	sd = rcu_dereference(per_cpu(sd_llc, cpu));
	if (!sd)
		return 0;

	return sched_domain_span(sd);
}

/*
 * Return the amount of CPUs in the same NUMA domain of @cpu (or zero if the
 * NUMA domain is not defined).
 */
static unsigned int numa_weight(s32 cpu)
{
	struct sched_domain *sd;
	struct sched_group *sg;

	sd = rcu_dereference(per_cpu(sd_numa, cpu));
	if (!sd)
		return 0;
	sg = sd->groups;
	if (!sg)
		return 0;

	return sg->group_weight;
}

/*
 * Return true if the LLC domains do not perfectly overlap with the NUMA
 * domains, false otherwise.
 */
static bool llc_numa_mismatch(void)
{
	int cpu;

	/*
	 * We need to scan all online CPUs to verify whether their scheduling
	 * domains overlap.
	 *
	 * While it is rare to encounter architectures with asymmetric NUMA
	 * topologies, CPU hotplugging or virtualized environments can result
	 * in asymmetric configurations.
	 *
	 * For example:
	 *
	 *  NUMA 0:
	 *    - LLC 0: cpu0..cpu7
	 *    - LLC 1: cpu8..cpu15 [offline]
	 *
	 *  NUMA 1:
	 *    - LLC 0: cpu16..cpu23
	 *    - LLC 1: cpu24..cpu31
	 *
	 * In this case, if we only check the first online CPU (cpu0), we might
	 * incorrectly assume that the LLC and NUMA domains are fully
	 * overlapping, which is incorrect (as NUMA 1 has two distinct LLC
	 * domains).
	 */
	for_each_online_cpu(cpu)
		if (llc_weight(cpu) != numa_weight(cpu))
			return true;

	return false;
}

/*
 * Initialize topology-aware scheduling.
 *
 * Detect if the system has multiple LLC or multiple NUMA domains and enable
 * cache-aware / NUMA-aware scheduling optimizations in the default CPU idle
 * selection policy.
 *
 * Assumption: the kernel's internal topology representation assumes that each
 * CPU belongs to a single LLC domain, and that each LLC domain is entirely
 * contained within a single NUMA node.
 */
static void update_selcpu_topology(struct sched_ext_ops *ops)
{
	bool enable_llc = false;
	unsigned int nr_cpus;
	s32 cpu = cpumask_first(cpu_online_mask);

	/*
	 * Enable LLC domain optimization only when there are multiple LLC
	 * domains among the online CPUs. If all online CPUs are part of a
	 * single LLC domain, the idle CPU selection logic can choose any
	 * online CPU without bias.
	 *
	 * Note that it is sufficient to check the LLC domain of the first
	 * online CPU to determine whether a single LLC domain includes all
	 * CPUs.
	 */
	rcu_read_lock();
	nr_cpus = llc_weight(cpu);
	if (nr_cpus > 0) {
		if (nr_cpus < num_online_cpus())
			enable_llc = true;
		/*
		 * No need to enable LLC optimization if the LLC domains are
		 * perfectly overlapping with the NUMA domains when per-node
		 * cpumasks are enabled.
		 */
		if ((ops->flags & SCX_OPS_BUILTIN_IDLE_PER_NODE) &&
		    !llc_numa_mismatch())
			enable_llc = false;
		pr_debug("sched_ext: LLC=%*pb weight=%u\n",
			 cpumask_pr_args(llc_span(cpu)), llc_weight(cpu));
	}
	rcu_read_unlock();

	pr_debug("sched_ext: LLC idle selection %s\n",
		 enable_llc ? "enabled" : "disabled");

	if (enable_llc)
		static_branch_enable_cpuslocked(&scx_selcpu_topo_llc);
	else
		static_branch_disable_cpuslocked(&scx_selcpu_topo_llc);

	/*
	 * Check if we need to enable per-node cpumasks.
	 */
	if (ops->flags & SCX_OPS_BUILTIN_IDLE_PER_NODE)
		static_branch_enable_cpuslocked(&scx_builtin_idle_per_node);
	else
		static_branch_disable_cpuslocked(&scx_builtin_idle_per_node);
}

/*
 * Built-in CPU idle selection policy:
 *
 * 1. Prioritize full-idle cores:
 *   - always prioritize CPUs from fully idle cores (both logical CPUs are
 *     idle) to avoid interference caused by SMT.
 *
 * 2. Reuse the same CPU:
 *   - prefer the last used CPU to take advantage of cached data (L1, L2) and
 *     branch prediction optimizations.
 *
 * 3. Pick a CPU within the same LLC (Last-Level Cache):
 *   - if the above conditions aren't met, pick a CPU that shares the same LLC
 *     to maintain cache locality.
 *
 * 4. Pick a CPU within the same NUMA node, if enabled:
 *   - choose a CPU from the same NUMA node to reduce memory access latency.
 *
 * Step 3 is performed only if the system has multiple LLC domains that are not
 * perfectly overlapping with the NUMA domains (see scx_selcpu_topo_llc).
 *
 * NOTE: tasks that can only run on 1 CPU are excluded by this logic, because
 * we never call ops.select_cpu() for them, see select_task_rq().
 */
static s32 scx_select_cpu_dfl(struct task_struct *p, s32 prev_cpu,
			      u64 wake_flags, bool *found)
{
	const struct cpumask *llc_cpus = NULL;
	int node = cpu_to_node(prev_cpu);
	s32 cpu;

	*found = false;

	/*
	 * This is necessary to protect llc_cpus.
	 */
	rcu_read_lock();

	/*
	 * Determine the scheduling domain only if the task is allowed to run
	 * on all CPUs.
	 *
	 * This is done primarily for efficiency, as it avoids the overhead of
	 * updating a cpumask every time we need to select an idle CPU (which
	 * can be costly in large SMP systems), but it also aligns logically:
	 * if a task's scheduling domain is restricted by user-space (through
	 * CPU affinity), the task will simply use the flat scheduling domain
	 * defined by user-space.
	 */
	if (p->nr_cpus_allowed >= num_possible_cpus())
		if (static_branch_maybe(CONFIG_SCHED_MC, &scx_selcpu_topo_llc))
			llc_cpus = llc_span(prev_cpu);

	/*
	 * If WAKE_SYNC, try to migrate the wakee to the waker's CPU.
	 */
	if (wake_flags & SCX_WAKE_SYNC) {
		cpu = smp_processor_id();

		/*
		 * If the waker's CPU is cache affine and prev_cpu is idle,
		 * then avoid a migration.
		 */
		if (cpus_share_cache(cpu, prev_cpu) &&
		    test_and_clear_cpu_idle(prev_cpu)) {
			cpu = prev_cpu;
			goto cpu_found;
		}

		/*
		 * If the waker's local DSQ is empty, and the system is under
		 * utilized, try to wake up @p to the local DSQ of the waker.
		 *
		 * Checking only for an empty local DSQ is insufficient as it
		 * could give the wakee an unfair advantage when the system is
		 * oversaturated.
		 *
		 * Checking only for the presence of idle CPUs is also
		 * insufficient as the local DSQ of the waker could have tasks
		 * piled up on it even if there is an idle core elsewhere on
		 * the system.
		 */
		if (!(current->flags & PF_EXITING) &&
		    cpu_rq(cpu)->scx.local_dsq.nr == 0 &&
		    !cpumask_empty(get_idle_cpumask_node(cpu_to_node(cpu)))) {
			if (cpumask_test_cpu(cpu, p->cpus_ptr))
				goto cpu_found;
		}
	}

	/*
	 * If CPU has SMT, any wholly idle CPU is likely a better pick than
	 * partially idle @prev_cpu.
	 */
	if (sched_smt_active()) {
		/*
		 * Keep using @prev_cpu if it's part of a fully idle core.
		 */
		if (cpumask_test_cpu(prev_cpu, get_idle_smtmask_node(node)) &&
		    test_and_clear_cpu_idle(prev_cpu)) {
			cpu = prev_cpu;
			goto cpu_found;
		}

		/*
		 * Search for any fully idle core in the same LLC domain.
		 */
		if (llc_cpus) {
			cpu = scx_pick_idle_cpu_from_node(node, llc_cpus, SCX_PICK_IDLE_CORE);
			if (cpu >= 0)
				goto cpu_found;
		}

		/*
		 * Search for any full idle core usable by the task.
		 */
		cpu = scx_pick_idle_cpu(p->cpus_ptr, prev_cpu, SCX_PICK_IDLE_CORE);
		if (cpu >= 0)
			goto cpu_found;
	}

	/*
	 * Use @prev_cpu if it's idle.
	 */
	if (test_and_clear_cpu_idle(prev_cpu)) {
		cpu = prev_cpu;
		goto cpu_found;
	}

	/*
	 * Search for any idle CPU in the same LLC domain.
	 */
	if (llc_cpus) {
		cpu = scx_pick_idle_cpu_from_node(node, llc_cpus, 0);
		if (cpu >= 0)
			goto cpu_found;
	}

	/*
	 * Search for any idle CPU usable by the task.
	 */
	cpu = scx_pick_idle_cpu(p->cpus_ptr, prev_cpu, 0);
	if (cpu >= 0)
		goto cpu_found;

	rcu_read_unlock();
	return prev_cpu;

cpu_found:
	rcu_read_unlock();

	*found = true;
	return cpu;
}

static void reset_idle_masks(void)
{
	int node;

	if (!static_branch_maybe(CONFIG_NUMA, &scx_builtin_idle_per_node)) {
		cpumask_copy(get_idle_cpumask_node(0), cpu_online_mask);
		cpumask_copy(get_idle_smtmask_node(0), cpu_online_mask);
		return;
	}

	/*
	 * Consider all online cpus idle. Should converge to the actual state
	 * quickly.
	 */
	for_each_node_state(node, N_POSSIBLE) {
		const struct cpumask *node_mask = cpumask_of_node(node);
		struct cpumask *idle_cpu = get_idle_cpumask_node(node);
		struct cpumask *idle_smt = get_idle_smtmask_node(node);

		cpumask_and(idle_cpu, cpu_online_mask, node_mask);
		cpumask_copy(idle_smt, idle_cpu);
	}
}

void __scx_update_idle(struct rq *rq, bool idle)
{
	int cpu = cpu_of(rq);
	int node = cpu_to_node(cpu);
	struct cpumask *idle_cpu = get_idle_cpumask_node(node);

	if (SCX_HAS_OP(update_idle) && !scx_rq_bypassing(rq)) {
		SCX_CALL_OP(SCX_KF_REST, update_idle, cpu_of(rq), idle);
		if (!static_branch_unlikely(&scx_builtin_idle_enabled))
			return;
	}

	assign_cpu(cpu, idle_cpu, idle);

#ifdef CONFIG_SCHED_SMT
	if (sched_smt_active()) {
		const struct cpumask *smt = cpu_smt_mask(cpu);
		struct cpumask *idle_smt = get_idle_smtmask_node(node);

		if (idle) {
			/*
			 * idle_smt handling is racy but that's fine as it's
			 * only for optimization and self-correcting.
			 */
			for_each_cpu(cpu, smt) {
				if (!cpumask_test_cpu(cpu, idle_cpu))
					return;
			}
			cpumask_or(idle_smt, idle_smt, smt);
		} else {
			cpumask_andnot(idle_smt, idle_smt, smt);
		}
	}
#endif	/* CONFIG_SCHED_SMT */
}
#else	/* !CONFIG_SMP */
static struct cpumask *get_curr_idle_cpumask(void)
{
	return cpu_none_mask;
}

static struct cpumask *get_curr_idle_smtmask(void)
{
	return cpu_none_mask;
}

static bool test_and_clear_cpu_idle(int cpu) { return false; }

static s32 scx_pick_idle_cpu(const struct cpumask *cpus_allowed, s32 prev_cpu, u64 flags)
{
	return -EBUSY;
}

static void reset_idle_masks(void) {}
#endif	/* CONFIG_SMP */

/********************************************************************************
 * Helpers that can be called from the BPF scheduler.
 */
__bpf_kfunc_start_defs();

/**
 * scx_bpf_select_cpu_dfl - The default implementation of ops.select_cpu()
 * @p: task_struct to select a CPU for
 * @prev_cpu: CPU @p was on previously
 * @wake_flags: %SCX_WAKE_* flags
 * @is_idle: out parameter indicating whether the returned CPU is idle
 *
 * Can only be called from ops.select_cpu() if the built-in CPU selection is
 * enabled - ops.update_idle() is missing or %SCX_OPS_KEEP_BUILTIN_IDLE is set.
 * @p, @prev_cpu and @wake_flags match ops.select_cpu().
 *
 * Returns the picked CPU with *@is_idle indicating whether the picked CPU is
 * currently idle and thus a good candidate for direct dispatching.
 */
__bpf_kfunc s32 scx_bpf_select_cpu_dfl(struct task_struct *p, s32 prev_cpu,
				       u64 wake_flags, bool *is_idle)
{
	if (!static_branch_likely(&scx_builtin_idle_enabled)) {
		scx_ops_error("built-in idle tracking is disabled");
		goto prev_cpu;
	}

	if (!scx_kf_allowed(SCX_KF_SELECT_CPU))
		goto prev_cpu;

#ifdef CONFIG_SMP
	return scx_select_cpu_dfl(p, prev_cpu, wake_flags, is_idle);
#endif

prev_cpu:
	*is_idle = false;
	return prev_cpu;
}

/**
 * scx_bpf_get_idle_cpumask_node - Get a referenced kptr to the idle-tracking
 * per-CPU cpumask of a target NUMA node.
 *
 * Returns an empty cpumask if idle tracking is not enabled, if @node is not
 * valid, or running on a UP kernel.
 */
__bpf_kfunc const struct cpumask *scx_bpf_get_idle_cpumask_node(int node)
{
	if (!static_branch_likely(&scx_builtin_idle_enabled)) {
		scx_ops_error("built-in idle tracking is disabled");
		return cpu_none_mask;
	}
	if (!static_branch_likely(&scx_builtin_idle_per_node)) {
		scx_ops_error("per-node idle tracking is disabled");
		return cpu_none_mask;
	}

	return get_idle_cpumask_node(node) ? : cpu_none_mask;
}
/**
 * scx_bpf_get_idle_cpumask - Get a referenced kptr to the idle-tracking
 * per-CPU cpumask of the current NUMA node.
 *
 * Returns an emtpy cpumask if idle tracking is not enabled, or running on a UP
 * kernel.
 */
__bpf_kfunc const struct cpumask *scx_bpf_get_idle_cpumask(void)
{
	if (!static_branch_likely(&scx_builtin_idle_enabled)) {
		scx_ops_error("built-in idle tracking is disabled");
		return cpu_none_mask;
	}

	return get_curr_idle_cpumask();
}

/**
 * scx_bpf_get_idle_smtmask_node - Get a referenced kptr to the idle-tracking,
 * per-physical-core cpumask of a target NUMA node. Can be used to determine
 * if an entire physical core is free.
 *
 * Returns an empty cpumask if idle tracking is not enabled, if @node is not
 * valid, or running on a UP kernel.
 */
__bpf_kfunc const struct cpumask *scx_bpf_get_idle_smtmask_node(int node)
{
	if (!static_branch_likely(&scx_builtin_idle_enabled)) {
		scx_ops_error("built-in idle tracking is disabled");
		return cpu_none_mask;
	}
	if (!static_branch_likely(&scx_builtin_idle_per_node)) {
		scx_ops_error("per-node idle tracking is disabled");
		return cpu_none_mask;
	}

	return get_idle_smtmask_node(node) ? : cpu_none_mask;
}

/**
 * scx_bpf_get_idle_smtmask - Get a referenced kptr to the idle-tracking,
 * per-physical-core cpumask of the current NUMA node. Can be used to determine
 * if an entire physical core is free.
 *
 * Returns an empty cumask if idle tracking is not enabled, or running on a UP
 * kernel.
 */
__bpf_kfunc const struct cpumask *scx_bpf_get_idle_smtmask(void)
{
	if (!static_branch_likely(&scx_builtin_idle_enabled)) {
		scx_ops_error("built-in idle tracking is disabled");
		return cpu_none_mask;
	}

	return get_curr_idle_smtmask();
}

/**
 * scx_bpf_put_idle_cpumask - Release a previously acquired referenced kptr to
 * either the percpu, or SMT idle-tracking cpumask.
 */
__bpf_kfunc void scx_bpf_put_idle_cpumask(const struct cpumask *idle_mask)
{
	/*
	 * Empty function body because we aren't actually acquiring or releasing
	 * a reference to a global idle cpumask, which is read-only in the
	 * caller and is never released. The acquire / release semantics here
	 * are just used to make the cpumask a trusted pointer in the caller.
	 */
}

/**
 * scx_bpf_test_and_clear_cpu_idle - Test and clear @cpu's idle state
 * @cpu: cpu to test and clear idle for
 *
 * Returns %true if @cpu was idle and its idle state was successfully cleared.
 * %false otherwise.
 *
 * Unavailable if ops.update_idle() is implemented and
 * %SCX_OPS_KEEP_BUILTIN_IDLE is not set.
 */
__bpf_kfunc bool scx_bpf_test_and_clear_cpu_idle(s32 cpu)
{
	if (!static_branch_likely(&scx_builtin_idle_enabled)) {
		scx_ops_error("built-in idle tracking is disabled");
		return false;
	}

	if (ops_cpu_valid(cpu, NULL))
		return test_and_clear_cpu_idle(cpu);
	else
		return false;
}

/**
 * scx_bpf_pick_idle_cpu_node - Pick and claim an idle cpu from a NUMA node
 * @node: target NUMA node
 * @cpus_allowed: Allowed cpumask
 * @flags: %SCX_PICK_IDLE_CPU_* flags
 *
 * Pick and claim an idle cpu in @cpus_allowed from the NUMA node @node.
 * Returns the picked idle cpu number on success. -%EBUSY if no matching cpu
 * was found.
 *
 * Unavailable if ops.update_idle() is implemented and
 * %SCX_OPS_KEEP_BUILTIN_IDLE is not set or if %SCX_OPS_KEEP_BUILTIN_IDLE is
 * not set.
 */
__bpf_kfunc s32 scx_bpf_pick_idle_cpu_node(int node, const struct cpumask *cpus_allowed,
				      u64 flags)
{
	if (!static_branch_likely(&scx_builtin_idle_enabled)) {
		scx_ops_error("built-in idle tracking is disabled");
		return -EBUSY;
	}
	if (!static_branch_likely(&scx_builtin_idle_per_node)) {
		scx_ops_error("per-node idle tracking is disabled");
		return -EBUSY;
	}

	return scx_pick_idle_cpu_from_node(node, cpus_allowed, flags);
}

/**
 * scx_bpf_pick_idle_cpu - Pick and claim an idle cpu
 * @cpus_allowed: Allowed cpumask
 * @flags: %SCX_PICK_IDLE_CPU_* flags
 *
 * Pick and claim an idle cpu in @cpus_allowed. Returns the picked idle cpu
 * number on success. -%EBUSY if no matching cpu was found.
 *
 * Idle CPU tracking may race against CPU scheduling state transitions. For
 * example, this function may return -%EBUSY as CPUs are transitioning into the
 * idle state. If the caller then assumes that there will be dispatch events on
 * the CPUs as they were all busy, the scheduler may end up stalling with CPUs
 * idling while there are pending tasks. Use scx_bpf_pick_any_cpu() and
 * scx_bpf_kick_cpu() to guarantee that there will be at least one dispatch
 * event in the near future.
 *
 * Unavailable if ops.update_idle() is implemented and
 * %SCX_OPS_KEEP_BUILTIN_IDLE is not set.
 */
__bpf_kfunc s32 scx_bpf_pick_idle_cpu(const struct cpumask *cpus_allowed,
				      u64 flags)
{
	if (!static_branch_likely(&scx_builtin_idle_enabled)) {
		scx_ops_error("built-in idle tracking is disabled");
		return -EBUSY;
	}

	return scx_pick_idle_cpu(cpus_allowed, smp_processor_id(), flags);
}

/**
 * scx_bpf_pick_any_cpu - Pick and claim an idle cpu if available or pick any CPU
 * @cpus_allowed: Allowed cpumask
 * @flags: %SCX_PICK_IDLE_CPU_* flags
 *
 * Pick and claim an idle cpu in @cpus_allowed. If none is available, pick any
 * CPU in @cpus_allowed. Guaranteed to succeed and returns the picked idle cpu
 * number if @cpus_allowed is not empty. -%EBUSY is returned if @cpus_allowed is
 * empty.
 *
 * If ops.update_idle() is implemented and %SCX_OPS_KEEP_BUILTIN_IDLE is not
 * set, this function can't tell which CPUs are idle and will always pick any
 * CPU.
 */
__bpf_kfunc s32 scx_bpf_pick_any_cpu(const struct cpumask *cpus_allowed,
				     u64 flags)
{
	s32 cpu;

	if (static_branch_likely(&scx_builtin_idle_enabled)) {
		cpu = scx_pick_idle_cpu(cpus_allowed, smp_processor_id(), flags);
		if (cpu >= 0)
			return cpu;
	}

	cpu = cpumask_any_distribute(cpus_allowed);
	if (cpu < nr_cpu_ids)
		return cpu;
	else
		return -EBUSY;
}

__bpf_kfunc_end_defs();

BTF_KFUNCS_START(scx_kfunc_ids_select_cpu)
BTF_ID_FLAGS(func, scx_bpf_select_cpu_dfl, KF_RCU)
BTF_ID_FLAGS(func, scx_bpf_get_idle_cpumask, KF_ACQUIRE)
BTF_ID_FLAGS(func, scx_bpf_get_idle_cpumask_node, KF_ACQUIRE)
BTF_ID_FLAGS(func, scx_bpf_get_idle_smtmask, KF_ACQUIRE)
BTF_ID_FLAGS(func, scx_bpf_get_idle_smtmask_node, KF_ACQUIRE)
BTF_ID_FLAGS(func, scx_bpf_put_idle_cpumask, KF_RELEASE)
BTF_ID_FLAGS(func, scx_bpf_test_and_clear_cpu_idle)
BTF_ID_FLAGS(func, scx_bpf_pick_idle_cpu, KF_RCU)
BTF_ID_FLAGS(func, scx_bpf_pick_idle_cpu_node, KF_RCU)
BTF_ID_FLAGS(func, scx_bpf_pick_any_cpu, KF_RCU)
BTF_KFUNCS_END(scx_kfunc_ids_select_cpu)

static const struct btf_kfunc_id_set scx_kfunc_set_select_cpu = {
	.owner			= THIS_MODULE,
	.set			= &scx_kfunc_ids_select_cpu,
};
