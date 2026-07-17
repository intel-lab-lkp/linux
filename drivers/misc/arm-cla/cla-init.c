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

static int cla_reset_ts(struct cla_dev *dev, unsigned int accid)
{
	int ret;
	u64 reg;

	ret = cla_op_regread(dev, accid, CLA_REG_ACAP, 1, &reg);
	if (ret)
		return ret;
	if (!FIELD_GET(CLA_ACAP_TS, reg))
		return 0;

	/*
	 * Disable TS control from userspace, provide TS from CNTP. If we do
	 * have to provide a timer to userspace or a virtual offset to a guest,
	 * we'll need to make sure we have access to both TSCTRLOWNER and
	 * TSOFFOWNER. For now best effort.
	 */
	reg = FIELD_PREP(CLA_TSCTRLOWNER_PL, cla_kernel_pl);
	ret = cla_op_regwrite(dev, accid, CLA_REG_TSCTRLOWNER, 1, &reg);
	if (!ret) {
		reg = FIELD_PREP(CLA_TSCTRL_TS, CLA_TSCTRL_PHYSICAL);
		ret = cla_op_regwrite(dev, accid, CLA_REG_TSCTRL, 1, &reg);
		if (ret)
			return ret;
	}

	reg = FIELD_PREP(CLA_TSOFFOWNER_PL, cla_kernel_pl);
	ret = cla_op_regread(dev, accid, CLA_REG_TSOFFOWNER, 1, &reg);
	if (!ret) {
		reg = 0;
		ret = cla_op_regwrite(dev, accid, CLA_REG_TSVOFF, 1, &reg);
		if (ret)
			return ret;
		ret = cla_op_regwrite(dev, accid, CLA_REG_TSPOFF, 1, &reg);
		if (ret)
			return ret;
	}
	return 0;
}

static int cla_reset_pmu(struct cla_dev *dev, unsigned int accid)
{
	int ret;
	u64 reg;

	/* Disable PMU access */
	reg = FIELD_PREP(CLA_PMUOWNER_PL, cla_kernel_pl);
	ret = cla_op_regwrite(dev, accid, CLA_REG_PMUOWNER, 1, &reg);
	if (!ret) {
		reg = 0;
		ret = cla_op_regwrite(dev, accid, CLA_REG_PMURESET, 1, &reg);
		if (ret)
			return ret;
	}
	return 0;
}

/*
 * Return: 0 on success, 1 if the accelerator is not attached or not usable, or
 * an error
 */
static int cla_dev_setup_accel(struct cla_dev *dev, unsigned int accid)
{
	struct cla_accel_desc *desc = &dev->accel_descs[accid];
	u64 status;
	u64 iassize;
	u64 acap;
	int ret;

	/*
	 * Probe and reset. Return 1 if no accelerator is attached, happy days.
	 * If the accelerator is unavailable (masked by higher PL with PLxCTRL),
	 * return an error. Individual accelerators cannot be owned by a higher
	 * PL, since the MTC is shared between all accelerators attached to this
	 * CLA.
	 *
	 * Some accelerators will be masked due to returning 1 further down this
	 * function. If we end up with no dev->accelerators because of that we
	 * won't setup the MTC, but as long as this reset succeeds, the
	 * accelerator is not issuing memory transactions.
	 */
	ret = cla_op_reset(dev, accid);
	if (ret)
		return ret;

	status = cla_reg_read(dev, CLA_REG_STATUS(accid));
	if ((status & CLA_STATUS_STATE_MASK) != CLA_STATUS_STATE_IDLE) {
		cla_err(dev, "unexpected status 0x%llx for accelerator %d\n",
			status, accid);
		return -EIO;
	}

	/*
	 * We don't support SROP (SAVE and RESTORE ops), only context switching
	 * with REGREAD and REGWRITE. SROP would require finding a DMA buffer
	 * where to save state, ideally in userspace process to avoid kernel
	 * DMA. It's complicated and no implementation needs it at the moment.
	 */
	ret = cla_op_regread(dev, accid, CLA_REG_ACAP, 1, &acap);
	if (ret)
		return ret;
	if (FIELD_GET(CLA_ACAP_SROP, acap)) {
		cla_err(dev, "[%u] SROP not supported\n", accid);
		return 1;
	}

	/*
	 * Cache some standard accelerator registers that user space may query
	 * via ioctl from a remote CPU.
	 */
	ret = cla_op_regread(dev, accid, CLA_REG_IIDR, 1, &desc->iidr);
	if (ret)
		return ret;
	ret = cla_op_regread(dev, accid, CLA_REG_DEVARCH, 1, &desc->devarch);
	if (ret)
		return ret;
	ret = cla_op_regread(dev, accid, CLA_REG_REVIDR, 1, &desc->revidr);
	if (ret)
		return ret;

	ret = cla_op_regread(dev, accid, CLA_REG_IASSIZE, 1, &iassize);
	if (ret)
		return ret;
	if (FIELD_GET(CLA_ACAP_REGSTATE, acap) && iassize)
		dev->iassizes += iassize;

	/*
	 * The following are nice to have, but the accelerator should work
	 * without them.
	 */
	ret = cla_reset_ts(dev, accid);
	if (ret)
		cla_err(dev, "[%u] could not reset TS: %d\n", accid, ret);

	ret = cla_reset_pmu(dev, accid);
	if (ret)
		cla_err(dev, "[%u] could not reset PMU: %d\n", accid, ret);

	return 0;
}

/* Clean the device before releasing it */
static void cla_dev_reinit(struct cla_dev *dev)
{
	int i;
	bool broken;

	mutex_lock(&dev->lock);
	broken = dev->broken;
	mutex_unlock(&dev->lock);
	if (broken)
		return;

	if (WARN_ON(cla_op_reset_all(dev)) ||
	    WARN_ON(cla_mtc_clear(dev))) {
		mutex_lock(&dev->lock);
		dev->broken = true;
		mutex_unlock(&dev->lock);
		cla_domain_set_broken(dev->domain);
		return;
	}

	if (is_kernel_in_hyp_mode())
		cla_reg_write(dev, CLA_REG_PL1CTRL, ~0ULL);
	cla_reg_write(dev, CLA_REG_PL0CTRL, ~0ULL);
	for (i = 0; i < CLA_NUM_DATA_REGS; i++)
		cla_reg_write(dev, CLA_REG_DATA(i), 0);
	cla_reg_write(dev, CLA_REG_LRESP, 0);
}

static int cla_dev_worker_init(struct cla_dev *dev, int cpu)
{
	struct kthread_worker *worker;

	worker = kthread_run_worker_on_cpu(cpu, 0, "cla-dev-worker/%u");
	if (IS_ERR(worker))
		return PTR_ERR(worker);

	mutex_lock(&dev->lock);
	WARN_ON(dev->worker);
	dev->worker = worker;
	mutex_unlock(&dev->lock);

	return 0;
}

static void cla_dev_worker_destroy(struct cla_dev *dev)
{
	struct kthread_worker *worker;

	/*
	 * Mark the worker as NULL, which prevents any new work from being
	 * queued to it. Then destroy it, which will flush any pending work.
	 * worker_sem guarantees lifetime of worker when flushing work in other
	 * paths. We must reinit the work so that work->worker is not dangling
	 * after releasing worker_sem.
	 */
	mutex_lock(&dev->lock);
	worker = dev->worker;
	dev->worker = NULL;
	mutex_unlock(&dev->lock);

	if (worker) {
		down_write(&dev->worker_sem);
		kthread_destroy_worker(worker);
		kthread_init_work(&dev->call.switch_ctx, cla_dev_switch_ctx);
		up_write(&dev->worker_sem);
	}
}

static int cla_dev_setup(unsigned int cpu)
{
	int i;
	int ret;
	bool broken;
	unsigned int accid;
	struct cla_dev *dev;
	u64 plxctrl_val = 0;

	dev = cla_lut_cpu[cpu];
	if (!dev)
		return 0;

	mutex_lock(&dev->lock);
	broken = dev->broken;
	mutex_unlock(&dev->lock);
	if (broken)
		return 0;

	if (WARN_ON(smp_processor_id() != cpu || dev->cpu != cpu))
		return -EINVAL;

	dev->aidr = cla_reg_read(dev, CLA_REG_CLAAIDR);
	if (dev->aidr != CLA_AAIDR_1_0)
		return -EPROTONOSUPPORT;

	/* Clear DATA and LRESP_DATANZ */
	for (i = 0; i < CLA_NUM_DATA_REGS; i++)
		cla_reg_write(dev, CLA_REG_DATA(i), 0);

	/*
	 * Reset all accelerators. We restrict PLxCTRL to the accelerators that
	 * are attached and well behaved.
	 */
	for (accid = 0; accid < CLA_NUM_ACC; accid++) {
		ret = cla_dev_setup_accel(dev, accid);
		if (ret > 0)
			continue;
		else if (ret < 0)
			goto err;

		dev->accelerators |= (1 << accid);
		plxctrl_val |= CLA_PLxCTRL_PREP(accid,
						FIELD_PREP(CLA_PLxCTRL_AVAIL, 1));
	}

	if (is_kernel_in_hyp_mode())
		cla_reg_write(dev, CLA_REG_PL1CTRL, plxctrl_val);

	cla_reg_write(dev, CLA_REG_PL0CTRL, plxctrl_val);

	if (dev->accelerators) {
		ret = cla_mtc_setup(dev);
		if (ret)
			goto err;

		cla_info(dev, "available accelerators: 0x%02x\n",
			 dev->accelerators);
	}

	ret = cla_dev_worker_init(dev, cpu);
	if (ret)
		goto err;

	return 0;
err:
	cla_dev_reinit(dev);
	return ret;
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

	cla_dev_worker_destroy(dev);
	cla_dev_reinit(dev);

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

	mutex_init(&dev->lock);
	init_rwsem(&dev->worker_sem);
	kthread_init_work(&dev->call.switch_ctx, cla_dev_switch_ctx);

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

	ret = cla_user_init();
	if (ret)
		goto err_cpuhp_remove;

	return 0;

err_cpuhp_remove:
	cpuhp_remove_state(cla_cpuhp_state);
err_driver_unregister:
	platform_driver_unregister(&cla_driver);
err_domains_free:
	cla_domains_free();
	return ret;
}

static void __exit cla_module_exit(void)
{
	cla_user_exit();
	cpuhp_remove_state(cla_cpuhp_state);
	platform_driver_unregister(&cla_driver);
	cla_domains_free();
}

module_init(cla_module_init);
module_exit(cla_module_exit);

MODULE_DESCRIPTION("Arm Core Local Accelerator");
MODULE_AUTHOR("Arm Limited");
MODULE_LICENSE("GPL");
