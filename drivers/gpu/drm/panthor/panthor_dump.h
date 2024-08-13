/* SPDX-License-Identifier: GPL-2.0 or MIT */
/* SPDX-FileCopyrightText: Copyright Collabora 2024 */

#ifndef __PANTHOR_DUMP_H__
#define __PANTHOR_DUMP_H__

#include <drm/drm_gpuvm.h>
#include <drm/panthor_drm.h>

#include "panthor_device.h"
#include "panthor_gem.h"

struct panthor_core_dump_args {
	struct panthor_device *ptdev;
	struct panthor_vm *group_vm;
	struct panthor_group *group;
};

int panthor_core_dump(struct panthor_core_dump_args *args);

#endif /* __PANTHOR_DUMP_H__ */
