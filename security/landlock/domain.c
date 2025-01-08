// SPDX-License-Identifier: GPL-2.0-only
/*
 * Landlock LSM - Domain management
 *
 * Copyright © 2016-2020 Mickaël Salaün <mic@digikod.net>
 * Copyright © 2018-2020 ANSSI
 * Copyright © 2024-2025 Microsoft Corporation
 */

#include <linux/landlock.h>
#include <linux/mm.h>

#include "domain.h"
#include "id.h"

void landlock_get_hierarchy(struct landlock_hierarchy *const hierarchy)
{
	if (hierarchy)
		refcount_inc(&hierarchy->usage);
}

void landlock_put_hierarchy(struct landlock_hierarchy *hierarchy)
{
	while (hierarchy && refcount_dec_and_test(&hierarchy->usage)) {
		const struct landlock_hierarchy *const freeme = hierarchy;

		hierarchy = hierarchy->parent;
		kfree(freeme);
	}
}

#ifdef CONFIG_AUDIT

/**
 * landlock_init_current_hierarchy - Partially initialize landlock_hierarchy
 *
 * @hierarchy: The hierarchy to initialize.
 *
 * @hierarchy->parent and @hierarchy->usage should already be set.
 */
int landlock_init_current_hierarchy(struct landlock_hierarchy *const hierarchy)
{
	hierarchy->id = landlock_get_id_range(1);
	return 0;
}

#endif /* CONFIG_AUDIT */
