/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SWAP_TIER_H
#define _SWAP_TIER_H

#include <linux/types.h>
#include <linux/spinlock.h>

extern spinlock_t swap_tier_lock;

/* Initialization and application */
void swap_tiers_init(void);
ssize_t swap_tiers_sysfs_show(char *buf);

int swap_tiers_add(const char *name, int prio);
int swap_tiers_remove(const char *name);

void swap_tiers_snapshot(void);
void swap_tiers_snapshot_restore(void);
bool swap_tiers_validate(void);
#endif /* _SWAP_TIER_H */
