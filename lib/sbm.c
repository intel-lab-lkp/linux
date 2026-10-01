/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/sbm.h>
#include <linux/init.h>
#include <linux/log2.h>
#include <linux/slab.h>
#include <linux/cache.h>
#include <linux/printk.h>
#include <linux/cpumask.h>
#include <linux/jump_label.h>

#include <asm/runtime-const.h>

static int sbm_max_threads_per_instance __ro_after_init = -1;
static int sbm_num_instance __ro_after_init = -1;

int __sbm_shift __ro_after_init;
int __sbm_mask __ro_after_init;

static struct {
	int		instance_id;	/* Instance ID linked to the leaf. */
	unsigned long	allocated_mask;	/* Set of IDs that have been allocated. */
} *__sbm_idx_metadata __ro_after_init;

/* Translations between cpu <-> sbm leaf */
static int *__sbm_cpu_to_idx __ro_after_init;
static int *__sbm_idx_to_cpu __ro_after_init;

static __always_inline int *_sbm_cpu_to_idx(void)
{
	return runtime_const_ptr(__sbm_cpu_to_idx);
}

static __always_inline int *_sbm_idx_to_cpu(void)
{
	return runtime_const_ptr(__sbm_idx_to_cpu);
}

int sbm_cpu_to_idx(int cpu)
{
	return _sbm_cpu_to_idx()[cpu];
}

int sbm_idx_to_cpu(int idx)
{
	return _sbm_idx_to_cpu()[idx];
}

/*
 * Certain architectures may skip initializing sbm propoerties
 * while having an arch_sbm_cpu_instance_id() definition.
 *
 * In such cases, don't trust the arch/ side redefine and use
 * the default single instance mapping.
 */
static DEFINE_STATIC_KEY_FALSE(sbm_arch_initialized);

/*
 * In absence of an arch definition, consider all CPUs to
 * belong to the same sbm instance.
 */
int __weak arch_sbm_cpu_instance_id(int cpu)
{
	return 0;
}

static int sbm_cpu_to_instance(int cpu)
{
	if (static_branch_likely(&sbm_arch_initialized))
		return arch_sbm_cpu_instance_id(cpu);

	return 0;
}

int alloc_sbm_index(int cpu)
{
	int cpu_instance = sbm_cpu_to_instance(cpu);
	int i, idx = BITS_PER_LONG, free_index = -1;

	for (i = 0; i < sbm_num_instance; ++i) {
		if (__sbm_idx_metadata[i].instance_id == cpu_instance) {
			idx = find_first_zero_bit(&__sbm_idx_metadata[i].allocated_mask,
						  BITS_PER_LONG);

			if (idx < BITS_PER_LONG)
				break;
		}
		if (free_index == -1 && __sbm_idx_metadata[i].instance_id == -1)
			free_index = i;
	}

	if (i == sbm_num_instance && free_index == -1)
		return -ENOENT;

	if (i == sbm_num_instance) {
		__sbm_idx_metadata[free_index].instance_id = cpu_instance;
		i = free_index;
		idx = 0;
	}

	WARN_ON_ONCE(idx >= sbm_max_threads_per_instance);

	__set_bit(idx, &__sbm_idx_metadata[i].allocated_mask);

	idx = (i << __sbm_shift) + idx;
	_sbm_idx_to_cpu()[idx] = cpu;
	_sbm_cpu_to_idx()[cpu] = idx;

	return 0;
}

void free_sbm_index(int cpu)
{
	int idx = sbm_cpu_to_idx(cpu);
	u32 leaf;

	if (idx < 0)
		return;

	_sbm_idx_to_cpu()[idx] = -1;
	_sbm_cpu_to_idx()[cpu] = -1;

	leaf = runtime_const_shift_right_32(idx, __sbm_shift);
	idx = runtime_const_mask_32(idx, __sbm_mask);

	__clear_bit(idx, &__sbm_idx_metadata[leaf].allocated_mask);

	if (find_first_bit(&__sbm_idx_metadata[leaf].allocated_mask, BITS_PER_LONG) ==
	    BITS_PER_LONG)
		__sbm_idx_metadata[leaf].instance_id = -1;
}

void __init sbm_set_topology(int num_instances, int max_threads_per_instance)
{
	sbm_max_threads_per_instance = max_threads_per_instance;
	sbm_num_instance = num_instances;

	pr_info("sbm topology set to %d instance(s) with a maximum of %d thread(s) per instance\n",
		sbm_num_instance,
		sbm_max_threads_per_instance);
}

int __init sbm_init(void)
{
	int i;

	if (sbm_max_threads_per_instance > 0 && sbm_num_instance > 0) {
		static_branch_enable(&sbm_arch_initialized);
		goto init_properties;
	}

	sbm_max_threads_per_instance = BITS_PER_LONG;
	sbm_num_instance = (nr_cpumask_bits / BITS_PER_LONG) + 1;

init_properties:
	/*
	 * If the number of CPUs per instance cross bitmask word boundary,
	 * split the instances into samller chunks on BITS_PER_LONG and
	 * increase the order of leaves.
	 */
	if (sbm_max_threads_per_instance > BITS_PER_LONG) {
		int split = (sbm_max_threads_per_instance + BITS_PER_LONG - 1) / BITS_PER_LONG;

		sbm_max_threads_per_instance = BITS_PER_LONG;
		sbm_num_instance *= split;
	}

	sbm_max_threads_per_instance = roundup_pow_of_two(sbm_max_threads_per_instance);

	__sbm_shift = ilog2(sbm_max_threads_per_instance);
	__sbm_mask = sbm_max_threads_per_instance - 1;

	__sbm_idx_metadata = kzalloc_objs(*__sbm_idx_metadata, sbm_num_instance);
	__sbm_cpu_to_idx = kzalloc_objs(*__sbm_cpu_to_idx, nr_cpumask_bits);
	__sbm_idx_to_cpu = kzalloc_objs(*__sbm_idx_to_cpu,
					sbm_max_threads_per_instance * sbm_num_instance);

	BUG_ON(!__sbm_cpu_to_idx || !__sbm_idx_to_cpu || !__sbm_idx_metadata);

	for (i = 0; i < nr_cpumask_bits; ++i)
		__sbm_cpu_to_idx[i] = -1;

	/* Set all instance id to -1 to allow for future allocations to claim them. */
	for (i = 0; i < sbm_num_instance; ++i)
		__sbm_idx_metadata[i].instance_id = -1;

	runtime_const_init(shift, __sbm_shift);
	runtime_const_init(mask,  __sbm_mask);
	runtime_const_init(ptr,   __sbm_cpu_to_idx);
	runtime_const_init(ptr,   __sbm_idx_to_cpu);

	barrier();

	pr_info("sbm instance count: %d (maximum threads per instance: %d)\n",
		sbm_num_instance,
		sbm_max_threads_per_instance);

	return 0;
}
