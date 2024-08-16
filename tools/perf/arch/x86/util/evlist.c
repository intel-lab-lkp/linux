// SPDX-License-Identifier: GPL-2.0
#include <stdio.h>
#include "util/pmu.h"
#include "util/pmus.h"
#include "util/evlist.h"
#include "util/parse-events.h"
#include "util/event.h"
#include "topdown.h"
#include "evsel.h"

static int ___evlist__add_default_attrs(struct evlist *evlist,
					struct perf_event_attr *attrs,
					size_t nr_attrs)
{
	LIST_HEAD(head);
	size_t i = 0;

	for (i = 0; i < nr_attrs; i++)
		event_attr_init(attrs + i);

	if (perf_pmus__num_core_pmus() == 1)
		return evlist__add_attrs(evlist, attrs, nr_attrs);

	for (i = 0; i < nr_attrs; i++) {
		struct perf_pmu *pmu = NULL;

		if (attrs[i].type == PERF_TYPE_SOFTWARE) {
			struct evsel *evsel = evsel__new(attrs + i);

			if (evsel == NULL)
				goto out_delete_partial_list;
			list_add_tail(&evsel->core.node, &head);
			continue;
		}

		while ((pmu = perf_pmus__scan_core(pmu)) != NULL) {
			struct perf_cpu_map *cpus;
			struct evsel *evsel;

			evsel = evsel__new(attrs + i);
			if (evsel == NULL)
				goto out_delete_partial_list;
			evsel->core.attr.config |= (__u64)pmu->type << PERF_PMU_TYPE_SHIFT;
			cpus = perf_cpu_map__get(pmu->cpus);
			evsel->core.cpus = cpus;
			evsel->core.own_cpus = perf_cpu_map__get(cpus);
			evsel->pmu_name = strdup(pmu->name);
			list_add_tail(&evsel->core.node, &head);
		}
	}

	evlist__splice_list_tail(evlist, &head);

	return 0;

out_delete_partial_list:
	{
		struct evsel *evsel, *n;

		__evlist__for_each_entry_safe(&head, n, evsel)
			evsel__delete(evsel);
	}
	return -1;
}

int arch_evlist__add_default_attrs(struct evlist *evlist,
				   struct perf_event_attr *attrs,
				   size_t nr_attrs)
{
	if (!nr_attrs)
		return 0;

	return ___evlist__add_default_attrs(evlist, attrs, nr_attrs);
}

int arch_evlist__cmp(const struct evsel *lhs, const struct evsel *rhs)
{
	/*
	 * Currently the following topdown events sequence are supported to
	 * move and regroup correctly.
	 *
	 * a. all events in a group
	 *    perf stat -e "{instructions,topdown-retiring,slots}" -C0 sleep 1
	 *    WARNING: events were regrouped to match PMUs
	 *     Performance counter stats for 'CPU(s) 0':
	 *          15,066,240     slots
	 *          1,899,760      instructions
	 *          2,126,998      topdown-retiring
	 * b. all events not in a group
	 *    perf stat -e "instructions,topdown-retiring,slots" -C0 sleep 1
	 *    WARNING: events were regrouped to match PMUs
	 *     Performance counter stats for 'CPU(s) 0':
	 *          2,045,561      instructions
	 *          17,108,370     slots
	 *          2,281,116      topdown-retiring
	 * c. slots event in a group but topdown metrics events outside the group
	 *    perf stat -e "{instructions,slots},topdown-retiring" -C0 sleep 1
	 *    WARNING: events were regrouped to match PMUs
	 *     Performance counter stats for 'CPU(s) 0':
	 *         20,323,878      slots
	 *          2,634,884      instructions
	 *          3,028,656      topdown-retiring
	 * d. slots event and topdown metrics events in two groups
	 *    perf stat -e "{instructions,slots},{topdown-retiring}" -C0 sleep 1
	 *    WARNING: events were regrouped to match PMUs
	 *     Performance counter stats for 'CPU(s) 0':
	 *         26,319,024      slots
	 *          2,427,791      instructions
	 *          2,683,508      topdown-retiring
	 *
	 * If slots event and topdown metrics events are not in same group, the
	 * topdown metrics events must be first event after the slots event group,
	 * otherwise topdown metrics events can't be regrouped correctly, e.g.
	 *
	 * a. perf stat -e "{instructions,slots},cycles,topdown-retiring" -C0 sleep 1
	 *    WARNING: events were regrouped to match PMUs
	 *     Performance counter stats for 'CPU(s) 0':
	 *         17,923,134      slots
	 *          2,154,855      instructions
	 *          3,015,058      cycles
	 *    <not supported>      topdown-retiring
	 *
	 * if slots event and topdown metrics events are in two groups, the group which
	 * has topdown metrics events must contain only the topdown metrics event,
	 * otherwise topdown metrics event can't be regrouped correctly as well, e.g.
	 *
	 * a. perf stat -e "{instructions,slots},{topdown-retiring,cycles}" -C0 sleep 1
	 *    WARNING: events were regrouped to match PMUs
	 *    Error:
	 *    The sys_perf_event_open() syscall returned with 22 (Invalid argument) for
	 *    event (topdown-retiring)
	 */
	if (topdown_sys_has_perf_metrics() &&
	    (arch_evsel__must_be_in_group(lhs) || arch_evsel__must_be_in_group(rhs))) {
		/* Ensure the topdown slots comes first. */
		if (arch_is_topdown_slots(lhs))
			return -1;
		if (arch_is_topdown_slots(rhs))
			return 1;
		/* Followed by topdown events. */
		if (arch_is_topdown_metrics(lhs) && !arch_is_topdown_metrics(rhs))
			return -1;
		/*
		 * Move topdown events forward only when topdown events
		 * are not in same group with previous event.
		 */
		if (!arch_is_topdown_metrics(lhs) && arch_is_topdown_metrics(rhs) &&
		    lhs->core.leader != rhs->core.leader)
			return 1;
	}

	/* Default ordering by insertion index. */
	return lhs->core.idx - rhs->core.idx;
}
