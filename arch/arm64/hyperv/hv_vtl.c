// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024, Microsoft, Inc.
 *
 * Author : Roman Kisel <romank@linux.microsoft.com>
 */

#include <asm/mshyperv.h>

void __init hv_vtl_init_platform(void)
{
	pr_info("Linux runs in Hyper-V Virtual Trust Level\n");
}
