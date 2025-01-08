/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Landlock - Kernel API
 *
 * Copyright © 2024-2025 Microsoft Corporation
 */

#ifndef _LINUX_LANDLOCK_H
#define _LINUX_LANDLOCK_H

struct landlock_hierarchy;

#ifdef CONFIG_SECURITY_LANDLOCK

void landlock_get_hierarchy(struct landlock_hierarchy *hierarchy);

void landlock_put_hierarchy(struct landlock_hierarchy *hierarchy);

#else /* CONFIG_SECURITY_LANDLOCK */

static inline void landlock_get_hierarchy(struct landlock_hierarchy *hierarchy)
{
}

static inline void landlock_put_hierarchy(struct landlock_hierarchy *hierarchy)
{
}

#endif /* CONFIG_SECURITY_LANDLOCK */

#endif /* _LINUX_LANDLOCK_H */
