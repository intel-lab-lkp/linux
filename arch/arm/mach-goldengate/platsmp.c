// SPDX-License-Identifier: GPL-2.0-only
/*
 * SMP support for the Cortina Systems CS75xx SoCs
 *
 * The bootloader parks the secondary core in a WFE loop that polls a
 * global software register and jumps to the address found there once it
 * becomes non-zero. The register is described by the cpu-release-addr
 * property of the secondary CPU node.
 */

#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/smp.h>
#include <asm/smp_plat.h>
#include <asm/smp_scu.h>

static int cs75xx_boot_secondary(unsigned int cpu, struct task_struct *idle)
{
	struct device_node *np;
	void __iomem *release;
	u64 addr;
	int ret;

	np = of_get_cpu_node(cpu, NULL);
	if (!np)
		return -ENODEV;

	ret = of_property_read_u64(np, "cpu-release-addr", &addr);
	of_node_put(np);
	if (ret) {
		pr_err("CPU%u: missing cpu-release-addr\n", cpu);
		return ret;
	}

	release = ioremap(addr, sizeof(u32));
	if (!release)
		return -ENOMEM;

	writel(__pa_symbol(secondary_startup), release);
	iounmap(release);

	/* The secondary core waits in WFE, wake it up */
	dsb_sev();

	return 0;
}

static void __init cs75xx_smp_prepare_cpus(unsigned int max_cpus)
{
	struct device_node *np;
	void __iomem *scu_base;

	np = of_find_compatible_node(NULL, NULL, "arm,cortex-a9-scu");
	scu_base = of_iomap(np, 0);
	of_node_put(np);
	if (!scu_base) {
		pr_err("failed to map SCU\n");
		return;
	}

	scu_enable(scu_base);
	iounmap(scu_base);
}

static const struct smp_operations cs75xx_smp_ops __initconst = {
	.smp_prepare_cpus	= cs75xx_smp_prepare_cpus,
	.smp_boot_secondary	= cs75xx_boot_secondary,
};

CPU_METHOD_OF_DECLARE(cs7542_smp, "cortina,cs7542-smp", &cs75xx_smp_ops);
