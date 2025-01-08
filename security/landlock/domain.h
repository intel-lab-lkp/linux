/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Landlock LSM - Domain management
 *
 * Copyright © 2016-2020 Mickaël Salaün <mic@digikod.net>
 * Copyright © 2018-2020 ANSSI
 */

#ifndef _SECURITY_LANDLOCK_DOMAIN_H
#define _SECURITY_LANDLOCK_DOMAIN_H

#include <linux/landlock.h>
#include <linux/refcount.h>

/**
 * struct landlock_hierarchy - Node in a domain hierarchy
 */
struct landlock_hierarchy {
	/**
	 * @parent: Pointer to the parent node, or NULL if it is a root
	 * Landlock domain.
	 */
	struct landlock_hierarchy *parent;
	/**
	 * @usage: Number of potential children domains plus their parent
	 * domain.
	 */
	refcount_t usage;
};

#endif /* _SECURITY_LANDLOCK_DOMAIN_H */
