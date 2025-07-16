/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SWAP_CGROUP_PRIORITY_H
#define _SWAP_CGROUP_PRIORITY_H
#include <linux/swap.h>

#ifdef CONFIG_SWAP_CGROUP_PRIORITY
#include <linux/limits.h>
/*
 * A priority of -1 is not assigned to global swap entries,
 * based on the kernel's specific negative priority assignment rules.
 */
#define SWAP_PRIORITY_DISABLE	-1
/*
 * (SHRT_MAX + 1) exceeds the maximum 'prio' value for signed short.
 * This marks it as an invalid or special priority state, not for standard use.
 */
#define SWAP_PRIORITY_GLOBAL	SHRT_MAX+1
/*
 * ID 0 is reserved/unused in kernel swap management, allowing its use
 * for special internal states or flags, as swap IDs typically start from 1.
 */
#define DEFAULT_ID		0

/* linux/mm/swapfile.c */
extern spinlock_t swap_lock;
extern int least_priority;
extern unsigned int nr_swapfiles;
extern spinlock_t swap_avail_lock;
extern struct swap_info_struct *swap_info[MAX_SWAPFILES];
int swap_node(struct swap_info_struct *si);
unsigned long cluster_alloc_swap_entry(struct swap_info_struct *si, int order,
				       unsigned char usage);
bool get_swap_device_info(struct swap_info_struct *si);

/* linux/mm/swap_cgroup_priority.c */
int apply_swap_cgroup_priority(struct mem_cgroup *memcg, u64 id, int prio);
void activate_swap_cgroup_priority(struct swap_info_struct *swp, bool swapon);
void deactivate_swap_cgroup_priority(struct swap_info_struct *swp,
				     bool swapoff);
int prepare_swap_cgroup_priority(int type);
void show_swap_cgroup_priority(struct seq_file *m);
void show_swap_cgroup_priority_effective(struct seq_file *m);
void get_swapdev_id(struct swap_info_struct *si);
void purge_swap_cgroup_priority(void);
struct swap_cgroup_priority *inherit_swap_cgroup_priority(
	struct mem_cgroup *parent);
bool swap_alloc_cgroup_priority(struct mem_cgroup *memcg, swp_entry_t *entry,
				int order);
void delete_swap_cgroup_priority(struct mem_cgroup *memcg);
#else
int swap_node(struct swap_info_struct *si);
unsigned long cluster_alloc_swap_entry(struct swap_info_struct *si, int order,
				       unsigned char usage);
bool get_swap_device_info(struct swap_info_struct *si);

static inline int apply_swap_cgroup_priority(struct mem_cgroup *memcg, int id,
					     int prio)
{
	return 0;
}
static inline void activate_swap_cgroup_priority(struct swap_info_struct *swp,
						 bool swapon)
{
}
static inline void deactivate_swap_cgroup_priority(struct swap_info_struct *swp, 
						   bool swapoff)
{
}
static inline int prepare_swap_cgroup_priority(int type)
{
	return 0;
}

static inline void get_swapdev_id(struct swap_info_struct *si)
{
}
static inline void purge_swap_cgroup_priority(void)
{
}
static inline bool swap_alloc_cgroup_priority(struct mem_cgroup *memcg,
					      swp_entry_t *entry, int order)
{
	return false;
}
static inline void delete_swap_cgroup_priority(struct mem_cgroup *memcg)
{
}
#endif
#endif
