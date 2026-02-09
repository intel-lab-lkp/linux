/* SPDX-License-Identifier: (GPL-2.0-only OR MIT) */
/*
 * Copyright (c) 2026 Amlogic, Inc. All rights reserved
 */

#ifndef __AML_CLK_MODULE_H
#define __AML_CLK_MODULE_H

#include <linux/kconfig.h>

#if IS_ENABLED(CONFIG_COMMON_CLK_AMLOGIC_PLL)
extern int aml_pll_driver_init(void);
extern void aml_pll_driver_exit(void);
#else /* CONFIG_COMMON_CLK_AMLOGIC_PLL */
static inline int aml_pll_driver_init(void)
{
	return 0;
}

static inline void aml_pll_driver_exit(void)
{
}
#endif /* CONFIG_COMMON_CLK_AMLOGIC_PLL */

#if IS_ENABLED(CONFIG_COMMON_CLK_AMLOGIC_MODEL)
extern int aml_clk_model_driver_init(void);
extern void aml_clk_model_driver_exit(void);
#else /* CONFIG_COMMON_CLK_AMLOGIC_MODEL */
static inline int aml_clk_model_driver_init(void)
{
	return 0;
}

static inline void aml_clk_model_driver_exit(void)
{
}
#endif /* CONFIG_COMMON_CLK_AMLOGIC_MODEL */

#if IS_ENABLED(CONFIG_COMMON_CLK_AMLOGIC_MISC)
extern int aml_clk_misc_driver_init(void);
extern void aml_clk_misc_driver_exit(void);
#else /* CONFIG_COMMON_CLK_AMLOGIC_MISC */
static inline int aml_clk_misc_driver_init(void)
{
	return 0;
}

static inline void aml_clk_misc_driver_exit(void)
{
}
#endif /* CONFIG_COMMON_CLK_AMLOGIC_MISC */

#endif /* __AML_CLK_MODULE_H */
