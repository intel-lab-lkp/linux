/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PKWM_TRUSTED_KEY_H
#define __PKWM_TRUSTED_KEY_H

#include <keys/trusted-type.h>

extern struct trusted_key_ops pkwm_trusted_key_ops;

static inline void dump_options(struct trusted_key_options *o)
{
	bool sb_audit_or_enforce_bit = o->wrap_flags & BIT(0);
	bool sb_enforce_bit = o->wrap_flags & BIT(1);

	if (sb_audit_or_enforce_bit)
		pr_debug("secure boot mode required: audit or enforce");
	else if (sb_enforce_bit)
		pr_debug("secure boot mode required: enforce");
	else
		pr_debug("secure boot mode required: disabled");
}

#endif
