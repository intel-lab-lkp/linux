/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SBM_H
#define _LINUX_SBM_H

#include <linux/bitmap.h>

/*
 * Masks and shifts for sbm index to translate
 * a sbm leaf to CPU.
 */
extern int __sbm_shift;
extern int __sbm_mask;

struct sbm {
	unsigned long   bitmap;
} ____cacheline_aligned;

int arch_sbm_cpu_instance_id(int cpu);
void sbm_set_topology(int num_instances, int max_threads_per_instance);

int sbm_cpu_to_idx(int cpu);
int sbm_idx_to_cpu(int idx);

struct sbm *sbm_alloc(void);
bool sbm_empty(struct sbm *sbm);
int sbm_find_next_bit(struct sbm *sbm, int start);

#define __sbm_op(sbm, func)				\
({							\
	int idx = sbm_cpu_to_idx(cpu);			\
	int nr = idx >> __sbm_shift;			\
	int bit = idx & __sbm_mask;			\
							\
	func(bit, &sbm[nr].bitmap);			\
})

static inline void sbm_cpu_set(struct sbm *sbm, int cpu)
{
	__sbm_op(sbm, set_bit);
}

static inline void sbm_cpu_clear(struct sbm *sbm, int cpu)
{
	__sbm_op(sbm, clear_bit);
}

static inline void __sbm_cpu_set(struct sbm *sbm, int cpu)
{
	__sbm_op(sbm, __set_bit);
}

static inline void __sbm_cpu_clear(struct sbm *sbm, int cpu)
{
	__sbm_op(sbm, __clear_bit);
}

static inline bool sbm_cpu_test(struct sbm *sbm, int cpu)
{
	return __sbm_op(sbm, test_bit);
}

static __always_inline
unsigned int sbm_find_next_bit_wrap(struct sbm *sbm, int start)
{
	int bit = sbm_find_next_bit(sbm, start);

	if (bit >= 0 || start == 0)
		return bit;

	bit = sbm_find_next_bit(sbm, 0);
	return bit < start ? bit : -1;
}

static __always_inline
unsigned int __sbm_for_each_wrap(struct sbm *sbm, int start, int n)
{
	int bit;

	/* If not wrapped around */
	if (n > start) {
		/* and have a bit, just return it. */
		bit = sbm_find_next_bit(sbm, n);
		if (bit >= 0)
			return bit;

		/* Otherwise, wrap around and ... */
		n = 0;
	}

	/* Search the other part. */
	bit = sbm_find_next_bit(sbm, n);
	return bit < start ? bit : -1;
}

#define sbm_for_each_set_bit(sbm, idx) \
	for (int idx = sbm_find_next_bit(sbm, 0); \
	     idx >= 0; idx = sbm_find_next_bit(sbm, idx+1))

#define sbm_for_each_set_bit_wrap(sbm, idx, start) \
	for (int idx = sbm_find_next_bit_wrap(sbm, start); \
	     idx >= 0; idx = __sbm_for_each_wrap(sbm, start, idx+1))

int alloc_sbm_index(int cpu);
void free_sbm_index(int cpu);
int sbm_init(void);

#endif /* _LINUX_SBM_H */
