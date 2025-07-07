// SPDX-License-Identifier: GPL-2.0-only
/*
 *  kernel/sched/cpupri.c
 *
 *  CPU priority management
 *
 *  Copyright (C) 2007-2008 Novell
 *
 *  Author: Gregory Haskins <ghaskins@novell.com>
 *
 *  This code tracks the priority of each CPU so that global migration
 *  decisions are easy to calculate.  Each CPU can be in a state as follows:
 *
 *                 (INVALID), NORMAL, RT1, ... RT99, HIGHER
 *
 *  going from the lowest priority to the highest.  CPUs in the INVALID state
 *  are not eligible for routing.  The system maintains this state with
 *  a 2 dimensional bitmap (the first for priority class, the second for CPUs
 *  in that class).  Therefore a typical application without affinity
 *  restrictions can find a suitable CPU with O(1) complexity (e.g. two bit
 *  searches).  For tasks with affinity restrictions, the algorithm has a
 *  worst case complexity of O(min(101, nr_domcpus)), though the scenario that
 *  yields the worst case search is fairly contrived.
 */

/*
 * p->rt_priority   p->prio   newpri   cpupri
 *
 *				  -1       -1 (CPUPRI_INVALID)
 *
 *				  99        0 (CPUPRI_NORMAL)
 *
 *		1        98       98        1
 *	      ...
 *	       49        50       50       49
 *	       50        49       49       50
 *	      ...
 *	       99         0        0       99
 *
 *				 100	  100 (CPUPRI_HIGHER)
 */
static int convert_prio(int prio)
{
	int cpupri;

	switch (prio) {
	case CPUPRI_INVALID:
		cpupri = CPUPRI_INVALID;	/* -1 */
		break;

	case 0 ... 98:
		cpupri = MAX_RT_PRIO-1 - prio;	/* 1 ... 99 */
		break;

	case MAX_RT_PRIO-1:
		cpupri = CPUPRI_NORMAL;		/*  0 */
		break;

	case MAX_RT_PRIO:
		cpupri = CPUPRI_HIGHER;		/* 100 */
		break;
	}

	return cpupri;
}

#ifdef	CONFIG_CPUMASK_OFFSTACK
static inline int alloc_vec_masks(struct cpupri_vec *vec)
{
	int i;

	for (i = 0; i < nr_node_ids; i++) {
		if (!zalloc_cpumask_var_node(&vec->masks[i], GFP_KERNEL, i))
			goto cleanup;

		// Clear masks of cur node, set others
		bitmap_complement(cpumask_bits(vec->masks[i]),
			cpumask_bits(cpumask_of_node(i)), small_cpumask_bits);
	}
	return 0;

cleanup:
	while (i--)
		free_cpumask_var(vec->masks[i]);
	return -ENOMEM;
}

static inline void free_vec_masks(struct cpupri_vec *vec)
{
	for (int i = 0; i < nr_node_ids; i++)
		free_cpumask_var(vec->masks[i]);
}

static inline int setup_vec_mask_var_ts(struct cpupri *cp)
{
	int i;

	for (i = 0; i < CPUPRI_NR_PRIORITIES; i++) {
		struct cpupri_vec *vec = &cp->pri_to_cpu[i];

		vec->masks = kcalloc(nr_node_ids, sizeof(cpumask_var_t), GFP_KERNEL);
		if (!vec->masks)
			goto cleanup;
	}
	return 0;

cleanup:
	/* Free any already allocated masks */
	while (i--) {
		kfree(cp->pri_to_cpu[i].masks);
		cp->pri_to_cpu[i].masks = NULL;
	}

	return -ENOMEM;
}

static inline void free_vec_mask_var_ts(struct cpupri *cp)
{
	for (int i = 0; i < CPUPRI_NR_PRIORITIES; i++) {
		kfree(cp->pri_to_cpu[i].masks);
		cp->pri_to_cpu[i].masks = NULL;
	}
}

static inline int
available_cpu_in_nodes(struct task_struct *p, struct cpupri_vec *vec)
{
	int cur_node = numa_node_id();

	for (int i = 0; i < nr_node_ids; i++) {
		int nid = (cur_node + i) % nr_node_ids;

		if (cpumask_first_and_and(&p->cpus_mask, vec->masks[nid],
					cpumask_of_node(nid)) < nr_cpu_ids)
			return 1;
	}

	return 0;
}

#define available_cpu_in_vec available_cpu_in_nodes

#else /* !CONFIG_CPUMASK_OFFSTACK */

static inline int alloc_vec_masks(struct cpupri_vec *vec)
{
	if (!zalloc_cpumask_var(&vec->mask, GFP_KERNEL))
		return -ENOMEM;

	return 0;
}

static inline void free_vec_masks(struct cpupri_vec *vec)
{
	free_cpumask_var(vec->mask);
}

static inline int setup_vec_mask_var_ts(struct cpupri *cp)
{
	return 0;
}

static inline void free_vec_mask_var_ts(struct cpupri *cp)
{
}

static inline int
available_cpu_in_vec(struct task_struct *p, struct cpupri_vec *vec)
{
	if (cpumask_any_and(&p->cpus_mask, vec->mask) >= nr_cpu_ids)
		return 0;

	return 1;
}
#endif

static inline int alloc_all_masks(struct cpupri *cp)
{
	int i;

	for (i = 0; i < CPUPRI_NR_PRIORITIES; i++) {
		if (alloc_vec_masks(&cp->pri_to_cpu[i]))
			goto cleanup;
	}

	return 0;

cleanup:
	while (i--)
		free_vec_masks(&cp->pri_to_cpu[i]);

	return -ENOMEM;
}

static inline void setup_vec_counts(struct cpupri *cp)
{
	for (int i = 0; i < CPUPRI_NR_PRIORITIES; i++) {
		struct cpupri_vec *vec = &cp->pri_to_cpu[i];

		atomic_set(&vec->count, 0);
	}
}

static inline int __cpupri_find(struct cpupri *cp, struct task_struct *p,
				struct cpumask *lowest_mask, int idx)
{
	struct cpupri_vec *vec  = &cp->pri_to_cpu[idx];
	int skip = 0;

	if (!atomic_read(&(vec)->count))
		skip = 1;
	/*
	 * When looking at the vector, we need to read the counter,
	 * do a memory barrier, then read the mask.
	 *
	 * Note: This is still all racy, but we can deal with it.
	 *  Ideally, we only want to look at masks that are set.
	 *
	 *  If a mask is not set, then the only thing wrong is that we
	 *  did a little more work than necessary.
	 *
	 *  If we read a zero count but the mask is set, because of the
	 *  memory barriers, that can only happen when the highest prio
	 *  task for a run queue has left the run queue, in which case,
	 *  it will be followed by a pull. If the task we are processing
	 *  fails to find a proper place to go, that pull request will
	 *  pull this task if the run queue is running at a lower
	 *  priority.
	 */
	smp_rmb();

	/* Need to do the rmb for every iteration */
	if (skip)
		return 0;

	if (!available_cpu_in_vec(p, vec))
		return 0;

#ifdef	CONFIG_CPUMASK_OFFSTACK
	struct cpumask *cpupri_mask = lowest_mask;

	// available && lowest_mask
	if (lowest_mask) {
		cpumask_copy(cpupri_mask, vec->masks[0]);
		for (int nid = 1; nid < nr_node_ids; nid++)
			cpumask_and(cpupri_mask, cpupri_mask, vec->masks[nid]);
	}
#else
	struct cpumask *cpupri_mask = vec->mask;
#endif

	if (lowest_mask) {
		cpumask_and(lowest_mask, &p->cpus_mask, cpupri_mask);
		cpumask_and(lowest_mask, lowest_mask, cpu_active_mask);

		/*
		 * We have to ensure that we have at least one bit
		 * still set in the array, since the map could have
		 * been concurrently emptied between the first and
		 * second reads of vec->mask.  If we hit this
		 * condition, simply act as though we never hit this
		 * priority level and continue on.
		 */
		if (cpumask_empty(lowest_mask))
			return 0;
	}

	return 1;
}

int cpupri_find(struct cpupri *cp, struct task_struct *p,
		struct cpumask *lowest_mask)
{
	return cpupri_find_fitness(cp, p, lowest_mask, NULL);
}

/**
 * cpupri_find_fitness - find the best (lowest-pri) CPU in the system
 * @cp: The cpupri context
 * @p: The task
 * @lowest_mask: A mask to fill in with selected CPUs (or NULL)
 * @fitness_fn: A pointer to a function to do custom checks whether the CPU
 *              fits a specific criteria so that we only return those CPUs.
 *
 * Note: This function returns the recommended CPUs as calculated during the
 * current invocation.  By the time the call returns, the CPUs may have in
 * fact changed priorities any number of times.  While not ideal, it is not
 * an issue of correctness since the normal rebalancer logic will correct
 * any discrepancies created by racing against the uncertainty of the current
 * priority configuration.
 *
 * Return: (int)bool - CPUs were found
 */
int cpupri_find_fitness(struct cpupri *cp, struct task_struct *p,
		struct cpumask *lowest_mask,
		bool (*fitness_fn)(struct task_struct *p, int cpu))
{
	int task_pri = convert_prio(p->prio);
	int idx, cpu;

	WARN_ON_ONCE(task_pri >= CPUPRI_NR_PRIORITIES);

	for (idx = 0; idx < task_pri; idx++) {

		if (!__cpupri_find(cp, p, lowest_mask, idx))
			continue;

		if (!lowest_mask || !fitness_fn)
			return 1;

		/* Ensure the capacity of the CPUs fit the task */
		for_each_cpu(cpu, lowest_mask) {
			if (!fitness_fn(p, cpu))
				cpumask_clear_cpu(cpu, lowest_mask);
		}

		/*
		 * If no CPU at the current priority can fit the task
		 * continue looking
		 */
		if (cpumask_empty(lowest_mask))
			continue;

		return 1;
	}

	/*
	 * If we failed to find a fitting lowest_mask, kick off a new search
	 * but without taking into account any fitness criteria this time.
	 *
	 * This rule favours honouring priority over fitting the task in the
	 * correct CPU (Capacity Awareness being the only user now).
	 * The idea is that if a higher priority task can run, then it should
	 * run even if this ends up being on unfitting CPU.
	 *
	 * The cost of this trade-off is not entirely clear and will probably
	 * be good for some workloads and bad for others.
	 *
	 * The main idea here is that if some CPUs were over-committed, we try
	 * to spread which is what the scheduler traditionally did. Sys admins
	 * must do proper RT planning to avoid overloading the system if they
	 * really care.
	 */
	if (fitness_fn)
		return cpupri_find(cp, p, lowest_mask);

	return 0;
}

/**
 * cpupri_set - update the CPU priority setting
 * @cp: The cpupri context
 * @cpu: The target CPU
 * @newpri: The priority (INVALID,NORMAL,RT1-RT99,HIGHER) to assign to this CPU
 *
 * Note: Assumes cpu_rq(cpu)->lock is locked
 *
 * Returns: (void)
 */
void cpupri_set(struct cpupri *cp, int cpu, int newpri)
{
	int *currpri = &cp->cpu_to_pri[cpu];
	int oldpri = *currpri;
	int do_mb = 0;

	newpri = convert_prio(newpri);

	BUG_ON(newpri >= CPUPRI_NR_PRIORITIES);

	if (newpri == oldpri)
		return;

	/*
	 * If the CPU was currently mapped to a different value, we
	 * need to map it to the new value then remove the old value.
	 * Note, we must add the new value first, otherwise we risk the
	 * cpu being missed by the priority loop in cpupri_find.
	 */
	if (likely(newpri != CPUPRI_INVALID)) {
		struct cpupri_vec *vec = &cp->pri_to_cpu[newpri];

#ifdef	CONFIG_CPUMASK_OFFSTACK
		cpumask_set_cpu(cpu, vec->masks[cpu_to_node(cpu)]);
#else
		cpumask_set_cpu(cpu, vec->mask);
#endif
		/*
		 * When adding a new vector, we update the mask first,
		 * do a write memory barrier, and then update the count, to
		 * make sure the vector is visible when count is set.
		 */
		smp_mb__before_atomic();
		atomic_inc(&(vec)->count);
		do_mb = 1;
	}
	if (likely(oldpri != CPUPRI_INVALID)) {
		struct cpupri_vec *vec  = &cp->pri_to_cpu[oldpri];

		/*
		 * Because the order of modification of the vec->count
		 * is important, we must make sure that the update
		 * of the new prio is seen before we decrement the
		 * old prio. This makes sure that the loop sees
		 * one or the other when we raise the priority of
		 * the run queue. We don't care about when we lower the
		 * priority, as that will trigger an rt pull anyway.
		 *
		 * We only need to do a memory barrier if we updated
		 * the new priority vec.
		 */
		if (do_mb)
			smp_mb__after_atomic();

		/*
		 * When removing from the vector, we decrement the counter first
		 * do a memory barrier and then clear the mask.
		 */
		atomic_dec(&(vec)->count);
		smp_mb__after_atomic();
#ifdef	CONFIG_CPUMASK_OFFSTACK
		cpumask_clear_cpu(cpu, vec->masks[cpu_to_node(cpu)]);
#else
		cpumask_clear_cpu(cpu, vec->mask);
#endif
	}

	*currpri = newpri;
}

/**
 * cpupri_init - initialize the cpupri structure
 * @cp: The cpupri context
 *
 * Return: -ENOMEM on memory allocation failure.
 */
int cpupri_init(struct cpupri *cp)
{
	int i;

	/* Allocate the cpu_to_pri array */
	cp->cpu_to_pri = kcalloc(nr_cpu_ids, sizeof(int), GFP_KERNEL);
	if (!cp->cpu_to_pri)
		return -ENOMEM;

	/* Initialize all CPUs to invalid priority */
	for_each_possible_cpu(i)
		cp->cpu_to_pri[i] = CPUPRI_INVALID;

	/* Setup priority vectors */
	setup_vec_counts(cp);
	if (setup_vec_mask_var_ts(cp))
		goto fail_setup_vectors;

	/* Allocate masks for each priority vector */
	if (alloc_all_masks(cp))
		goto fail_alloc_masks;

	return 0;

fail_alloc_masks:
	free_vec_mask_var_ts(cp);

fail_setup_vectors:
	kfree(cp->cpu_to_pri);
	return -ENOMEM;
}

/**
 * cpupri_cleanup - clean up the cpupri structure
 * @cp: The cpupri context
 */
void cpupri_cleanup(struct cpupri *cp)
{
	kfree(cp->cpu_to_pri);

	for (int i = 0; i < CPUPRI_NR_PRIORITIES; i++)
		free_vec_masks(&cp->pri_to_cpu[i]);

	free_vec_mask_var_ts(cp);
}
