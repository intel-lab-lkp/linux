// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023, Microsoft, Inc.
 *
 * Author : Roman Kisel <romank@linux.microsoft.com>
 */

#include <asm/mshyperv.h>

void __init hv_vtl_init_platform(void)
{
	pr_info("Linux runs in Hyper-V Virtual Trust Level %d\n", ms_hyperv.vtl);
}

int __init hv_vtl_early_init(void)
{
	return 0;
}
early_initcall(hv_vtl_early_init);
