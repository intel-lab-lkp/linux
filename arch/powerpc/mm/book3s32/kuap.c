// SPDX-License-Identifier: GPL-2.0-or-later

#include <asm/kup.h>
#include <asm/smp.h>

struct static_key_false disable_kuap_key;
EXPORT_SYMBOL(disable_kuap_key);

void kuap_lock_all_ool(void)
{
	kuap_lock_all();
}
EXPORT_SYMBOL(kuap_lock_all_ool);

void kuap_unlock_all_ool(void)
{
	kuap_unlock_all();
}
EXPORT_SYMBOL(kuap_unlock_all_ool);

void setup_kuap(bool disabled)
{
	pr_info("%s:%d\n", __func__, __LINE__);
	if (!disabled) {
		pr_info("%s:%d\n", __func__, __LINE__);
		kuap_lock_all_ool();
		init_mm.context.sr0 |= SR_KS;
		current->thread.sr0 |= SR_KS;
	}
	pr_info("%s:%d\n", __func__, __LINE__);

	if (smp_processor_id() != boot_cpuid)
		return;

	pr_info("%s:%d\n", __func__, __LINE__);
	if (disabled)
		static_branch_enable(&disable_kuap_key);
	else
		pr_info("Activating Kernel Userspace Access Protection\n");
	pr_info("%s:%d\n", __func__, __LINE__);
}
