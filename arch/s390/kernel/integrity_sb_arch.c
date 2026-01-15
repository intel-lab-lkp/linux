// SPDX-License-Identifier: GPL-2.0

#include <linux/integrity.h>
#include <asm/boot_data.h>

bool arch_integrity_get_secureboot(void)
{
	return ipl_secure_flag;
}
