// SPDX-License-Identifier: GPL-2.0
/*
 * Generic support for CPU Cache Invalidate Memregion
 */

#include <linux/spinlock.h>
#include <linux/export.h>
#include <asm/cacheflush.h>


static const struct system_cache_flush_method *scfm_data;
DEFINE_SPINLOCK(scfm_lock);

void generic_set_sys_cache_flush_method(const struct system_cache_flush_method *method)
{
	guard(spinlock_irqsave)(&scfm_lock);
	if (scfm_data || !method || !method->invalidate_memregion)
		return;

	scfm_data = method;
}
EXPORT_SYMBOL_GPL(generic_set_sys_cache_flush_method);

void generic_clr_sys_cache_flush_method(const struct system_cache_flush_method *method)
{
	guard(spinlock_irqsave)(&scfm_lock);
	if (scfm_data && scfm_data == method)
		scfm_data = NULL;
}

int cpu_cache_invalidate_memregion(int res_desc, phys_addr_t start, size_t len)
{
	guard(spinlock_irqsave)(&scfm_lock);
	if (!scfm_data)
		return -EOPNOTSUPP;

	return scfm_data->invalidate_memregion(res_desc, start, len);
}
EXPORT_SYMBOL_NS_GPL(cpu_cache_invalidate_memregion, "DEVMEM");

bool cpu_cache_has_invalidate_memregion(void)
{
	guard(spinlock_irqsave)(&scfm_lock);
	return !!scfm_data;
}
EXPORT_SYMBOL_NS_GPL(cpu_cache_has_invalidate_memregion, "DEVMEM");
