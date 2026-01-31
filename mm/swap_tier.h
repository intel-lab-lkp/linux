/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SWAP_TIER_H
#define _SWAP_TIER_H

#include <linux/types.h>
#include <linux/spinlock.h>

#define MAX_TIERNAME		16

/* Ensure MAX_SWAPTIER does not exceed MAX_SWAPFILES */
#if 8 > MAX_SWAPFILES
#define MAX_SWAPTIER		MAX_SWAPFILES
#else
#define MAX_SWAPTIER		8
#endif

extern spinlock_t swap_tier_lock;

struct swap_tier_save_ctx {
	char name[MAX_TIERNAME];
	short prio;
};

#define DEFINE_SWAP_TIER_SAVE_CTX(_name) \
	struct swap_tier_save_ctx _name[MAX_SWAPTIER] = {0}

/* Initialization and application */
void swap_tiers_init(void);
ssize_t swap_tiers_sysfs_show(char *buf);

int swap_tiers_add(const char *name, int prio);
int swap_tiers_remove(const char *name);
int swap_tiers_modify(const char *name, int prio);

void swap_tiers_save(struct swap_tier_save_ctx ctx[]);
void swap_tiers_restore(struct swap_tier_save_ctx ctx[]);
bool swap_tiers_validate(void);
#endif /* _SWAP_TIER_H */
