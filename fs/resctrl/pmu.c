// SPDX-License-Identifier: GPL-2.0-only
/*
 * Perf event access to resctrl monitoring (cache occupancy, memory bandwidth)
 */

#define pr_fmt(fmt) "resctrl_pmu: " fmt

#include <linux/kernel.h>
#include <linux/perf_event.h>
#include <linux/errno.h>
#include <linux/file.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/seq_file.h>
#include "internal.h"

static struct pmu resctrl_pmu;

/*
 * Event private data - stores information about the monitored resctrl group
 */
struct resctrl_pmu_event {
	struct rdtgroup *rdtgrp;	/* Reference to rdtgroup being monitored */
};

static void resctrl_event_destroy(struct perf_event *event);

/*
 * Initialize a new resctrl perf event
 * The config field contains the file descriptor of the monitoring file
 */
static int resctrl_event_init(struct perf_event *event)
{
	struct resctrl_pmu_event *resctrl_event;
	struct file *file;
	struct rdtgroup *rdtgrp;
	int fd;
	int ret;

	fd = (int)event->attr.config;
	if (fd < 0)
		return -EINVAL;

	file = fget(fd);
	if (!file)
		return -EBADF;

	/* Resolve rdtgroup from the monitoring file and take a reference */
	rdtgrp = rdtgroup_get_from_file(file);
	fput(file);
	if (IS_ERR(rdtgrp))
		return PTR_ERR(rdtgrp);

	resctrl_event = kzalloc(sizeof(*resctrl_event), GFP_KERNEL);
	if (!resctrl_event) {
		rdtgroup_put(rdtgrp);
		return -ENOMEM;
	}

	resctrl_event->rdtgrp = rdtgrp;
	event->pmu_private = resctrl_event;
	event->destroy = resctrl_event_destroy;

	return 0;
}

static void resctrl_event_destroy(struct perf_event *event)
{
	struct resctrl_pmu_event *resctrl_event = event->pmu_private;

	if (resctrl_event) {
		struct rdtgroup *rdtgrp = resctrl_event->rdtgrp;

		if (rdtgrp)
			rdtgroup_put(rdtgrp);

		kfree(resctrl_event);
		event->pmu_private = NULL;
	}
}

static void resctrl_event_update(struct perf_event *event)
{
	/* Currently just a stub - would read actual cache occupancy here */
	local64_set(&event->hw.prev_count, 0);
}

static void resctrl_event_start(struct perf_event *event, int flags)
{
	resctrl_event_update(event);
}

static void resctrl_event_stop(struct perf_event *event, int flags)
{
	if (flags & PERF_EF_UPDATE)
		resctrl_event_update(event);
}

static int resctrl_event_add(struct perf_event *event, int flags)
{
	if (flags & PERF_EF_START)
		resctrl_event_start(event, flags);

	return 0;
}

static void resctrl_event_del(struct perf_event *event, int flags)
{
	resctrl_event_stop(event, PERF_EF_UPDATE);
}

static struct pmu resctrl_pmu = {
	.task_ctx_nr	= perf_invalid_context,
	.event_init	= resctrl_event_init,
	.add		= resctrl_event_add,
	.del		= resctrl_event_del,
	.start		= resctrl_event_start,
	.stop		= resctrl_event_stop,
	.read		= resctrl_event_update,
	.capabilities	= PERF_PMU_CAP_NO_INTERRUPT | PERF_PMU_CAP_NO_EXCLUDE,
};

int resctrl_pmu_init(void)
{
	int ret;

	ret = perf_pmu_register(&resctrl_pmu, "resctrl", -1);
	if (ret) {
		pr_err("Failed to register resctrl PMU: %d\n", ret);
		return ret;
	}

	return 0;
}

void resctrl_pmu_exit(void)
{
	perf_pmu_unregister(&resctrl_pmu);
}
