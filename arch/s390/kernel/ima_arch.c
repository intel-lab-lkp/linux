// SPDX-License-Identifier: GPL-2.0

#include <linux/ima.h>
#include <linux/integrity.h>
#include <asm/boot_data.h>

bool arch_integrity_get_secureboot(void)
{
	return ipl_secure_flag;
}

#ifdef CONFIG_IMA_SECURE_AND_OR_TRUSTED_BOOT
const char * const *arch_get_ima_policy(void)
{
	return NULL;
}
#endif
