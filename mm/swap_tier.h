/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SWAP_TIER_H
#define _SWAP_TIER_H

#include <linux/types.h>
#include <linux/spinlock.h>

/* Forward declarations */
struct swap_info_struct;

extern spinlock_t swap_tier_lock;

#define TIER_ALL_MASK		(~0)
#define TIER_DEFAULT_IDX	(31)
#define TIER_DEFAULT_MASK	(1U << TIER_DEFAULT_IDX)

/* Initialization and application */
void swap_tiers_init(void);
ssize_t swap_tiers_sysfs_show(char *buf);

int swap_tiers_add(const char *name, int prio);
int swap_tiers_remove(const char *name);

void swap_tiers_snapshot(void);
void swap_tiers_snapshot_restore(void);
bool swap_tiers_update(void);

/* Tier assignment */
void swap_tiers_assign_dev(struct swap_info_struct *swp);

#endif /* _SWAP_TIER_H */
