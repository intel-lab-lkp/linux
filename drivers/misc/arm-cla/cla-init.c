// SPDX-License-Identifier: GPL-2.0-only
/*
 * Arm CLA driver - probing and initialization
 *
 * Copyright 2026 Arm Limited.
 */

#include <linux/cpuhotplug.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/smp.h>

#include <asm/virt.h>

#include "arm-cla.h"

static int cla_cpuhp_state = -1;

static int cla_dev_setup(unsigned int cpu)
{
	struct cla_dev *dev;

	dev = cla_lut_cpu[cpu];
	if (!dev)
		return 0;

	if (WARN_ON(smp_processor_id() != cpu || dev->cpu != cpu))
		return -EINVAL;

	return 0;
}

static int cla_dev_teardown(unsigned int cpu)
{
	struct cla_dev *dev;

	/*
	 * Careful what we return here, the teardown path isn't really allowed
	 * to fail (BUG_ON in kernel/cpu.c)
	 */
	dev = cla_lut_cpu[cpu];
	if (!dev)
		return 0;

	return 0;
}

static int cla_of_to_cpu(struct device_node *of_node)
{
	int cpu;
	int ret;
	u32 cpu_phandle;
	struct device_node *cpu_node;

	if (!of_node)
		return -ENODEV;

	ret = of_property_read_u32(of_node, "cpu", &cpu_phandle);
	if (WARN_ON(ret))
		return -EINVAL;

	cpu_node = of_find_node_by_phandle(cpu_phandle);
	if (WARN_ON(!cpu_node))
		return -EINVAL;

	cpu = of_cpu_node_to_id(cpu_node);
	of_node_put(cpu_node);

	return cpu;
}

static struct cla_dev *cla_dev_alloc(struct device *parent, int cpu,
				     void __iomem *regs, phys_addr_t base)
{
	struct cla_dev *dev;

	dev = devm_kzalloc(parent, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return ERR_PTR(-ENOMEM);

	dev->pfn = __phys_to_pfn(base);
	dev->regs = cla_get_regs(regs, cla_kernel_pl);
	dev->cpu = cpu;
	dev->dev = parent;

	/* Attempt to find device domain, or allocate a new one */
	dev->domain = cla_dev_domain_get(dev);
	if (IS_ERR(dev->domain))
		return ERR_CAST(dev->domain);

	cla_nr_devs++;

	return dev;
}

static int cla_probe(struct platform_device *pdev)
{
	int cpu;
	void __iomem *reg;
	size_t reg_size;
	struct cla_dev *dev;
	struct resource *res;

	/*
	 * TODO: the firmware maps this as well to access PL3, and the guest
	 * will map PL1 and PL0. Avoid TLB attr mismatches by only mapping what
	 * we need.
	 */
	reg = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(reg)) {
		dev_err(&pdev->dev, "could not map CLA registers\n");
		return PTR_ERR(reg);
	}

	if (!IS_ALIGNED(res->start, SZ_256K)) {
		dev_err(&pdev->dev, "invalid CLA registers alignment\n");
		return -EINVAL;
	}

	reg_size = resource_size(res);
	if (reg_size <= CLA_FRAME_SIZE) {
		/* A single CLA. We need information about its CPU. */
		cpu = cla_of_to_cpu(pdev->dev.of_node);
		if (cpu < 0)
			return cpu;

		/*
		 * As a guest we may not get PL3 or PL2. Tolerate CLAs smaller
		 * than 4*regs.
		 */
		if (reg_size < (cla_kernel_pl + 1) * CLA_REG_SIZE)
			return -ENXIO;

		dev = cla_dev_alloc(&pdev->dev, cpu, reg, res->start);
		if (IS_ERR(dev))
			return PTR_ERR(dev);

		dev_dbg(&pdev->dev, "CLA found %pa size 0x%llx\n", &res->start,
			resource_size(res));

	} else {
		dev_err(&pdev->dev, "unexpected CLA registers size\n");
		return -EINVAL;
	}

	return 0;
}

static const struct of_device_id cla_of_match[] = {
	{.compatible = "arm,cla",},
	{},
};
MODULE_DEVICE_TABLE(of, cla_of_match);

static struct platform_driver cla_driver = {
	.driver	= {
		.name			= "arm-cla",
		.of_match_table		= cla_of_match,
	},
	.probe	= cla_probe,
};

static int __init cla_module_init(void)
{
	int ret;

	/*
	 * CPUs may be hotplugged, but all CLAs are described by firmware so the
	 * probe can be synchronous. This only sets up the resources, and CPUHP
	 * callbacks will do the actual peeking and poking.
	 *
	 * This returns an error when no CLA is present.
	 */
	ret = platform_driver_probe(&cla_driver, cla_probe);
	if (ret) {
		if (ret != -ENODEV)
			pr_err("arm-cla: probe failed with %d\n", ret);
		/* Some domains may have been created during probe */
		goto err_domains_free;
	}

	ret = cla_domains_finalise();
	if (ret) {
		pr_err("arm-cla: failed to finalise domains: %d", ret);
		goto err_domains_free;
	}

	/*
	 * Each CPU initializes their own CLA. CPUHP uses a pair of smp_mb()
	 * when calling the startup callback, ensuring that cla_dev_setup()
	 * reads fully initialized cla_lut_cpu and cla_dev structures.
	 */
	ret = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "arm-cla",
				cla_dev_setup, cla_dev_teardown);
	if (ret < 0) {
		pr_err("arm-cla: failed to setup cpuhp: %d", ret);
		goto err_driver_unregister;
	}
	cla_cpuhp_state = ret;

	return 0;

err_driver_unregister:
	platform_driver_unregister(&cla_driver);
err_domains_free:
	cla_domains_free();
	return ret;
}

static void __exit cla_module_exit(void)
{
	cpuhp_remove_state(cla_cpuhp_state);
	platform_driver_unregister(&cla_driver);
	cla_domains_free();
}

module_init(cla_module_init);
module_exit(cla_module_exit);

MODULE_DESCRIPTION("Arm Core Local Accelerator");
MODULE_AUTHOR("Arm Limited");
MODULE_LICENSE("GPL");
