/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SMCCC_RMM_H
#define _SMCCC_RMM_H

#include <linux/init.h>

#ifdef CONFIG_ARM64
#include <linux/arm-smccc-bus.h>
#include <asm/rsi_cmds.h>
void __init register_rsi_device(void);
#else

static inline void __init register_rsi_device(void)
{
}
#endif
#endif
