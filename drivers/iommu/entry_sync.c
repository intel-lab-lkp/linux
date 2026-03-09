// SPDX-License-Identifier: GPL-2.0-only
/*
 * Helpers for drivers to update multi-quanta entries shared with HW without
 * races to minimize breaking changes.
 */
#include "entry_sync.h"
#include <linux/kconfig.h>
#include <linux/atomic.h>

#if IS_ENABLED(CONFIG_IOMMU_ENTRY_SYNC64)
static bool entry_set64(struct entry_sync_writer64 *writer, __le64 *entry,
			const __le64 *target, unsigned int start,
			unsigned int len)
{
	bool changed = false;
	unsigned int i;

	for (i = start; len != 0; len--, i++) {
		if (entry[i] != target[i]) {
			WRITE_ONCE(entry[i], target[i]);
			changed = true;
		}
	}

	if (changed)
		writer->ops->sync(writer);
	return changed;
}

#define entry_sync_writer entry_sync_writer64
#define quanta_t __le64
#define NS(name) CONCATENATE(name, 64)
#include "entry_sync_template.h"
#endif

#if IS_ENABLED(CONFIG_IOMMU_ENTRY_SYNC128)
static bool entry_set128(struct entry_sync_writer128 *writer, u128 *entry,
			 const u128 *target, unsigned int start,
			 unsigned int len)
{
	bool changed = false;
	unsigned int i;

	for (i = start; len != 0; len--, i++) {
		if (entry[i] != target[i]) {
			/*
			 * Use cmpxchg128 to generate an indivisible write from
			 * the CPU to DMA'able memory. This must ensure that HW
			 * sees either the new or old 128 bit value and not
			 * something torn. As updates are serialized by a
			 * spinlock, we use the local (unlocked) variant to
			 * avoid unnecessary bus locking overhead.
			 */
			cmpxchg128_local(&entry[i], entry[i], target[i]);
			changed = true;
		}
	}

	if (changed)
		writer->ops->sync(writer);
	return changed;
}

#define entry_sync_writer entry_sync_writer128
#define quanta_t u128
#define NS(name) CONCATENATE(name, 128)
#include "entry_sync_template.h"
#endif
