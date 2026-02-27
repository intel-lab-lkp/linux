// SPDX-License-Identifier: GPL-2.0-only
#include <linux/kpkeys.h>
#include <linux/mm.h>

__ro_after_init DEFINE_STATIC_KEY_FALSE(kpkeys_hardened_pgtables_key);

void __init kpkeys_hardened_pgtables_init(void)
{
	if (!arch_kpkeys_enabled())
		return;

	static_branch_enable(&kpkeys_hardened_pgtables_key);
}
