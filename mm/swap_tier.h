/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SWAP_TIER_H
#define _SWAP_TIER_H

#include "swap.h"
#include <linux/memcontrol.h>

#ifdef CONFIG_SWAP_TIER

enum swap_tiers_index {
	SWAP_TIER_DEFAULT,
#ifdef CONFIG_ZSWAP
	SWAP_TIER_ZSWAP,
#endif
	SWAP_TIER_RESERVED_CNT,
};

#define MAX_TIERNAME			16
#define MAX_SWAPTIER			12

#define TIER_MASK(idx, mask)		((mask) << (idx) * 2)
#define TIER_ON_MASK			(1 << 0)
#define TIER_OFF_MASK			(1 << 1)
#define TIER_FULL_MASK			(TIER_ON_MASK | TIER_OFF_MASK)

#define DEFAULT_ON_MASK			TIER_MASK(SWAP_TIER_DEFAULT, TIER_ON_MASK)
#define DEFAULT_OFF_MASK		TIER_MASK(SWAP_TIER_DEFAULT, TIER_OFF_MASK)
#define DEFAULT_FULL_MASK		(DEFAULT_ON_MASK | DEFAULT_OFF_MASK)

#define DEFAULT_TIER_NAME		""
#define ZSWAP_TIER_NAME			"ZSWAP"

struct tiers_desc {
	char name[MAX_TIERNAME];
	union {
		signed short prio_st;
		signed short ops;
	};
	int tier_idx;
};

void swap_tiers_init(void);
int swap_tiers_add(struct tiers_desc desc[], int nr);
int swap_tiers_remove(struct tiers_desc desc[], int nr);
ssize_t swap_tiers_show_sysfs(char *buf);
void swap_tiers_show_memcg(struct seq_file *m, struct mem_cgroup *memcg);
void swap_tiers_assign(struct swap_info_struct *swp);
void swap_tiers_release(struct swap_info_struct *swp);
int swap_tiers_get_mask(struct tiers_desc *desc, int nr, struct mem_cgroup *memcg);
void swap_tiers_put_mask(struct mem_cgroup *memcg);
static inline bool swap_tiers_test_off(int tier_idx, int mask)
{
	return TIER_MASK(tier_idx, TIER_OFF_MASK) & mask;
}
int swap_tiers_collect_compare_mask(struct mem_cgroup *memcg);
#else
static inline void swap_tiers_init(void)
{
}
static inline void swap_tiers_assign(struct swap_info_struct *swp)
{
}
static inline void swap_tiers_release(struct swap_info_struct *swp)
{
}
static inline bool swap_tiers_test_off(int tier_off_mask, int mask)
{
	return false;
}
static inline int swap_tiers_collect_compare_mask(struct mem_cgroup *memcg);
{
	return 0;
}
#endif
#endif
