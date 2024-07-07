// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2010 Broadcom
 */

#include <linux/init.h>
#include <linux/irqchip.h>
#include <linux/of_address.h>

#include <asm/mach/arch.h>
#include <asm/mach/map.h>
#include <asm/system_info.h>

#include "platsmp.h"

static const char * const bcm2835_compat[] = {
#ifdef CONFIG_ARCH_MULTI_V6
	"brcm,bcm2835",
#endif
#ifdef CONFIG_ARCH_MULTI_V7
	"brcm,bcm2836",
	"brcm,bcm2837",
#endif
	NULL
};

static void __init bcm2835_init(void)
{
	struct device_node *np = of_find_node_by_path("/system");
	u32 val;

	if (!of_property_read_u32(np, "linux,revision", &val))
		system_rev = val;
}

DT_MACHINE_START(BCM2835, "BCM2835")
	.dt_compat = bcm2835_compat,
	.smp = smp_ops(bcm2836_smp_ops),
	.init_machine = bcm2835_init,
MACHINE_END
