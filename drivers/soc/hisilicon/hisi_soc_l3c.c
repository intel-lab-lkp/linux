// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for HiSilicon L3 cache.
 *
 * Copyright (c) 2024 HiSilicon Technologies Co., Ltd.
 * Author: Yushan Wang <wangyushan12@huawei.com>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/acpi.h>
#include <linux/cleanup.h>
#include <linux/cpuhotplug.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/spinlock_types.h>
#include <linux/types.h>
#include <linux/xarray.h>

#include "hisi_soc_cache_framework.h"

#define HISI_L3C_LOCK_CTRL	0x0530
#define HISI_L3C_LOCK_AREA	0x0534
#define HISI_L3C_LOCK_START_L	0x0538
#define HISI_L3C_LOCK_START_H	0x053C

#define HISI_L3C_LOCK_CTRL_POLL_GAP_US		10
#define HISI_L3C_LOCK_CTRL_POLL_TIMEOUT_US	50000

#define HISI_L3C_MAX_LOCKREGION_SIZE	\
	"hisilicon,l3c-max-single-lockregion-size"
#define HISI_L3C_MAX_LOCKREGION_NUM	\
	"hisilicon,l3c-lockregion-num"

/* L3C control register bit definition */
#define HISI_L3C_LOCK_CTRL_LOCK_EN		BIT(0)
#define HISI_L3C_LOCK_CTRL_LOCK_DONE		BIT(1)
#define HISI_L3C_LOCK_CTRL_UNLOCK_EN		BIT(2)
#define HISI_L3C_LOCK_CTRL_UNLOCK_DONE		BIT(3)

#define l3c_lock_reg_offset(reg, set)	((reg) + 16 * (set))

#define l3c_lock_ctrl_mask(lock_ctrl, mask) ((lock_ctrl) & (mask))

#define to_hisi_l3c(p) container_of((p), struct hisi_soc_l3c, comp)

static int hisi_l3c_cpuhp_state;

struct hisi_soc_l3c {
	struct hisi_soc_comp comp;
	cpumask_t associated_cpus;

	/* Stores the first address locked by each register sets. */
	struct xarray lock_sets;
	/* Stores if a set of lock control register has been used. */
	u32 reg_used_map;
	/* Locks reg_used_map and lock_sets to forbid overlapping access. */
	spinlock_t reg_lock;

	/* Maximum locked memory size for a single lock region. */
	unsigned int max_single_lock_size;
	/* Maximum number of locked memory size. */
	int max_lock_num;

	struct hlist_node node;
	void __iomem *base;

	/* ID of Super CPU cluster on where the L3 cache locates. */
	int sccl_id;
	/* ID of CPU cluster where L3 cache is located. */
	int ccl_id;
};

/**
 * hisi_soc_l3c_alloc_lock_reg_set - Allocate an available control register set
 *				     of L3 cache for lock & unlock operations.
 * @soc_l3c:	The L3C instance on which the register set will be allocated.
 * @addr:	The address to be locked.
 *
 * @return:
 *   - -EBUSY: If there is no available register sets.
 *   - 0: If allocation succeeds.
 *
 * Maintains the resource of control registers of L3 cache.  On allocation,
 * the index of a spare set of registers is returned, then the address is
 * stored inside for future match of unlock operation.
 */
static int hisi_soc_l3c_alloc_lock_reg_set(struct hisi_soc_l3c *soc_l3c,
					   phys_addr_t addr)
{
	unsigned long idx;
	void *entry;

	for (idx = 0; idx < soc_l3c->max_lock_num; ++idx) {
		entry = xa_load(&soc_l3c->lock_sets, idx);
		if (!entry)
			break;
	}

	if (idx >= soc_l3c->max_lock_num)
		return -EBUSY;

	xa_store(&soc_l3c->lock_sets, idx, xa_mk_value(addr), GFP_KERNEL);
	return idx;
}

/**
 * hisi_soc_l3c_get_locked_reg_set - Get the index of an allocated register set
 *				     by locked address.
 * @soc_l3c:	The L3C instance on which the register set is allocated.
 * @addr:	The locked address.
 *
 * @return:
 *   - >= 0: index of register set which controls locked memory region of @addr.
 *   - -EINVAL: If @addr is not locked in this cache.
 */
static int hisi_soc_l3c_get_locked_reg_set(struct hisi_soc_l3c *soc_l3c,
					   phys_addr_t addr)
{
	void *entry;
	unsigned long idx;

	xa_for_each_range(&soc_l3c->lock_sets, idx, entry, 0,
			  soc_l3c->max_lock_num) {
		if (xa_to_value(entry) == addr)
			return idx;
	}
	return -EINVAL;
}

/**
 * hisi_soc_l3c_free_lock_reg_set - Free an allocated register set by locked
 *				    address.
 *
 * @soc_l3c:	The L3C instance on which the register set is allocated.
 * @addr:	The locked address.
 */
static void hisi_soc_l3c_free_lock_reg_set(struct hisi_soc_l3c *soc_l3c,
					   int regset)
{
	if (regset < 0)
		return;

	xa_erase(&soc_l3c->lock_sets, regset);
}

static int hisi_l3c_lock_ctrl_wait_finished(struct hisi_soc_l3c *soc_l3c,
					    int regset, u32 mask)
{
	u32 reg_used_map = soc_l3c->reg_used_map;
	void *base = soc_l3c->base;
	u32 val;

	/*
	 * Each HiSilicon L3 cache instance will have lock/unlock done bit set
	 * to 0 when first put to use even if the device is available.
	 * A reg_used_map is proposed to record if an instance has been called
	 * to lock down, then we can determine if it is available by
	 * reading lock/unlock done bit.
	 */
	if (!(reg_used_map & BIT(regset))) {
		reg_used_map |= BIT(regset);
		return 1;
	}

	return !readl_poll_timeout_atomic(
			base + l3c_lock_reg_offset(HISI_L3C_LOCK_CTRL, regset),
			val, l3c_lock_ctrl_mask(val, mask),
			HISI_L3C_LOCK_CTRL_POLL_GAP_US,
			HISI_L3C_LOCK_CTRL_POLL_TIMEOUT_US);
}

static int hisi_soc_l3c_do_lock(struct hisi_soc_comp *l3c_comp,
				       phys_addr_t addr, size_t size)
{
	struct hisi_soc_l3c *soc_l3c = to_hisi_l3c(l3c_comp);
	void *base = soc_l3c->base;
	int regset;
	u32 ctrl;

	if (size > soc_l3c->max_single_lock_size)
		return -EINVAL;

	guard(spinlock)(&soc_l3c->reg_lock);

	regset = hisi_soc_l3c_alloc_lock_reg_set(soc_l3c, addr);
	if (regset < 0)
		return -EBUSY;

	if (!hisi_l3c_lock_ctrl_wait_finished(soc_l3c, regset,
					      HISI_L3C_LOCK_CTRL_LOCK_DONE)) {
		hisi_soc_l3c_free_lock_reg_set(soc_l3c, regset);
		return -EBUSY;
	}

	writel(lower_32_bits(addr),
	       base + l3c_lock_reg_offset(HISI_L3C_LOCK_START_L, regset));
	writel(upper_32_bits(addr),
	       base + l3c_lock_reg_offset(HISI_L3C_LOCK_START_H, regset));
	writel(size, base + l3c_lock_reg_offset(HISI_L3C_LOCK_AREA, regset));

	ctrl = readl(base + l3c_lock_reg_offset(HISI_L3C_LOCK_CTRL, regset));
	ctrl = (ctrl | HISI_L3C_LOCK_CTRL_LOCK_EN) &
		~HISI_L3C_LOCK_CTRL_UNLOCK_EN;
	writel(ctrl, base + l3c_lock_reg_offset(HISI_L3C_LOCK_CTRL, regset));

	return 0;
}

static int hisi_soc_l3c_poll_lock_done(struct hisi_soc_comp *l3c_comp,
				   phys_addr_t addr, size_t size)
{
	struct hisi_soc_l3c *soc_l3c = to_hisi_l3c(l3c_comp);
	int regset;

	guard(spinlock)(&soc_l3c->reg_lock);

	regset = hisi_soc_l3c_get_locked_reg_set(soc_l3c, addr);
	if (regset < 0)
		return -EINVAL;

	if (!hisi_l3c_lock_ctrl_wait_finished(soc_l3c, regset,
					      HISI_L3C_LOCK_CTRL_LOCK_DONE))
		return -ETIMEDOUT;

	return 0;
}

static int hisi_soc_l3c_do_unlock(struct hisi_soc_comp *l3c_comp,
				  phys_addr_t addr)
{
	struct hisi_soc_l3c *soc_l3c = to_hisi_l3c(l3c_comp);
	void *base = soc_l3c->base;
	int regset;
	u32 ctrl;

	guard(spinlock)(&soc_l3c->reg_lock);

	regset = hisi_soc_l3c_get_locked_reg_set(soc_l3c, addr);
	if (regset < 0)
		return -EINVAL;

	if (!hisi_l3c_lock_ctrl_wait_finished(soc_l3c, regset,
					      HISI_L3C_LOCK_CTRL_UNLOCK_DONE))
		return -EBUSY;

	ctrl = readl(base + l3c_lock_reg_offset(HISI_L3C_LOCK_CTRL, regset));
	ctrl = (ctrl | HISI_L3C_LOCK_CTRL_UNLOCK_EN) &
		~HISI_L3C_LOCK_CTRL_LOCK_EN;
	writel(ctrl, base + l3c_lock_reg_offset(HISI_L3C_LOCK_CTRL, regset));

	return 0;
}

static int hisi_soc_l3c_poll_unlock_done(struct hisi_soc_comp *l3c_comp,
					 phys_addr_t addr)
{
	struct hisi_soc_l3c *soc_l3c = to_hisi_l3c(l3c_comp);
	int regset;

	guard(spinlock)(&soc_l3c->reg_lock);

	regset = hisi_soc_l3c_get_locked_reg_set(soc_l3c, addr);
	if (regset < 0)
		return -EINVAL;

	if (!hisi_l3c_lock_ctrl_wait_finished(soc_l3c, regset,
					      HISI_L3C_LOCK_CTRL_UNLOCK_DONE))
		return -ETIMEDOUT;

	hisi_soc_l3c_free_lock_reg_set(soc_l3c, regset);

	return 0;
}

/**
 * hisi_soc_l3c_remove_locks - Remove all cache locks when the driver exits.
 *
 * @soc_l3c:	The L3C instance on which the cache locks should be removed.
 */
static void hisi_soc_l3c_remove_locks(struct hisi_soc_l3c *soc_l3c)
{

	void *base = soc_l3c->base;
	unsigned long regset;
	int timeout;
	void *entry;
	u32 ctrl;

	guard(spinlock)(&soc_l3c->reg_lock);

	xa_for_each(&soc_l3c->lock_sets, regset, entry) {
		timeout = hisi_l3c_lock_ctrl_wait_finished(soc_l3c, regset,
						HISI_L3C_LOCK_CTRL_UNLOCK_DONE);

		ctrl = readl(base + l3c_lock_reg_offset(HISI_L3C_LOCK_CTRL,
							regset));
		ctrl = (ctrl | HISI_L3C_LOCK_CTRL_UNLOCK_EN) &
			~HISI_L3C_LOCK_CTRL_LOCK_EN;
		writel(ctrl, base + l3c_lock_reg_offset(HISI_L3C_LOCK_CTRL,
							regset));

		timeout = hisi_l3c_lock_ctrl_wait_finished(soc_l3c, regset,
						HISI_L3C_LOCK_CTRL_UNLOCK_DONE);

		/*
		 * If cache lock remove fails, inform user since the removal of
		 * driver cannot fail.
		 */
		if (timeout)
			pr_err("failed to remove %ldth cache lock.\n", regset);
	}
}

static int hisi_soc_l3c_init_lock_capacity(struct hisi_soc_l3c *soc_l3c,
					   struct device *dev)
{
	if (device_property_read_u32(dev, HISI_L3C_MAX_LOCKREGION_SIZE,
				     &soc_l3c->max_single_lock_size)
	    || soc_l3c->max_single_lock_size <= 0)
		return -EINVAL;

	if (device_property_read_u32(dev, HISI_L3C_MAX_LOCKREGION_NUM,
				     &soc_l3c->max_lock_num)
	    || soc_l3c->max_lock_num <= 0)
		return -EINVAL;

	return 0;
}

static int hisi_soc_l3c_init_topology(struct hisi_soc_l3c *soc_l3c,
				      struct device *dev)
{
	soc_l3c->sccl_id = -1;
	soc_l3c->ccl_id = -1;

	if (device_property_read_u32(dev, "hisilicon,scl-id", &soc_l3c->sccl_id)
	    || soc_l3c->sccl_id < 0)
		return -EINVAL;

	if (device_property_read_u32(dev, "hisilicon,ccl-id", &soc_l3c->ccl_id)
	    || soc_l3c->ccl_id < 0)
		return -EINVAL;

	return 0;
}

static void hisi_init_associated_cpus(struct hisi_soc_l3c *soc_l3c)
{
	if (!cpumask_empty(&soc_l3c->associated_cpus))
		return;
	cpumask_clear(&soc_l3c->associated_cpus);
	cpumask_copy(&soc_l3c->comp.affinity_mask, &soc_l3c->associated_cpus);
}

static struct hisi_soc_comp_ops hisi_soc_l3c_comp_ops = {
	.do_lock = hisi_soc_l3c_do_lock,
	.poll_lock_done = hisi_soc_l3c_poll_lock_done,
	.do_unlock = hisi_soc_l3c_do_unlock,
	.poll_unlock_done = hisi_soc_l3c_poll_unlock_done,
};

static struct hisi_soc_comp hisi_soc_l3c_comp = {
	.comp_type = BIT(HISI_SOC_L3C),
	.ops = &hisi_soc_l3c_comp_ops,
};

static int hisi_soc_l3c_probe(struct platform_device *pdev)
{
	struct hisi_soc_l3c *soc_l3c;
	struct resource *mem;
	int ret = 0;

	soc_l3c = devm_kzalloc(&pdev->dev, sizeof(*soc_l3c), GFP_KERNEL);
	if (!soc_l3c)
		return -ENOMEM;

	platform_set_drvdata(pdev, soc_l3c);

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!mem)
		return -ENODEV;

	/*
	 * L3C cache driver share the same register region with L3C uncore PMU
	 * driver in hardware's perspective, none of them should reserve the
	 * resource to itself only.  Here exclusive access verification is
	 * avoided by calling devm_ioremap instead of devm_ioremap_resource to
	 * allow both drivers to exist at the same time.
	 */
	soc_l3c->base = devm_ioremap(&pdev->dev, mem->start,
				     resource_size(mem));
	if (IS_ERR_OR_NULL(soc_l3c->base))
		return PTR_ERR(soc_l3c->base);

	soc_l3c->comp = hisi_soc_l3c_comp;
	spin_lock_init(&soc_l3c->reg_lock);
	xa_init(&soc_l3c->lock_sets);

	ret = hisi_soc_l3c_init_lock_capacity(soc_l3c, &pdev->dev);
	if (ret)
		goto err_xa;

	hisi_init_associated_cpus(soc_l3c);

	ret = hisi_soc_l3c_init_topology(soc_l3c, &pdev->dev);
	if (ret)
		goto err_xa;

	ret = cpuhp_state_add_instance(hisi_l3c_cpuhp_state, &soc_l3c->node);
	if (ret)
		goto err_xa;

	ret = hisi_soc_comp_inst_add(&soc_l3c->comp);
	if (ret)
		goto err_hotplug;

	return ret;

err_hotplug:
	cpuhp_state_remove_instance_nocalls(hisi_l3c_cpuhp_state,
					    &soc_l3c->node);

err_xa:
	xa_destroy(&soc_l3c->lock_sets);
	return ret;
}

static void hisi_soc_l3c_remove(struct platform_device *pdev)
{
	struct hisi_soc_l3c *soc_l3c = platform_get_drvdata(pdev);
	unsigned long idx;
	void *entry;

	hisi_soc_l3c_remove_locks(soc_l3c);

	hisi_soc_comp_inst_del(&soc_l3c->comp);

	cpuhp_state_remove_instance_nocalls(hisi_l3c_cpuhp_state,
					    &soc_l3c->node);

	xa_for_each(&soc_l3c->lock_sets, idx, entry)
		xa_erase(&soc_l3c->lock_sets, idx);

	xa_destroy(&soc_l3c->lock_sets);
}

static void hisi_read_sccl_and_ccl_id(int *scclp, int *cclp)
{
	u64 mpidr = read_cpuid_mpidr();
	int aff3 = MPIDR_AFFINITY_LEVEL(mpidr, 3);
	int aff2 = MPIDR_AFFINITY_LEVEL(mpidr, 2);
	int aff1 = MPIDR_AFFINITY_LEVEL(mpidr, 1);
	int sccl, ccl;

	if (mpidr & MPIDR_MT_BITMASK) {
		sccl = aff3;
		ccl = aff2;
	} else {
		sccl = aff2;
		ccl = aff1;
	}

	*scclp = sccl;
	*cclp = ccl;
}

static bool hisi_soc_l3c_is_associated(struct hisi_soc_l3c *soc_l3c)
{
	int sccl_id, ccl_id;

	hisi_read_sccl_and_ccl_id(&sccl_id, &ccl_id);
	return sccl_id == soc_l3c->sccl_id && ccl_id == soc_l3c->ccl_id;
}

static int hisi_soc_l3c_online_cpu(unsigned int cpu, struct hlist_node *node)
{
	struct hisi_soc_l3c *soc_l3c =
		hlist_entry_safe(node, struct hisi_soc_l3c, node);

	if (!cpumask_test_cpu(cpu, &soc_l3c->associated_cpus)) {
		if (!(hisi_soc_l3c_is_associated(soc_l3c)))
			return 0;

		cpumask_set_cpu(cpu, &soc_l3c->associated_cpus);
		cpumask_copy(&soc_l3c->comp.affinity_mask,
			     &soc_l3c->associated_cpus);
	}
	return 0;
}

static const struct acpi_device_id hisi_l3c_acpi_match[] = {
	{ "HISI0501", },
	{ }
};
MODULE_DEVICE_TABLE(acpi, hisi_l3c_acpi_match);

static struct platform_driver hisi_soc_l3c_driver = {
	.driver = {
		.name = "hisi_soc_l3c",
		.acpi_match_table = hisi_l3c_acpi_match,
	},
	.probe = hisi_soc_l3c_probe,
	.remove = hisi_soc_l3c_remove,
};

static int __init hisi_soc_l3c_init(void)
{
	int ret;

	ret = cpuhp_setup_state_multi(CPUHP_AP_ONLINE_DYN, "hisi_soc_l3c",
				      hisi_soc_l3c_online_cpu, NULL);
	if (ret < 0)
		return ret;
	hisi_l3c_cpuhp_state = ret;

	ret = platform_driver_register(&hisi_soc_l3c_driver);
	if (ret)
		cpuhp_remove_multi_state(CPUHP_AP_ONLINE_DYN);

	return ret;
}
module_init(hisi_soc_l3c_init);

static void __exit hisi_soc_l3c_exit(void)
{
	platform_driver_unregister(&hisi_soc_l3c_driver);
	cpuhp_remove_multi_state(CPUHP_AP_ONLINE_DYN);
}
module_exit(hisi_soc_l3c_exit);

MODULE_DESCRIPTION("Driver supporting cache lockdown for Hisilicon L3 cache");
MODULE_AUTHOR("Yushan Wang <wangyushan12@huawei.com>");
MODULE_LICENSE("GPL");
