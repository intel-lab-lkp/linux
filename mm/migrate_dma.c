// SPDX-License-Identifier: GPL-2.0
#include <linux/migrate.h>
#include <linux/migrate_dma.h>
#include <linux/rculist.h>
#include <linux/static_call.h>

atomic_t dispatch_to_dma = ATOMIC_INIT(0);
EXPORT_SYMBOL_GPL(dispatch_to_dma);

DEFINE_MUTEX(migrator_mut);
DEFINE_SRCU(mig_srcu);

struct migrator migrator = {
	.name = "kernel",
	.migrate_dma = folios_copy,
	.can_migrate_dma = can_dma_migrate,
	.srcu_head.func = srcu_mig_cb,
	.owner = NULL,
};

bool can_dma_migrate(struct folio *dst, struct folio *src)
{
	return true;
}
EXPORT_SYMBOL_GPL(can_dma_migrate);

void start_offloading(struct migrator *m)
{
	int offloading = 0;

	pr_info("starting migration offload by %s\n", m->name);
	dma_update_migrator(m);
	atomic_try_cmpxchg(&dispatch_to_dma, &offloading, 1);
}
EXPORT_SYMBOL_GPL(start_offloading);

void stop_offloading(void)
{
	int offloading = 1;

	pr_info("stopping migration offload by %s\n", migrator.name);
	dma_update_migrator(NULL);
	atomic_try_cmpxchg(&dispatch_to_dma, &offloading, 0);
}
EXPORT_SYMBOL_GPL(stop_offloading);

unsigned char *get_active_migrator_name(void)
{
	return migrator.name;
}
EXPORT_SYMBOL_GPL(get_active_migrator_name);
