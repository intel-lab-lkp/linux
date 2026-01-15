// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019 IBM Corporation
 * Author: Nayna Jain
 */

#include <linux/integrity.h>
#include <asm/secure_boot.h>

bool arch_integrity_get_secureboot(void)
{
	return is_ppc_secureboot_enabled();
}
