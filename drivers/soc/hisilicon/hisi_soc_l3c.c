// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for HiSilicon L3 cache.
 *
 * Copyright (c) 2025 HiSilicon Technologies Co., Ltd.
 * Author: Yushan Wang <wangyushan12@huawei.com>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/acpi.h>
#include <linux/cleanup.h>
#include <linux/cpuhotplug.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mm.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/spinlock_types.h>
#include <linux/types.h>
#include <linux/xarray.h>

#include <asm/cputype.h>

#include <uapi/misc/hisi_l3c.h>

#define HISI_L3C_LOCK_CTRL	0x0530
#define HISI_L3C_LOCK_AREA	0x0534
#define HISI_L3C_LOCK_START_L	0x0538
#define HISI_L3C_LOCK_START_H	0x053C

#define HISI_L3C_DYNAMIC_AUCTRL	0x0404

#define HISI_L3C_LOCK_CTRL_POLL_GAP_US	10
#define HISI_L3C_LOCK_CTRL_POLL_MAX_US	10000

/* L3C control register bit definition */
#define HISI_L3C_LOCK_CTRL_LOCK_EN		BIT(0)
#define HISI_L3C_LOCK_CTRL_LOCK_DONE		BIT(1)
#define HISI_L3C_LOCK_CTRL_UNLOCK_EN		BIT(2)
#define HISI_L3C_LOCK_CTRL_UNLOCK_DONE		BIT(3)

#define HISI_L3C_LOCK_MIN_SIZE		(1 * 1024 * 1024)
#define HISI_L3_CACHE_LINE_SIZE		64

/* Allow maximum 70% of cache locked. */
#define HISI_L3C_MAX_LOCK_SIZE(size)	((size) / 10 * 7)

#define l3c_lock_reg_offset(reg, set)	((reg) + 16 * (set))

#define to_hisi_l3c(p) container_of((p), struct hisi_l3c, comp)

/**
 * struct hisi_soc_comp - Struct of HiSilicon SoC cache components.
 *
 * @node: list node of hisi_soc_comp_list.
 * @ops: possible operations a component may perform.
 * @affinity_mask: cpus that associate with this component.
 * @private: component specific data.
 */
struct hisi_soc_comp {
	struct list_head node;
	struct hisi_soc_comp_ops *ops;
	cpumask_t affinity_mask;
	void *private;
};

/**
 * struct hisi_soc_comp_ops - Callbacks for SoC cache drivers to handle
 *			      operation requests.
 *
 * @do_lock: lock certain region of L3 cache from being evicted.
 * @poll_lock_done: check if the lock operation has succeeded.
 * @do_unlock: unlock the locked region of L3 cache back to normal.
 * @poll_unlock_done: check if the unlock operation has succeeded.
	      operation requests.
 *
 * Operations are decoupled into two phases so that framework does not have
 * to wait for one operation to finish before calling the next when multiple
 * hardwares onboard.
 *
 * Implementers must implement the functions in pairs.  Implementation should
 * return -EBUSY when:
 * - insufficient resources are available to perform the operation.
 * - previously raised operation is not finished.
 * - new operations (do_lock(), do_unlock() etc.) to the same address
 *   before corresponding done functions being called.
 */
struct hisi_soc_comp_ops {
	int (*do_lock)(struct hisi_soc_comp *comp, phys_addr_t addr, size_t size);
	int (*poll_lock_done)(struct hisi_soc_comp *comp, phys_addr_t addr, size_t size);
	int (*do_unlock)(struct hisi_soc_comp *comp, phys_addr_t addr);
	int (*poll_unlock_done)(struct hisi_soc_comp *comp, phys_addr_t addr);
};

struct hisi_l3c_lock_region {
	/* physical address of the arena allocated for aligned address */
	unsigned long arena_start;
	/* VMA region of locked memory for future release */
	unsigned long vm_start;
	unsigned long vm_end;
	phys_addr_t addr;
	size_t size;
	/* Return value of cache lock call */
	int status;
	int cpu;
};

struct hisi_soc_comp_list {
	struct list_head node;
	/* protects list of HiSilicon SoC cache components */
	spinlock_t lock;
};

struct hisi_l3c {
	struct hisi_soc_comp comp;
	cpumask_t associated_cpus;

	/* Stores the first address locked by each register sets. */
	struct xarray lock_sets;
	/* Locks lock_sets to forbid overlapping access. */
	spinlock_t reg_lock;

	struct hlist_node node;
	void __iomem *base;

	/* ID of Super CPU cluster on where the L3 cache locates. */
	int sccl_id;
	/* ID of CPU cluster where L3 cache is located. */
	int ccl_id;
};

static int hisi_l3c_cpuhp_state;

static struct hisi_soc_comp_list l3c_devs;

/**
 * hisi_l3c_alloc_lock_reg_set - Allocate an available control register set
 *				     of L3 cache for lock & unlock operations.
 * @l3c:	The L3C instance on which the register set will be allocated.
 * @addr:	The address to be locked.
 * @size:	The size to be locked.
 *
 * @return:
 *   - -EBUSY: If there is no available register sets.
 *   - -ENOMEM: If there is no available memory for lock region struct.
 *   - -EINVAL: If there is no available cache size for lock.
 *   - 0: If allocation succeeds.
 *
 * Maintains the resource of control registers of L3 cache.  On allocation,
 * the index of a spare set of registers is returned, then the address is
 * stored inside for future match of unlock operation.
 */
static int hisi_l3c_alloc_lock_reg_set(struct hisi_l3c *l3c, phys_addr_t addr, size_t size)
{
	struct hisi_l3c_lock_info *info = l3c->comp.private;
	struct hisi_l3c_lock_region *lr;
	void *entry;
	int idx, ret;

	if (size > info->lock_size)
		return -EINVAL;

	for (idx = 0; idx < info->lock_region_num; ++idx) {
		entry = xa_load(&l3c->lock_sets, idx);
		if (!entry)
			break;
	}

	if (idx > info->lock_region_num)
		return -EBUSY;

	lr = kzalloc(sizeof(*lr), GFP_KERNEL);
	if (!lr)
		return -ENOMEM;

	lr->addr = addr;
	lr->size = size;

	ret = xa_alloc(&l3c->lock_sets, &idx, lr, xa_limit_31b, GFP_KERNEL);
	if (ret) {
		kfree(lr);
		return ret;
	}

	info->lock_size -= size;
	info->lock_region_num -= 1;

	return idx;
}

/**
 * hisi_l3c_get_locked_reg_set - Get the index of an allocated register set
 *				     by locked address.
 * @l3c:	The L3C instance on which the register set is allocated.
 * @addr:	The locked address.
 *
 * @return:
 *   - >= 0: index of register set which controls locked memory region of @addr.
 *   - -EINVAL: If @addr is not locked in this cache.
 */
static int hisi_l3c_get_locked_reg_set(struct hisi_l3c *l3c, phys_addr_t addr)
{
	struct hisi_l3c_lock_region *entry;
	unsigned long idx;

	xa_for_each(&l3c->lock_sets, idx, entry) {
		if (entry->addr == addr)
			return idx;
	}
	return -EINVAL;
}

/**
 * hisi_l3c_free_lock_reg_set - Free an allocated register set by locked
 *				    address.
 *
 * @l3c:	The L3C instance on which the register set is allocated.
 * @regset:	ID of Register set to be freed.
 */
static void hisi_l3c_free_lock_reg_set(struct hisi_l3c *l3c, int regset)
{
	struct hisi_l3c_lock_info *info = l3c->comp.private;
	struct hisi_l3c_lock_region *entry;

	if (regset < 0)
		return;

	entry = xa_erase(&l3c->lock_sets, regset);
	if (!entry)
		return;

	info->lock_size += entry->size;
	info->lock_region_num += 1;
	kfree(entry);
}

static bool hisi_l3c_lock_wait_finished(struct hisi_l3c *l3c, int regset)
{
	void *reg = l3c->base + l3c_lock_reg_offset(HISI_L3C_LOCK_CTRL, regset);
	/* Wait until neither lock or unlock operation is going on. */
	u32 mask = HISI_L3C_LOCK_CTRL_LOCK_DONE | HISI_L3C_LOCK_CTRL_UNLOCK_DONE;
	u32 val;

	/*
	 * Lock/unlock done bits are initially 0 if no lock operation was ever
	 * issued, and will be set until next operation comes.
	 * Check if this is the first lock operation after boot by checking if
	 * the register is 0. If so, proceed with the operation.
	 */
	val = readl(reg);
	if (!val)
		return true;

	return !readl_poll_timeout_atomic(reg, val, val & mask,
			HISI_L3C_LOCK_CTRL_POLL_GAP_US,
			HISI_L3C_LOCK_CTRL_POLL_MAX_US);
}

static int hisi_l3c_do_lock(struct hisi_soc_comp *comp, phys_addr_t addr, size_t size)
{
	struct hisi_l3c *l3c = to_hisi_l3c(comp);
	struct hisi_l3c_lock_info *info = l3c->comp.private;
	void *base = l3c->base;
	int regset;
	u32 ctrl;

	if (info->address_alignment && addr % size != 0)
		return -EINVAL;

	if (size < info->min_lock_size || size > info->max_lock_size)
		return -EINVAL;

	guard(spinlock)(&l3c->reg_lock);

	regset = hisi_l3c_alloc_lock_reg_set(l3c, addr, size);
	if (regset < 0)
		return -EBUSY;

	if (!hisi_l3c_lock_wait_finished(l3c, regset)) {
		hisi_l3c_free_lock_reg_set(l3c, regset);
		return -EBUSY;
	}

	writel(lower_32_bits(addr),
	       base + l3c_lock_reg_offset(HISI_L3C_LOCK_START_L, regset));
	writel(upper_32_bits(addr),
	       base + l3c_lock_reg_offset(HISI_L3C_LOCK_START_H, regset));
	writel(size, base + l3c_lock_reg_offset(HISI_L3C_LOCK_AREA, regset));

	ctrl = readl(base + HISI_L3C_DYNAMIC_AUCTRL);
	ctrl |= BIT(regset);
	writel(ctrl, base + HISI_L3C_DYNAMIC_AUCTRL);

	ctrl = readl(base + l3c_lock_reg_offset(HISI_L3C_LOCK_CTRL, regset));
	ctrl = (ctrl | HISI_L3C_LOCK_CTRL_LOCK_EN) &
		~HISI_L3C_LOCK_CTRL_UNLOCK_EN;
	writel(ctrl, base + l3c_lock_reg_offset(HISI_L3C_LOCK_CTRL, regset));

	return 0;
}

static int hisi_l3c_poll_lock_done(struct hisi_soc_comp *comp, phys_addr_t addr, size_t size)
{
	struct hisi_l3c *l3c = to_hisi_l3c(comp);
	int regset;

	guard(spinlock)(&l3c->reg_lock);

	regset = hisi_l3c_get_locked_reg_set(l3c, addr);
	if (regset < 0)
		return -EINVAL;

	if (!hisi_l3c_lock_wait_finished(l3c, regset))
		return -ETIMEDOUT;

	return 0;
}

static int hisi_l3c_do_unlock(struct hisi_soc_comp *comp, phys_addr_t addr)
{
	struct hisi_l3c *l3c = to_hisi_l3c(comp);
	void *base = l3c->base;
	int regset;
	u32 ctrl;

	guard(spinlock)(&l3c->reg_lock);

	regset = hisi_l3c_get_locked_reg_set(l3c, addr);
	if (regset < 0)
		return -EINVAL;

	if (!hisi_l3c_lock_wait_finished(l3c, regset))
		return -EBUSY;

	ctrl = readl(base + HISI_L3C_DYNAMIC_AUCTRL);
	ctrl &= ~BIT(regset);
	writel(ctrl, base + HISI_L3C_DYNAMIC_AUCTRL);

	ctrl = readl(base + l3c_lock_reg_offset(HISI_L3C_LOCK_CTRL, regset));
	ctrl = (ctrl | HISI_L3C_LOCK_CTRL_UNLOCK_EN) &
		~HISI_L3C_LOCK_CTRL_LOCK_EN;
	writel(ctrl, base + l3c_lock_reg_offset(HISI_L3C_LOCK_CTRL, regset));

	return 0;
}

static int hisi_l3c_poll_unlock_done(struct hisi_soc_comp *comp, phys_addr_t addr)
{
	struct hisi_l3c *l3c = to_hisi_l3c(comp);
	int regset;

	guard(spinlock)(&l3c->reg_lock);

	regset = hisi_l3c_get_locked_reg_set(l3c, addr);
	if (regset < 0)
		return -EINVAL;

	if (!hisi_l3c_lock_wait_finished(l3c, regset))
		return -ETIMEDOUT;

	hisi_l3c_free_lock_reg_set(l3c, regset);

	return 0;
}

static void hisi_l3c_remove_locks(struct hisi_l3c *l3c)
{
	void *base = l3c->base;
	unsigned long regset;
	void *entry;

	guard(spinlock)(&l3c->reg_lock);

	xa_for_each(&l3c->lock_sets, regset, entry) {
		int timeout;
		u32 ctrl;

		ctrl = readl(base + l3c_lock_reg_offset(HISI_L3C_LOCK_CTRL, regset));
		ctrl = (ctrl | HISI_L3C_LOCK_CTRL_UNLOCK_EN) & ~HISI_L3C_LOCK_CTRL_LOCK_EN;
		writel(ctrl, base + l3c_lock_reg_offset(HISI_L3C_LOCK_CTRL, regset));

		timeout = hisi_l3c_lock_wait_finished(l3c, regset);
		if (timeout)
			pr_err("failed to remove %lu-th cache lock.\n", regset);
	}
}

static int hisi_l3c_lock(int cpu, phys_addr_t addr, size_t size)
{
	struct hisi_soc_comp *comp;
	int ret;

	guard(spinlock)(&l3c_devs.lock);

	/* When there is no instance onboard, no locked memory is available. */
	if (list_empty(&l3c_devs.node))
		return -ENOMEM;

	/* Lock need to be performed on each channel of associated L3 cache. */
	list_for_each_entry(comp, &l3c_devs.node, node) {
		if (!cpumask_test_cpu(cpu, &comp->affinity_mask))
			continue;
		ret = comp->ops->do_lock(comp, addr, size);
		if (ret)
			return ret;
	}

	list_for_each_entry(comp, &l3c_devs.node, node) {
		if (!cpumask_test_cpu(cpu, &comp->affinity_mask))
			continue;
		ret = comp->ops->poll_lock_done(comp, addr, size);
		if (ret)
			return ret;
	}

	return 0;
}

static int hisi_l3c_unlock(int cpu, phys_addr_t addr)
{
	struct hisi_soc_comp *comp;
	int ret;

	guard(spinlock)(&l3c_devs.lock);

	if (list_empty(&l3c_devs.node))
		return -EINVAL;

	/* Perform unlock on each channel of associated L3 cache. */
	list_for_each_entry(comp, &l3c_devs.node, node) {
		if (!cpumask_test_cpu(cpu, &comp->affinity_mask))
			continue;
		ret = comp->ops->do_unlock(comp, addr);
		if (ret)
			return ret;
	}

	list_for_each_entry(comp, &l3c_devs.node, node) {
		if (!cpumask_test_cpu(cpu, &comp->affinity_mask))
			continue;
		ret = comp->ops->poll_unlock_done(comp, addr);
		if (ret)
			return ret;
	}

	return 0;
}

static void hisi_soc_comp_add(struct hisi_soc_comp *comp)
{
	guard(spinlock)(&l3c_devs.lock);
	list_add_tail(&comp->node, &l3c_devs.node);
}

/* Null @comp means to delete all instances. */
static int hisi_soc_comp_del(struct hisi_soc_comp *comp)
{
	struct hisi_soc_comp *entry, *tmp;

	guard(spinlock)(&l3c_devs.lock);
	list_for_each_entry_safe(entry, tmp, &l3c_devs.node, node) {
		if (comp && comp != entry)
			continue;

		list_del(&entry->node);

		/* Only continue to delete nodes when @comp is NULL */
		if (comp)
			break;
	}

	return 0;
}

static void hisi_l3c_vm_open(struct vm_area_struct *vma)
{
	struct hisi_l3c_lock_region *clr = vma->vm_private_data;

	/*
	 * Only perform cache lock when the vma passed in is created in
	 * hisi_l3c_mmap.
	 */
	if (clr->vm_start != vma->vm_start || clr->vm_end != vma->vm_end)
		return;

	clr->status = hisi_l3c_lock(clr->cpu, clr->addr, clr->size);
}

static void hisi_l3c_vm_close(struct vm_area_struct *vma)
{
	struct hisi_l3c_lock_region *clr = vma->vm_private_data;
	int order = get_order(clr->size);

	/*
	 * Only perform cache unlock when the vma passed in is created
	 * in hisi_l3c_mmap.
	 */
	if (clr->vm_start != vma->vm_start || clr->vm_end != vma->vm_end)
		return;

	hisi_l3c_unlock(clr->cpu, clr->addr);

	free_contig_range(PHYS_PFN(clr->addr), 1 << order);
	kfree(clr);
	vma->vm_private_data = NULL;
}

/* mremap operation is not supported for HiSilicon SoC cache. */
static int hisi_l3c_vm_mremap(struct vm_area_struct *vma)
{
	struct hisi_l3c_lock_region *clr = vma->vm_private_data;

	/*
	 * vma region size will be changed as requested by mremap despite the
	 * callback failure in this function.  Thus, change the vma region
	 * stored in clr according to the parameters to verify if the pages
	 * should be freed when unmapping.
	 */
	clr->vm_end = clr->vm_start + (vma->vm_end - vma->vm_start);
	pr_err("mremap for HiSilicon SoC locked cache is not supported\n");

	return -EOPNOTSUPP;
}

static int hisi_l3c_may_split(struct vm_area_struct *area, unsigned long addr)
{
	pr_err("HiSilicon SoC locked cache may not be split.\n");
	return -EINVAL;
}

static const struct vm_operations_struct hisi_l3c_vm_ops = {
	.open = hisi_l3c_vm_open,
	.close = hisi_l3c_vm_close,
	.may_split = hisi_l3c_may_split,
	.mremap = hisi_l3c_vm_mremap,
};

static int hisi_l3c_mmap(struct file *file, struct vm_area_struct *vma)
{
	unsigned long size = vma->vm_end - vma->vm_start;
	int order = get_order(size);
	unsigned long addr;
	struct page *pg;
	int ret;

	struct hisi_l3c_lock_region *clr __free(kfree) = kzalloc(sizeof(*clr), GFP_KERNEL);
	if (!clr)
		return -ENOMEM;

	/* Continuous physical memory is required for L3 cache lock. */
	pg = alloc_contig_pages(1 << order, GFP_KERNEL | __GFP_NOWARN | __GFP_ZERO,
				cpu_to_node(smp_processor_id()), NULL);
	if (!pg)
		return -ENOMEM;

	addr = page_to_phys(pg);
	*clr = (struct hisi_l3c_lock_region) {
		.addr = addr,
		.size = size,
		.cpu = smp_processor_id(),
		/* vma should not be moved, store here for validation */
		.vm_start = vma->vm_start,
		.vm_end = vma->vm_end,
	};

	vm_flags_set(vma, VM_DONTCOPY | VM_DONTEXPAND);
	vma->vm_ops = &hisi_l3c_vm_ops;
	vma->vm_private_data = clr;

	hisi_l3c_vm_ops.open(vma);
	if (clr->status) {
		ret = clr->status;
		goto out_page;
	}

	ret = remap_pfn_range(vma, vma->vm_start, PFN_DOWN(addr), size,
			      vma->vm_page_prot);
	if (ret)
		goto out_page;

	/* Save clr from being freed when lock succeeds. */
	vma->vm_private_data = no_free_ptr(clr);

	return 0;

out_page:
	free_contig_range(PHYS_PFN(clr->addr), 1 << order);
	return ret;
}

static int hisi_l3c_lock_restriction(unsigned long arg)
{
	void __user *uarg = (void __user *)arg;
	int cpu = smp_processor_id();
	struct hisi_soc_comp *comp;

	if (list_empty(&l3c_devs.node))
		return -ENODEV;

	list_for_each_entry(comp, &l3c_devs.node, node) {
		if (!cpumask_test_cpu(cpu, &comp->affinity_mask))
			continue;

		if (!comp->private)
			return -ENOENT;

		if (copy_to_user(uarg, comp->private, sizeof(struct hisi_l3c_lock_info)))
			return -EFAULT;

		return 0;
	}

	return -ENODEV;
}

static int hisi_l3c_init_lock_capacity(struct hisi_l3c *l3c, struct device *dev)
{
	int ret;
	u32 val;

	struct hisi_l3c_lock_info *info __free(kfree) = kmalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	ret = device_property_read_u32(dev, "hisilicon,l3c-lockregion-num", &val);
	if (ret || val <= 0)
		return -EINVAL;

	info->lock_region_num = val;

	ret = device_property_read_u32(dev, "hisilicon,l3c-max-single-lockregion-size", &val);
	if (ret || val <= 0)
		return -EINVAL;

	info->lock_size = HISI_L3C_MAX_LOCK_SIZE(val);
	info->address_alignment = info->lock_region_num == 1;
	info->max_lock_size = HISI_L3C_MAX_LOCK_SIZE(val);
	info->min_lock_size = info->lock_region_num == 1
					? HISI_L3C_LOCK_MIN_SIZE
					: HISI_L3_CACHE_LINE_SIZE;

	l3c->comp.private = no_free_ptr(info);

	return 0;
}

static int hisi_l3c_init_topology(struct hisi_l3c *l3c, struct device *dev)
{
	l3c->sccl_id = -1;
	l3c->ccl_id = -1;

	if (device_property_read_u32(dev, "hisilicon,scl-id", &l3c->sccl_id) ||
	    l3c->sccl_id < 0)
		return -EINVAL;

	if (device_property_read_u32(dev, "hisilicon,ccl-id", &l3c->ccl_id) ||
	    l3c->ccl_id < 0)
		return -EINVAL;

	return 0;
}

static void hisi_init_associated_cpus(struct hisi_l3c *l3c)
{
	if (!cpumask_empty(&l3c->associated_cpus))
		return;
	cpumask_clear(&l3c->associated_cpus);
	cpumask_copy(&l3c->comp.affinity_mask, &l3c->associated_cpus);
}

static struct hisi_soc_comp_ops hisi_comp_ops = {
	.do_lock = hisi_l3c_do_lock,
	.poll_lock_done = hisi_l3c_poll_lock_done,
	.do_unlock = hisi_l3c_do_unlock,
	.poll_unlock_done = hisi_l3c_poll_unlock_done,
};

static struct hisi_soc_comp hisi_comp = {
	.ops = &hisi_comp_ops,
};

static int hisi_l3c_probe(struct platform_device *pdev)
{
	struct hisi_l3c *l3c;
	struct resource *mem;
	int ret;

	l3c = devm_kzalloc(&pdev->dev, sizeof(*l3c), GFP_KERNEL);
	if (!l3c)
		return -ENOMEM;

	platform_set_drvdata(pdev, l3c);

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!mem)
		return -ENODEV;

	l3c->base = devm_ioremap(&pdev->dev, mem->start, resource_size(mem));
	if (IS_ERR_OR_NULL(l3c->base))
		return PTR_ERR(l3c->base);

	l3c->comp = hisi_comp;
	spin_lock_init(&l3c->reg_lock);
	xa_init_flags(&l3c->lock_sets, XA_FLAGS_ALLOC);

	ret = hisi_l3c_init_lock_capacity(l3c, &pdev->dev);
	if (ret)
		goto err_xa;

	hisi_init_associated_cpus(l3c);

	ret = hisi_l3c_init_topology(l3c, &pdev->dev);
	if (ret)
		goto err_xa;

	ret = cpuhp_state_add_instance(hisi_l3c_cpuhp_state, &l3c->node);
	if (ret)
		goto err_xa;

	hisi_soc_comp_add(&l3c->comp);

	return 0;

err_xa:
	xa_destroy(&l3c->lock_sets);
	return ret;
}

static void hisi_l3c_remove(struct platform_device *pdev)
{
	struct hisi_l3c *l3c = platform_get_drvdata(pdev);
	unsigned long idx;
	struct hisi_l3c_lock_region *entry;

	hisi_l3c_remove_locks(l3c);

	hisi_soc_comp_del(&l3c->comp);

	cpuhp_state_remove_instance_nocalls(hisi_l3c_cpuhp_state, &l3c->node);

	xa_for_each(&l3c->lock_sets, idx, entry)
		entry = xa_erase(&l3c->lock_sets, idx);

	xa_destroy(&l3c->lock_sets);
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

static bool hisi_l3c_is_associated(struct hisi_l3c *l3c)
{
	int sccl_id, ccl_id;

	hisi_read_sccl_and_ccl_id(&sccl_id, &ccl_id);

	return sccl_id == l3c->sccl_id && ccl_id == l3c->ccl_id;
}

static int hisi_l3c_online_cpu(unsigned int cpu, struct hlist_node *node)
{
	struct hisi_l3c *l3c = hlist_entry_safe(node, struct hisi_l3c, node);

	if (!cpumask_test_cpu(cpu, &l3c->associated_cpus)) {
		if (!(hisi_l3c_is_associated(l3c)))
			return 0;

		cpumask_set_cpu(cpu, &l3c->associated_cpus);
		cpumask_copy(&l3c->comp.affinity_mask,
			     &l3c->associated_cpus);
	}
	return 0;
}

static const struct acpi_device_id hisi_l3c_acpi_match[] = {
	{ "HISI0501", },
	{ }
};
MODULE_DEVICE_TABLE(acpi, hisi_l3c_acpi_match);

static struct platform_driver hisi_l3c_driver = {
	.driver = {
		.name = "hisi_l3c",
		.acpi_match_table = hisi_l3c_acpi_match,
	},
	.probe = hisi_l3c_probe,
	.remove = hisi_l3c_remove,
};

static long hisi_l3c_ioctl(struct file *file, u32 cmd, unsigned long arg)
{
	switch (cmd) {
	case HISI_L3C_LOCK_INFO:
		return hisi_l3c_lock_restriction(arg);
	default:
		return -EINVAL;
	}
}

static const struct file_operations l3c_dev_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = hisi_l3c_ioctl,
	.mmap = hisi_l3c_mmap,
};

static struct miscdevice l3c_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "hisi_l3c",
	.fops = &l3c_dev_fops,
	.mode = 0600,
};

static int __init hisi_l3c_init(void)
{
	int ret;

	spin_lock_init(&l3c_devs.lock);
	INIT_LIST_HEAD(&l3c_devs.node);

	ret = misc_register(&l3c_miscdev);
	if (ret)
		return ret;

	ret = cpuhp_setup_state_multi(CPUHP_AP_ONLINE_DYN, "hisi_l3c",
				      hisi_l3c_online_cpu, NULL);
	if (ret < 0)
		goto err_hp;

	hisi_l3c_cpuhp_state = ret;

	ret = platform_driver_register(&hisi_l3c_driver);
	if (ret)
		goto err_plat;

	return 0;

err_plat:
	cpuhp_remove_multi_state(CPUHP_AP_ONLINE_DYN);
err_hp:
	misc_deregister(&l3c_miscdev);

	return ret;
}
module_init(hisi_l3c_init);

static void __exit hisi_l3c_exit(void)
{
	platform_driver_unregister(&hisi_l3c_driver);
	cpuhp_remove_multi_state(CPUHP_AP_ONLINE_DYN);
	misc_deregister(&l3c_miscdev);
	hisi_soc_comp_del(NULL);
}
module_exit(hisi_l3c_exit);

MODULE_DESCRIPTION("Hisilicon L3 Cache Driver");
MODULE_AUTHOR("Yushan Wang <wangyushan12@huawei.com>");
MODULE_LICENSE("GPL");
