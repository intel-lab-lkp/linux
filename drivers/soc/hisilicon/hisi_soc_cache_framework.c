// SPDX-License-Identifier: GPL-2.0
/*
 * Framework for HiSilicon SoC cache, manages HiSilicon SoC cache drivers.
 *
 * Copyright (c) 2024 HiSilicon Technologies Co., Ltd.
 * Author: Jie Wang <wangjie125@huawei.com>
 * Author: Yicong Yang <yangyicong@hisilicon.com>
 * Author: Yushan Wang <wangyushan12@huawei.com>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cleanup.h>
#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/memory.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/pagewalk.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>

#include <asm/page.h>

#include "hisi_soc_cache_framework.h"

struct hisi_soc_cache_lock_region {
	/* physical memory region of the arena allocated for aligned address */
	unsigned long arena_start;
	unsigned long arena_end;
	/* VMA region of locked memory for future release */
	unsigned long vm_start;
	unsigned long vm_end;
	phys_addr_t addr;
	size_t size;
	u32 status;
	int cpu;
};

struct hisi_soc_comp_inst {
	struct list_head node;
	struct hisi_soc_comp *comp;
};

struct hisi_soc_comp_list {
	struct list_head node;
	/* protects list of HiSilicon SoC cache components */
	spinlock_t lock;
	u32 inst_num;
};

static struct hisi_soc_comp_list soc_cache_devs[SOC_COMP_TYPE_MAX];

static int hisi_soc_cache_lock(int cpu, phys_addr_t addr, size_t size)
{
	struct hisi_soc_comp_inst *inst;
	struct list_head *head;
	int ret = -ENOMEM;

	guard(spinlock)(&soc_cache_devs[HISI_SOC_L3C].lock);

	/* Iterate L3C instances to perform operation, break loop once found. */
	head = &soc_cache_devs[HISI_SOC_L3C].node;
	list_for_each_entry(inst, head, node) {
		if (!cpumask_test_cpu(cpu, &inst->comp->affinity_mask))
			continue;
		ret = inst->comp->ops->do_lock(inst->comp, addr, size);
		if (ret)
			return ret;
		break;
	}

	list_for_each_entry(inst, head, node) {
		if (!cpumask_test_cpu(cpu, &inst->comp->affinity_mask))
			continue;
		ret = inst->comp->ops->poll_lock_done(inst->comp, addr, size);
		if (ret)
			return ret;
		break;
	}

	return ret;
}

static int hisi_soc_cache_unlock(int cpu, phys_addr_t addr)
{
	struct hisi_soc_comp_inst *inst;
	struct list_head *head;
	int ret = 0;

	guard(spinlock)(&soc_cache_devs[HISI_SOC_L3C].lock);

	/* Iterate L3C instances to perform operation, break loop once found. */
	head = &soc_cache_devs[HISI_SOC_L3C].node;
	list_for_each_entry(inst, head, node) {
		if (!cpumask_test_cpu(cpu, &inst->comp->affinity_mask))
			continue;
		ret = inst->comp->ops->do_unlock(inst->comp, addr);
		if (ret)
			return ret;
		break;
	}

	list_for_each_entry(inst, head, node) {
		if (!cpumask_test_cpu(cpu, &inst->comp->affinity_mask))
			continue;
		ret = inst->comp->ops->poll_unlock_done(inst->comp, addr);
		if (ret)
			return ret;
		break;
	}

	return ret;
}

static int hisi_soc_cache_inst_check(const struct hisi_soc_comp *comp,
				     enum hisi_soc_comp_type comp_type)
{
	struct hisi_soc_comp_ops *ops = comp->ops;

	/* Different types of component could have different ops. */
	switch (comp_type) {
	case HISI_SOC_L3C:
		if (!ops->do_lock || !ops->poll_lock_done
		    || !ops->do_unlock || !ops->poll_unlock_done)
			return -EINVAL;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int hisi_soc_cache_inst_add(struct hisi_soc_comp *comp,
				   enum hisi_soc_comp_type comp_type)
{
	struct hisi_soc_comp_inst *comp_inst;
	int ret;

	ret = hisi_soc_cache_inst_check(comp, comp_type);
	if (ret)
		return ret;

	comp_inst = kzalloc(sizeof(*comp_inst), GFP_KERNEL);
	if (!comp_inst)
		return -ENOMEM;

	comp_inst->comp = comp;

	scoped_guard(spinlock, &soc_cache_devs[comp_type].lock) {
		list_add_tail(&comp_inst->node,
			      &soc_cache_devs[comp_type].node);
		soc_cache_devs[comp_type].inst_num++;
	}

	return 0;
}

/*
 * When @comp is NULL, it means to delete all instances of @comp_type.
 */
static void hisi_soc_cache_inst_del(struct hisi_soc_comp *comp,
				    enum hisi_soc_comp_type comp_type)
{
	struct hisi_soc_comp_inst *inst, *tmp;

	guard(spinlock)(&soc_cache_devs[comp_type].lock);
	list_for_each_entry_safe(inst, tmp, &soc_cache_devs[comp_type].node,
				 node) {
		if (comp && comp != inst->comp)
			continue;

		if (soc_cache_devs[comp_type].inst_num > 0)
			soc_cache_devs[comp_type].inst_num--;

		list_del(&inst->node);
		kfree(inst);

		/* Stop the loop if we have already deleted @comp. */
		if (comp)
			break;
	}
}

int hisi_soc_comp_inst_add(struct hisi_soc_comp *comp)
{
	int ret, i = HISI_SOC_L3C;

	if (!comp || !comp->ops || comp->comp_type == 0)
		return -EINVAL;

	for_each_set_bit_from(i, &comp->comp_type, SOC_COMP_TYPE_MAX) {
		ret = hisi_soc_cache_inst_add(comp, i);
		if (ret)
			return ret;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(hisi_soc_comp_inst_add);

int hisi_soc_comp_inst_del(struct hisi_soc_comp *comp)
{
	int i;

	if (!comp)
		return -EINVAL;

	for_each_set_bit(i, &comp->comp_type, SOC_COMP_TYPE_MAX)
		hisi_soc_cache_inst_del(comp, i);

	return 0;
}
EXPORT_SYMBOL_GPL(hisi_soc_comp_inst_del);

static void hisi_soc_cache_vm_open(struct vm_area_struct *vma)
{
	struct hisi_soc_cache_lock_region *clr = vma->vm_private_data;

	/*
	 * Only perform cache lock when the vma passed in is created
	 * in hisi_soc_cache_mmap.
	 */
	if (clr->vm_start != vma->vm_start || clr->vm_end != vma->vm_end)
		return;

	clr->status = hisi_soc_cache_lock(clr->cpu, clr->addr, clr->size);
}

/**
 * hisi_soc_cache_aligned_alloc - Allocate memory region to be locked and
 *				  returns address that aligned to the requested
 *				  size.
 * @clr:	The locked memory region to be allocated for.
 * @size:	Requested memory size.
 * @addr:	Pointer of the start physical address of the requested
 *		memory region.
 *
 * @return:
 *    - -ENOMEM: If allocation fails.
 *    - 0: If allocations succeeds.
 *
 * Physical address of allocated memory region is requested to be aligned to
 * its size.  In order to achieve that, add the order of requested memory size
 * by 1 to double the size of allocated memory to ensure the existence of size-
 * aligned address.  After locating the aligned region, release the unused
 * pages from both sides to avoid waste.
 */
static int hisi_soc_cache_aligned_alloc(struct hisi_soc_cache_lock_region *clr,
					unsigned long size,
					unsigned long *addr)
{
	unsigned long prefix_hole_size, suffix_hole_size;
	unsigned long arena_start, arena_end;
	int order = get_order(size) + 1;
	unsigned long start, end;
	unsigned long free_size;
	struct page *pg;
	int bit;

	pg = alloc_pages(GFP_KERNEL | __GFP_NOWARN | __GFP_ZERO, order);
	if (!pg)
		return -ENOMEM;

	arena_start = page_to_phys(pg);
	arena_end = arena_start + (PAGE_SIZE << order) - 1;

	/*
	 * Align up the address by the requested size if the address is not
	 * naturally aligned to the size.
	 */
	start = arena_start % size == 0
		? arena_start
		: arena_start / size * size + size;
	end = start + size - 1;
	prefix_hole_size = start - arena_start;
	suffix_hole_size = arena_end - end;

	if (prefix_hole_size >= PAGE_SIZE) {
		bit = PAGE_SHIFT;
		for_each_set_bit_from(bit, &prefix_hole_size,
				      MAX_PAGE_ORDER + PAGE_SHIFT) {
			free_size = (PAGE_SIZE << (bit - PAGE_SHIFT)) - 1;
			__free_pages(phys_to_page(arena_start),
				     bit - PAGE_SHIFT);
			arena_start += free_size + 1;
		}
	}

	if (suffix_hole_size >= PAGE_SIZE) {
		bit = PAGE_SHIFT;
		for_each_set_bit_from(bit, &suffix_hole_size,
				      MAX_PAGE_ORDER + PAGE_SHIFT) {
			free_size = (PAGE_SIZE << (bit - PAGE_SHIFT)) - 1;
			__free_pages(phys_to_page(arena_end - free_size),
				     bit - PAGE_SHIFT);
			arena_end -= free_size + 1;
		}
	}

	*addr = start;
	clr->arena_start = arena_start;
	clr->arena_end = arena_end;

	return 0;
}

/**
 * hisi_soc_cache_aligned_free - Free the aligned memory region allcated by
 *				 hisi_soc_cache_aligned_alloc().
 * @clr:	The allocated locked memory region.
 *
 * Since unused memory pages are release in hisi_soc_cache_aligned_alloc(), the
 * memory region to be freed here may not be power of 2 numbers of pages.
 * Thus split the memory by page order and release them accordingly.
 */
static void hisi_soc_cache_aligned_free(struct hisi_soc_cache_lock_region *clr)
{
	unsigned long size = clr->arena_end - clr->arena_start;
	int bit = PAGE_SHIFT;

	for_each_set_bit_from(bit, &size, MAX_PAGE_ORDER)
		__free_pages(phys_to_page(clr->arena_start), bit - PAGE_SHIFT);
}

static void hisi_soc_cache_vm_close(struct vm_area_struct *vma)
{
	struct hisi_soc_cache_lock_region *clr = vma->vm_private_data;
	int ret;

	/*
	 * Only perform cache unlock when the vma passed in is created
	 * in hisi_soc_cache_mmap.
	 */
	if (clr->vm_start != vma->vm_start || clr->vm_end != vma->vm_end)
		return;

	ret = hisi_soc_cache_unlock(clr->cpu, clr->addr);

	hisi_soc_cache_aligned_free(clr);
	kfree(clr);
	vma->vm_private_data = NULL;
}

/*
 * mremap operation is not supported for HiSilicon SoC cache.
 */
static int hisi_soc_cache_vm_mremap(struct vm_area_struct *vma)
{
	struct hisi_soc_cache_lock_region *clr = vma->vm_private_data;

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

static const struct vm_operations_struct hisi_soc_cache_vm_ops = {
	.open = hisi_soc_cache_vm_open,
	.close = hisi_soc_cache_vm_close,
	.mremap = hisi_soc_cache_vm_mremap,
};

static int hisi_soc_cache_mmap(struct file *file, struct vm_area_struct *vma)
{
	unsigned long size = vma->vm_end - vma->vm_start;
	struct hisi_soc_cache_lock_region *clr;
	unsigned long addr;
	int ret;

	clr = kzalloc(sizeof(*clr), GFP_KERNEL);
	if (!clr)
		return -ENOMEM;

	ret = hisi_soc_cache_aligned_alloc(clr, size, &addr);
	if (ret)
		goto out_clr;

	ret = remap_pfn_range(vma, vma->vm_start,
			      (addr >> PAGE_SHIFT) + vma->vm_pgoff,
			      size, vma->vm_page_prot);
	if (ret)
		goto out_page;

	clr->addr = addr;
	clr->size = size;
	clr->cpu = smp_processor_id();
	vm_flags_set(vma, VM_DONTCOPY | VM_DONTEXPAND);

	/*
	 * The vma should not be moved throughout its lifetime, store the
	 * region for verification.
	 */
	clr->vm_start = vma->vm_start;
	clr->vm_end = vma->vm_end;

	vma->vm_private_data = clr;
	vma->vm_ops = &hisi_soc_cache_vm_ops;
	hisi_soc_cache_vm_ops.open(vma);

	if (clr->status) {
		ret = -ENOMEM;
		goto out_page;
	}

	return 0;

out_page:
	hisi_soc_cache_aligned_free(clr);
out_clr:
	kfree(clr);
	return ret;
}

static const struct file_operations soc_cache_dev_fops = {
	.owner = THIS_MODULE,
	.mmap = hisi_soc_cache_mmap,
};

static struct miscdevice soc_cache_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "hisi_soc_cache_mgmt",
	.fops = &soc_cache_dev_fops,
	.mode = 0600,
};

static void hisi_soc_cache_inst_uninit(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(soc_cache_devs); ++i)
		hisi_soc_cache_inst_del(NULL, i);
}

static void hisi_soc_cache_framework_data_init(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(soc_cache_devs); ++i) {
		spin_lock_init(&soc_cache_devs[i].lock);
		INIT_LIST_HEAD(&soc_cache_devs[i].node);
	}
}

static const char *const hisi_soc_cache_item_str[SOC_COMP_TYPE_MAX] = {
	"cache"
};

/*
 * Print cache instance number debug information for debug FS.
 */
static ssize_t hisi_soc_cache_dbg_get_inst_num(struct file *file,
					       char __user *buff,
					       size_t cnt,
					       loff_t *ppos)
{
#define HISI_SOC_CACHE_DBGFS_REG_LEN 100
	char *read_buff __free(kfree) = NULL;
	int len, i, pos = 0;

	if (!access_ok(buff, cnt))
		return -EFAULT;
	if (*ppos < 0)
		return -EINVAL;
	if (cnt == 0)
		return 0;

	read_buff = kzalloc(HISI_SOC_CACHE_DBGFS_REG_LEN, GFP_KERNEL);
	if (!read_buff)
		return -ENOMEM;

	len = HISI_SOC_CACHE_DBGFS_REG_LEN;

	for (i = 0; i < ARRAY_SIZE(soc_cache_devs); i++) {
		guard(spinlock)(&soc_cache_devs[i].lock);
		pos += scnprintf(read_buff + pos, len - pos,
				 "%s inst num: %u\n",
				 hisi_soc_cache_item_str[i],
				 soc_cache_devs[i].inst_num);
	}

	return simple_read_from_buffer(buff, cnt, ppos, read_buff,
				       strlen(read_buff));
}

static struct dentry *hisi_cache_dbgfs_root;
static const struct file_operations hisi_cache_dbgfs_ops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = hisi_soc_cache_dbg_get_inst_num,
};

static void hisi_soc_cache_dbgfs_init(void)
{
	hisi_cache_dbgfs_root = debugfs_create_dir("hisi_soc_cache_frm", NULL);
	debugfs_create_file("instance", 0400, hisi_cache_dbgfs_root, NULL,
			    &hisi_cache_dbgfs_ops);
}

static void hisi_soc_cache_dbgfs_uninit(void)
{
	debugfs_remove_recursive(hisi_cache_dbgfs_root);
	hisi_cache_dbgfs_root = NULL;
}

static int __init hisi_soc_cache_framework_init(void)
{
	int ret;

	hisi_soc_cache_framework_data_init();

	ret = misc_register(&soc_cache_miscdev);
	if (ret) {
		hisi_soc_cache_inst_uninit();
		return ret;
	}

	hisi_soc_cache_dbgfs_init();

	return 0;
}
module_init(hisi_soc_cache_framework_init);

static void __exit hisi_soc_cache_framework_exit(void)
{
	hisi_soc_cache_dbgfs_uninit();
	misc_deregister(&soc_cache_miscdev);
	hisi_soc_cache_inst_uninit();
}
module_exit(hisi_soc_cache_framework_exit);

MODULE_DESCRIPTION("HiSilicon SoC Cache Framework Driver");
MODULE_AUTHOR("Jie Wang <wangjie125@huawei.com>");
MODULE_AUTHOR("Yushan Wang <wangyushan12@huawei.com>");
MODULE_LICENSE("GPL");
