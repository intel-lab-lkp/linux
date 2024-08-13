/* SPDX-License-Identifier: GPL-2.0 or MIT */
/* SPDX-FileCopyrightText: Copyright Collabora 2024 */

#ifndef __PANTHOR_DUMP_H__
#define __PANTHOR_DUMP_H__

#include <drm/drm_gpuvm.h>
#include <drm/panthor_drm.h>

#include "panthor_device.h"
#include "panthor_gem.h"

struct panthor_dump_job_entry {
	void *mem;
	size_t size;
	struct list_head node;
};

struct panthor_core_dump_args {
	struct panthor_device *ptdev;
	struct panthor_vm *group_vm;
	struct panthor_group *group;
	/** @job_list: used if the dump contains more than one job.
	 *
	 * Note that the default devcoredump behavior is to discard dumps when a
	 * previous dump has not been read yet. There is also a limit on the number
	 * of dumps that can be stored.
	 */
	struct list_head *job_list;
	/** @append: whether to append the current job dump to job_list */
	bool append;
};

int panthor_core_dump(struct panthor_core_dump_args *args);

#endif /* __PANTHOR_DUMP_H__ */
