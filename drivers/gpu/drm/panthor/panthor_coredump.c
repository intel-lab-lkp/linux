// SPDX-License-Identifier: GPL-2.0 or MIT
/* Copyright 2025 Google LLC */

#include <drm/drm_drv.h>
#include <drm/drm_print.h>
#include <drm/drm_managed.h>
#include <generated/utsrelease.h>
#include <linux/devcoredump.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/timekeeping.h>

#include "panthor_coredump.h"
#include "panthor_device.h"
#include "panthor_sched.h"

/**
 * enum panthor_coredump_mask - Coredump state
 */
enum panthor_coredump_mask {
	PANTHOR_COREDUMP_GROUP = BIT(0),
};

/**
 * struct panthor_coredump_header - Coredump header
 */
struct panthor_coredump_header {
	enum panthor_coredump_reason reason;
	ktime_t timestamp;
};

/**
 * struct panthor_coredump - Coredump
 */
struct panthor_coredump {
	/** @ptdev: Device. */
	struct panthor_device *ptdev;

	/** @work: Bottom half of panthor_coredump_capture. */
	struct work_struct work;

	/** @header: Header. */
	struct panthor_coredump_header header;

	/** @mask: Bitmask of captured states. */
	u32 mask;

	struct panthor_coredump_group_state group;

	/* @data: Serialized coredump data. */
	void *data;

	/* @size: Serialized coredump size. */
	size_t size;
};

static const char *reason_str(enum panthor_coredump_reason reason)
{
	switch (reason) {
	case PANTHOR_COREDUMP_REASON_MMU_FAULT:
		return "MMU_FAULT";
	case PANTHOR_COREDUMP_REASON_CSG_REQ_TIMEOUT:
		return "CSG_REQ_TIMEOUT";
	case PANTHOR_COREDUMP_REASON_CSG_UNKNOWN_STATE:
		return "CSG_UNKNOWN_STATE";
	case PANTHOR_COREDUMP_REASON_CSG_PROGRESS_TIMEOUT:
		return "CSG_PROGRESS_TIMEOUT";
	case PANTHOR_COREDUMP_REASON_CS_FATAL:
		return "CS_FATAL";
	case PANTHOR_COREDUMP_REASON_CS_FAULT:
		return "CS_FAULT";
	case PANTHOR_COREDUMP_REASON_CS_TILER_OOM:
		return "CS_TILER_OOM";
	case PANTHOR_COREDUMP_REASON_JOB_TIMEOUT:
		return "JOB_TIMEOUT";
	default:
		return "UNKNOWN";
	}
}

static void print_group(struct drm_printer *p,
			const struct panthor_coredump_group_state *group)
{
	drm_puts(p, "group:\n");
	drm_printf(p, "  priority: %d\n", group->priority);
	drm_printf(p, "  queue_count: %u\n", group->queue_count);
	drm_printf(p, "  pid: %d\n", group->pid);
	drm_printf(p, "  comm: %s\n", group->comm);
	drm_printf(p, "  destroyed: %d\n", group->destroyed);
	drm_printf(p, "  csg_id: %d\n", group->csg_id);
}

static void print_header(struct drm_printer *p,
			 const struct panthor_coredump_header *header,
			 const struct drm_driver *drv)
{
	drm_puts(p, "header:\n");
	drm_puts(p, "  kernel: " UTS_RELEASE "\n");
	drm_puts(p, "  module: " KBUILD_MODNAME "\n");
	drm_printf(p, "  driver_version: %d.%d\n", drv->major, drv->minor);

	drm_printf(p, "  reason: %s\n", reason_str(header->reason));
	drm_printf(p, "  timestamp: %lld\n", ktime_to_ns(header->timestamp));
}

static void print_cd(struct drm_printer *p, const struct panthor_coredump *cd)
{
	/* in YAML format */
	drm_puts(p, "---\n");
	print_header(p, &cd->header, cd->ptdev->base.driver);

	if (cd->mask & PANTHOR_COREDUMP_GROUP)
		print_group(p, &cd->group);
}

static void process_cd(struct panthor_device *ptdev,
		       struct panthor_coredump *cd)
{
	struct drm_print_iterator iter = {
		.remain = SSIZE_MAX,
	};
	struct drm_printer p = drm_coredump_printer(&iter);

	print_cd(&p, cd);

	iter.remain = SSIZE_MAX - iter.remain;
	iter.data = kvmalloc(iter.remain, GFP_USER);
	if (!iter.data)
		return;

	cd->data = iter.data;
	cd->size = iter.remain;

	drm_info(&ptdev->base, "generating coredump of size %zu\n", cd->size);

	p = drm_coredump_printer(&iter);
	print_cd(&p, cd);
}

static void capture_cd(struct panthor_device *ptdev,
		       struct panthor_coredump *cd, struct panthor_group *group)
{
	drm_info(&ptdev->base, "capturing coredump states\n");

	if (group) {
		panthor_group_capture_coredump(group, &cd->group);
		cd->mask |= PANTHOR_COREDUMP_GROUP;
	}
}

static void panthor_coredump_free(void *data)
{
	struct panthor_coredump *cd = data;
	struct panthor_device *ptdev = cd->ptdev;

	kvfree(cd->data);
	kfree(cd);

	atomic_set(&ptdev->coredump.pending, 0);
}

static ssize_t panthor_coredump_read(char *buffer, loff_t offset, size_t count,
				     void *data, size_t datalen)
{
	const struct panthor_coredump *cd = data;

	if (offset >= cd->size)
		return 0;

	if (count > cd->size - offset)
		count = cd->size - offset;

	memcpy(buffer, cd->data + offset, count);

	return count;
}

static void panthor_coredump_process_work(struct work_struct *work)
{
	struct panthor_coredump *cd =
		container_of(work, struct panthor_coredump, work);
	struct panthor_device *ptdev = cd->ptdev;

	process_cd(ptdev, cd);

	dev_coredumpm(ptdev->base.dev, THIS_MODULE, cd, 0, GFP_KERNEL,
		      panthor_coredump_read, panthor_coredump_free);
}

void panthor_coredump_capture(struct panthor_coredump *cd,
			      struct panthor_group *group)
{
	struct panthor_device *ptdev = cd->ptdev;

	capture_cd(ptdev, cd, group);

	queue_work(system_unbound_wq, &cd->work);
}

struct panthor_coredump *
panthor_coredump_alloc(struct panthor_device *ptdev,
		       enum panthor_coredump_reason reason, gfp_t gfp)
{
	struct panthor_coredump *cd;

	/* reject all but the first coredump until it is handled */
	if (atomic_cmpxchg(&ptdev->coredump.pending, 0, 1)) {
		drm_dbg(&ptdev->base, "skip subsequent coredump\n");
		return NULL;
	}

	cd = kzalloc(sizeof(*cd), gfp);
	if (!cd) {
		atomic_set(&ptdev->coredump.pending, 0);
		return NULL;
	}

	cd->ptdev = ptdev;
	INIT_WORK(&cd->work, panthor_coredump_process_work);

	cd->header.reason = reason;
	cd->header.timestamp = ktime_get_real();

	return cd;
}
