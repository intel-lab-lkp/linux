// SPDX-License-Identifier: GPL-2.0-only
#include <linux/mm.h>
#include <linux/kpkeys.h>

DEFINE_STATIC_KEY_FALSE(kpkeys_hardened_pgtables_enabled);

void __init kpkeys_hardened_pgtables_enable(void)
{
	int ret;

	if (!arch_kpkeys_enabled())
		return;

	static_branch_enable(&kpkeys_hardened_pgtables_enabled);
	ret = kernel_pgtables_set_pkey(KPKEYS_PKEY_PGTABLES);
	WARN_ON(ret);
}
