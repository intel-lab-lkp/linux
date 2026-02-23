// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
#include <linux/module.h>
#include <linux/kernel.h>

static int __init qda_core_init(void)
{
	pr_info("QDA: driver initialization complete\n");
	return 0;
}

static void __exit qda_core_exit(void)
{
	pr_info("QDA: driver exit complete\n");
}

module_init(qda_core_init);
module_exit(qda_core_exit);

MODULE_AUTHOR("Qualcomm AI Infra Team");
MODULE_DESCRIPTION("Qualcomm DSP Accelerator Driver");
MODULE_LICENSE("GPL");
