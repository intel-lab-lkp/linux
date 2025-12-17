// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for HiSilicon L3 cache.
 *
 * Copyright (c) 2025 HiSilicon Technologies Co., Ltd.
 * Author: Yushan Wang <wangyushan12@huawei.com>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cleanup.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/spinlock_types.h>
#include <linux/types.h>

#include <asm/cputype.h>

#include <uapi/misc/hisi_l3c.h>

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

static struct hisi_soc_comp_list l3c_devs;

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
	spin_lock_init(&l3c_devs.lock);
	INIT_LIST_HEAD(&l3c_devs.node);

	return misc_register(&l3c_miscdev);
}
module_init(hisi_l3c_init);

static void __exit hisi_l3c_exit(void)
{
	misc_deregister(&l3c_miscdev);
	hisi_soc_comp_del(NULL);
}
module_exit(hisi_l3c_exit);

MODULE_DESCRIPTION("Hisilicon L3 Cache Driver");
MODULE_AUTHOR("Yushan Wang <wangyushan12@huawei.com>");
MODULE_LICENSE("GPL");
