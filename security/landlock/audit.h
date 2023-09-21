/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Landlock LSM - Audit helpers
 *
 * Copyright © 2023 Microsoft Corporation
 */

#ifndef _SECURITY_LANDLOCK_AUDIT_H
#define _SECURITY_LANDLOCK_AUDIT_H

#include <linux/audit.h>
#include <linux/lsm_audit.h>

#include "ruleset.h"

#ifdef CONFIG_AUDIT

void landlock_log_create_ruleset(struct landlock_ruleset *const ruleset);
void landlock_log_restrict_self(struct landlock_ruleset *const domain,
				struct landlock_ruleset *const ruleset);
void landlock_log_release_ruleset(const struct landlock_ruleset *const ruleset);

#else /* CONFIG_AUDIT */

static inline void
landlock_log_create_ruleset(struct landlock_ruleset *const ruleset)
{
}

static inline void
landlock_log_restrict_self(struct landlock_ruleset *const domain,
			   struct landlock_ruleset *const ruleset)
{
}

static inline void
landlock_log_release_ruleset(const struct landlock_ruleset *const ruleset)
{
}

#endif /* CONFIG_AUDIT */

#endif /* _SECURITY_LANDLOCK_AUDIT_H */
