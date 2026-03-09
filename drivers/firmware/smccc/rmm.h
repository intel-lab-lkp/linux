/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SMCCC_RMM_H
#define _SMCCC_RMM_H

#include <linux/platform_device.h>

#ifdef CONFIG_ARM64
#include <asm/rsi_cmds.h>
void __init register_rsi_device(struct platform_device *pdev);
#else

static void __init register_rsi_device(struct platform_device *pdev)
{

}
#endif
#endif
