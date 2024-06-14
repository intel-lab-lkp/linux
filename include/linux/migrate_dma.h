/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _MIGRATE_DMA_H
#define _MIGRATE_DMA_H
#include <linux/migrate_mode.h>

#define MIGRATOR_NAME_LEN 32
struct migrator {
	char name[MIGRATOR_NAME_LEN];
	void (*migrate_dma)(struct list_head *dst_list, struct list_head *src_list);
	bool (*can_migrate_dma)(struct folio *dst, struct folio *src);
	struct rcu_head srcu_head;
	struct module *owner;
};

extern struct migrator migrator;
extern struct mutex migrator_mut;
extern struct srcu_struct mig_srcu;

#ifdef CONFIG_DMA_MIGRATION
void srcu_mig_cb(struct rcu_head *head);
void dma_update_migrator(struct migrator *mig);
unsigned char *get_active_migrator_name(void);
bool can_dma_migrate(struct folio *dst, struct folio *src);
void start_offloading(struct migrator *migrator);
void stop_offloading(void);
#else
static inline void srcu_mig_cb(struct rcu_head *head) { };
static inline void dma_update_migrator(struct migrator *mig) { };
static inline unsigned char *get_active_migrator_name(void) { return NULL; };
static inline bool can_dma_migrate(struct folio *dst, struct folio *src) {return true; };
static inline void start_offloading(struct migrator *migrator) { };
static inline void stop_offloading(void) { };
#endif /* CONFIG_DMA_MIGRATION */

#endif /* _MIGRATE_DMA_H */
