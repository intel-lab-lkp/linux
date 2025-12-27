// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (C) 2018 - 2025 KylinSoft Co., Ltd. All rights reserved.
 * Copyright (C) 2018 - 2025 Ke Sun <sunke@kylinos.cn>
 */

#include <linux/kernel.h>
#include <linux/cred.h>
#include <linux/capability.h>
#include <linux/hardirq.h>
#include <linux/printk.h>

/*
 * Helper function for Rust to format a restricted pointer (%pK).
 *
 * This function determines what pointer value should be printed based on the
 * kptr_restrict sysctl setting:
 *
 * - kptr_restrict == 0: Returns the original pointer (will be hashed by caller)
 * - kptr_restrict == 1: Returns the original pointer if the current process has
 *                       CAP_SYSLOG and same euid/egid, NULL otherwise
 * - kptr_restrict >= 2: Always returns NULL
 *
 * Returns:
 *   - The original pointer if it should be printed (case 0 or case 1 with permission)
 *   - NULL if it should not be printed (no permission, IRQ context, or restrict >= 2)
 */
const void *rust_helper_kptr_restrict_value(const void *ptr)
{
	switch (kptr_restrict) {
	case 0:
		/* Handle as %p - return original pointer for hashing */
		return ptr;
	case 1: {
		const struct cred *cred;

		/*
		 * kptr_restrict==1 cannot be used in IRQ context because the
		 * capability check would be meaningless (no process context).
		 */
		if (in_hardirq() || in_serving_softirq() || in_nmi())
			return NULL;

		/*
		 * Only return the real pointer value if the current process has
		 * CAP_SYSLOG and is running with the same credentials it started with.
		 * This prevents privilege escalation attacks where a process opens a
		 * file with %pK, then elevates privileges before reading it.
		 */
		cred = current_cred();
		if (!has_capability_noaudit(current, CAP_SYSLOG) ||
		    !uid_eq(cred->euid, cred->uid) ||
		    !gid_eq(cred->egid, cred->gid))
			return NULL;
		break;
	}
	case 2:
	default:
		/* Always hide pointer values when kptr_restrict >= 2 */
		return NULL;
	}

	return ptr;
}
