// SPDX-License-Identifier: GPL-2.0
/*
 * Description: PMUs specific to running nested KVM-HV guests
 * on Book3S processors (specifically POWER9 and later).
 */

#define pr_fmt(fmt)  "kvmppc-pmu: " fmt

#include "asm-generic/local64.h"
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/ratelimit.h>
#include <linux/kvm_host.h>
#include <linux/gfp_types.h>
#include <linux/pgtable.h>
#include <linux/perf_event.h>
#include <linux/spinlock_types.h>
#include <linux/spinlock.h>

#include <asm/types.h>
#include <asm/kvm_ppc.h>
#include <asm/kvm_book3s.h>
#include <asm/mmu.h>
#include <asm/pgalloc.h>
#include <asm/pte-walk.h>
#include <asm/reg.h>
#include <asm/plpar_wrappers.h>
#include <asm/firmware.h>

enum kvmppc_pmu_eventid {
	KVMPPC_EVENT_MAX,
};

static struct attribute *kvmppc_pmu_events_attr[] = {
	NULL,
};

static const struct attribute_group kvmppc_pmu_events_group = {
	.name = "events",
	.attrs = kvmppc_pmu_events_attr,
};

PMU_FORMAT_ATTR(event, "config:0");
static struct attribute *kvmppc_pmu_format_attr[] = {
	&format_attr_event.attr,
	NULL,
};

static struct attribute_group kvmppc_pmu_format_group = {
	.name = "format",
	.attrs = kvmppc_pmu_format_attr,
};

static const struct attribute_group *kvmppc_pmu_attr_groups[] = {
	&kvmppc_pmu_events_group,
	&kvmppc_pmu_format_group,
	NULL,
};

static int kvmppc_pmu_event_init(struct perf_event *event)
{
	unsigned int config = event->attr.config;

	pr_debug("%s: Event(%p) id=%llu cpu=%x on_cpu=%x config=%u",
		 __func__, event, event->id, event->cpu,
		 event->oncpu, config);

	if (event->attr.type != event->pmu->type)
		return -ENOENT;

	if (config >= KVMPPC_EVENT_MAX)
		return -EINVAL;

	local64_set(&event->hw.prev_count, 0);
	local64_set(&event->count, 0);

	return 0;
}

static void kvmppc_pmu_del(struct perf_event *event, int flags)
{
}

static int kvmppc_pmu_add(struct perf_event *event, int flags)
{
	return 0;
}

static void kvmppc_pmu_read(struct perf_event *event)
{
}

/* L1 wide counters PMU */
static struct pmu kvmppc_pmu = {
	.task_ctx_nr = perf_sw_context,
	.name = "kvm-hv",
	.event_init = kvmppc_pmu_event_init,
	.add = kvmppc_pmu_add,
	.del = kvmppc_pmu_del,
	.read = kvmppc_pmu_read,
	.attr_groups = kvmppc_pmu_attr_groups,
	.type = -1,
};

int kvmppc_register_pmu(void)
{
	int rc = -EOPNOTSUPP;

	/* only support events for nestedv2 right now */
	if (kvmhv_is_nestedv2()) {
		/* Setup done now register the PMU */
		pr_info("Registering kvm-hv pmu");

		/* Register only if we arent already registered */
		rc = (kvmppc_pmu.type == -1) ?
			     perf_pmu_register(&kvmppc_pmu, kvmppc_pmu.name,
					       -1) : 0;
	}

	return rc;
}
EXPORT_SYMBOL_GPL(kvmppc_register_pmu);

void kvmppc_unregister_pmu(void)
{
	if (kvmhv_is_nestedv2()) {
		if (kvmppc_pmu.type != -1)
			perf_pmu_unregister(&kvmppc_pmu);

		pr_info("kvmhv_pmu unregistered.\n");
	}
}
EXPORT_SYMBOL_GPL(kvmppc_unregister_pmu);
