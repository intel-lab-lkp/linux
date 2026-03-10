/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_SOC_RENESAS_R9A06G032_SYSCTRL_H__
#define __LINUX_SOC_RENESAS_R9A06G032_SYSCTRL_H__

#ifdef CONFIG_CLK_R9A06G032
int r9a06g032_sysctrl_set_dmamux(u32 mask, u32 val);

enum r9a06g032_sysctrl_rst_src {
	R9A06G032_RST_WATCHDOG_CA7_0,
	R9A06G032_RST_WATCHDOG_CA7_1,
};

int r9a06g032_sysctrl_enable_rst(enum r9a06g032_sysctrl_rst_src rst_src);

#else
static inline int r9a06g032_sysctrl_set_dmamux(u32 mask, u32 val) { return -ENODEV; }
static inline int r9a06g032_sysctrl_enable_rst(enum r9a06g032_sysctrl_rst_src rst_src)
{
	return -ENODEV;
}
#endif

#endif /* __LINUX_SOC_RENESAS_R9A06G032_SYSCTRL_H__ */
