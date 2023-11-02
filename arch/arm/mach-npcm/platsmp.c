// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2018 Nuvoton Technology corporation.
// Copyright 2018 Google, Inc.

#define pr_fmt(fmt) "nuvoton,npcm7xx-smp: " fmt

#include <linux/delay.h>
#include <linux/smp.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <asm/cacheflush.h>
#include <asm/smp.h>
#include <asm/smp_plat.h>
#include <asm/smp_scu.h>

#define NPCM7XX_SCRPAD_REG 0x13c

extern void npcm7xx_secondary_startup(void);

static int npcm7xx_smp_boot_secondary(unsigned int cpu,
				      struct task_struct *idle)
{
	struct device_node *gcr_np;
	void __iomem *gcr_base;
	int ret = 0;

	gcr_np = of_find_compatible_node(NULL, NULL, "nuvoton,npcm750-gcr");
	if (!gcr_np) {
		pr_err("no gcr device node\n");
		ret = -ENODEV;
		goto out;
	}
	gcr_base = of_iomap(gcr_np, 0);
	if (!gcr_base) {
		pr_err("could not iomap gcr");
		ret = -ENOMEM;
		goto out;
	}

	/* give boot ROM kernel start address. */
	iowrite32(__pa_symbol(npcm7xx_secondary_startup), gcr_base +
		  NPCM7XX_SCRPAD_REG);
	/* make sure the previous write is seen by all observers. */
	dsb_sev();

	iounmap(gcr_base);
out:
	return ret;
}

static void __init npcm7xx_smp_prepare_cpus(unsigned int max_cpus)
{
	struct device_node *scu_np;
	void __iomem *scu_base;

	scu_np = of_find_compatible_node(NULL, NULL, "arm,cortex-a9-scu");
	if (!scu_np) {
		pr_err("no scu device node\n");
		return;
	}
	scu_base = of_iomap(scu_np, 0);
	if (!scu_base) {
		pr_err("could not iomap scu");
		return;
	}

	scu_enable(scu_base);

	iounmap(scu_base);
}
#ifdef CONFIG_HOTPLUG_CPU
static void npcm7xx_cpu_die(unsigned int cpu)
{
	while (1)
		cpu_do_idle();
}

static int npcm7xx_cpu_kill(unsigned int l_cpu)
{
	return 1;
}
#endif


static struct smp_operations npcm7xx_smp_ops __initdata = {
	.smp_prepare_cpus = npcm7xx_smp_prepare_cpus,
	.smp_boot_secondary = npcm7xx_smp_boot_secondary,
#ifdef CONFIG_HOTPLUG_CPU
	.cpu_die			= npcm7xx_cpu_die,
	.cpu_kill			= npcm7xx_cpu_kill,
#endif
};

CPU_METHOD_OF_DECLARE(npcm7xx_smp, "nuvoton,npcm750-smp", &npcm7xx_smp_ops);
