/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PKWM_TRUSTED_KEY_H
#define __PKWM_TRUSTED_KEY_H

#include <keys/trusted-type.h>

extern struct trusted_key_ops pkwm_trusted_key_ops;

#define PKWM_DEBUG 0

#if PKWM_DEBUG
static inline void dump_options(struct trusted_key_options *o)
{
	bool sb_audit_or_enforce_bit = o->policyhandle & BIT(0);
	bool sb_enforce_bit = o->policyhandle & BIT(1);

	if (sb_audit_or_enforce_bit)
		pr_info("secure boot mode: audit or enforce");
	else if (sb_enforce_bit)
		pr_info("secure boot mode: enforce");
	else
		pr_info("secure boot mode: disabled");
}
#else
static inline void dump_options(struct trusted_key_options *o)
{
}
#endif

#endif
