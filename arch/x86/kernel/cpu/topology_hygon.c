// SPDX-License-Identifier: GPL-2.0
#include <linux/cpu.h>
#include <linux/printk.h>
#include <linux/spinlock.h>

#include <asm/processor.h>

#include "topology.h"

/* (logical_pkg_id, amd_node_id) -> logical_die_id mapping. */
struct hygon_die_map_entry {
	u32 logical_pkg_id;
	u32 amd_node_id;
	u32 logical_die_id;
};

static DEFINE_SPINLOCK(hygon_die_map_lock);
static struct hygon_die_map_entry hygon_die_map[NR_CPUS];
static u32 hygon_next_logical_die_id;
static unsigned int hygon_die_map_nr;

/*
 * Build a bijective mapping from (logical_pkg_id, amd_node_id) to a contiguous
 * logical_die_id for Hygon CPUs.
 *
 * Returns logical_die_id (>= 0) on success, or -EINVAL on error.
 */
static int hygon_get_logical_die_id(struct topo_scan *tscan)
{
	struct cpuinfo_x86 *c = tscan->c;
	unsigned long flags;
	int logical_die_id;
	unsigned int i;
	int logical_pkg_id = c->topo.logical_pkg_id;

	if (c->x86_vendor != X86_VENDOR_HYGON)
		return -EINVAL;

	if (logical_pkg_id < 0)
		return -EINVAL;

	spin_lock_irqsave(&hygon_die_map_lock, flags);

	/* Look up existing (logical_pkg_id, amd_node_id) -> logical_die_id. */
	for (i = 0; i < hygon_die_map_nr; i++) {
		if (hygon_die_map[i].logical_pkg_id == logical_pkg_id &&
		    hygon_die_map[i].amd_node_id == tscan->amd_node_id) {
			logical_die_id = hygon_die_map[i].logical_die_id;
			goto out_unlock;
		}
	}

	if (hygon_die_map_nr >= ARRAY_SIZE(hygon_die_map)) {
		pr_warn_once("CPU topo: Hygon die map exhausted\n");
		logical_die_id = -EINVAL;
		goto out_unlock;
	}

	/* Allocate new contiguous logical_die_id. */
	logical_die_id = hygon_next_logical_die_id++;
	hygon_die_map[hygon_die_map_nr++] = (struct hygon_die_map_entry) {
		.logical_pkg_id = logical_pkg_id,
		.amd_node_id = tscan->amd_node_id,
		.logical_die_id = logical_die_id,
	};

out_unlock:
	spin_unlock_irqrestore(&hygon_die_map_lock, flags);
	return logical_die_id;
}

void cpu_topology_fixup_hygon(struct topo_scan *tscan)
{
	/* Skip the fixup if CPUID leaf 0x80000026 is available. */
	if (tscan->c->extended_cpuid_level >= 0x80000026)
		return;

	/* When CPUID leaf 0x80000026 is not available, die (node) topology is
	 * not encoded in APIC ID space. logical_die_id from the DIE domain is
	 * incorrect, so fix it up.
	 */
	int logical_die_id = hygon_get_logical_die_id(tscan);

	if (logical_die_id >= 0)
		tscan->c->topo.logical_die_id = logical_die_id;
}
