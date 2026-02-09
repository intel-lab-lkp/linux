// SPDX-License-Identifier: (GPL-2.0-only OR MIT)
/*
 * Copyright (c) 2026 Amlogic, Inc. All rights reserved
 */

#include <linux/module.h>

#include "clk-module.h"

static int __init aml_clk_driver_init(void)
{
	int ret;

	ret = aml_pll_driver_init();
	if (ret)
		return ret;

	ret = aml_clk_model_driver_init();
	if (ret)
		return ret;

	ret = aml_clk_misc_driver_init();
	if (ret)
		return ret;

	return 0;
}

static void __exit aml_clk_driver_exit(void)
{
	aml_clk_misc_driver_exit();
	aml_clk_model_driver_exit();
	aml_pll_driver_exit();
}

module_init(aml_clk_driver_init);
module_exit(aml_clk_driver_exit);

MODULE_DESCRIPTION("Amlogic Clock Controllers Driver Register");
MODULE_AUTHOR("Chuan Liu <chuan.liu@amlogic.com>");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("CLK_AMLOGIC");
