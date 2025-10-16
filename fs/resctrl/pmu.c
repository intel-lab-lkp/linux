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
#include <linux/cpu.h>
#include "internal.h"

static struct pmu resctrl_pmu;

/*
 * Event private data - stores information about the monitored resctrl group
 */
struct resctrl_pmu_event {
	struct rdtgroup *rdtgrp;	/* Reference to rdtgroup being monitored */
	struct rmid_read rr;		/* RMID read setup for monitoring */
	cpumask_t *cpumask;		/* Valid CPUs for this monitoring file */
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
	struct kernfs_open_file *of;
	struct mon_data *md;
	struct rmid_read rr = {0};
	cpumask_t *cpumask;
	int fd;
	int ret;

	if (event->cpu < 0)
		return -EINVAL;

	fd = (int)event->attr.config;
	if (fd < 0)
		return -EINVAL;

	file = fget(fd);
	if (!file)
		return -EBADF;

	of = rdtgroup_get_mondata_open_file(file);
	if (IS_ERR(of)) {
		ret = PTR_ERR(of);
		goto out_fput;
	}

	/* Extract mon_data which specifies which resource to measure */
	if (!of->kn || !of->kn->priv) {
		ret = -EIO;
		goto out_fput;
	}
	md = of->kn->priv;

	rdtgrp = rdtgroup_get_from_mondata_file(of);
	if (IS_ERR(rdtgrp)) {
		ret = PTR_ERR(rdtgrp);
		goto out_fput;
	}

	fput(file);
	file = NULL;

	cpus_read_lock();

	ret = mon_event_setup_read(&rr, &cpumask, md, rdtgrp);
	if (ret) {
		cpus_read_unlock();
		rdtgroup_put(rdtgrp);
		return ret;
	}

	/* Validate that the requested CPU is in the valid CPU mask for this monitoring file */
	if (!cpumask_test_cpu(event->cpu, cpumask)) {
		ret = -EINVAL;
		cpus_read_unlock();
		rdtgroup_put(rdtgrp);
		return ret;
	}

	cpus_read_unlock();

	resctrl_event = kzalloc(sizeof(*resctrl_event), GFP_KERNEL);
	if (!resctrl_event) {
		rdtgroup_put(rdtgrp);
		return -ENOMEM;
	}

	resctrl_event->rdtgrp = rdtgrp;
	resctrl_event->rr = rr;
	resctrl_event->cpumask = cpumask;
	event->pmu_private = resctrl_event;
	event->destroy = resctrl_event_destroy;

	return 0;

out_fput:
	fput(file);
	return ret;
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
	struct resctrl_pmu_event *resctrl_event = event->pmu_private;
	struct rdtgroup *rdtgrp = resctrl_event->rdtgrp;
	struct rmid_read rr;
	u64 value = 0;

	/* Check if rdtgroup has been deleted */
	if (rdtgrp->flags & RDT_DELETED) {
		local64_set(&event->count, 0);
		return;
	}

	/* Setup rmid_read structure with current parameters */
	rr = resctrl_event->rr;
	rr.val = 0;
	rr.err = 0;

	/* Take cpus read lock only around the actual RMID read */
	cpus_read_lock();
	mon_event_read_this_cpu(&rr);
	cpus_read_unlock();

	/* Update counter value based on read result */
	if (!rr.err)
		value = rr.val;
	else
		WARN_ONCE(1, "resctrl PMU: RMID read error (err=%d) for closid=%u, rmid=%u, evtid=%d\n",
			  rr.err, rdtgrp->closid, rdtgrp->mon.rmid, rr.evtid);

	local64_set(&event->count, value);
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
