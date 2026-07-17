// SPDX-License-Identifier: GPL-2.0
/*
 * Arm CLA driver - userspace interface
 *
 * Copyright 2026 Arm Limited.
 */

#include <linux/cdev.h>
#include <linux/cpu.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "arm-cla.h"
#include <uapi/linux/arm-cla.h>

static struct class *cla_class;
static dev_t cla_devt;

#define dev_nospec(idx) array_index_nospec(idx, cla_nr_devs)
#define accel_nospec(idx) array_index_nospec(idx, CLA_NUM_ACC)

#define cla_for_each_mapped_domain(_vma, _domain)			\
	for (unsigned long __pg = (_vma)->vm_pgoff;			\
	     __pg < (_vma)->vm_pgoff + vma_pages(_vma) &&		\
	     ((_domain) = cla_lut_pg[dev_nospec(__pg)]->domain);	\
	     __pg = (_domain)->pg_offset + (_domain)->nr_devs)

static long cla_ioctl_validate_param(struct arm_cla_param *param)
{
	int accel_id;
	int dev_id;

	switch (param->param) {
	case ARM_CLA_PARAM_UABI_VERSION:
	case ARM_CLA_PARAM_DEV_NR:
		if (param->index != 0)
			return -EINVAL;
		break;
	case ARM_CLA_PARAM_DEV_CPU_ID:
	case ARM_CLA_PARAM_DEV_DOMAIN_ID:
	case ARM_CLA_PARAM_DEV_PGOFF:
	case ARM_CLA_PARAM_DEV_AIDR:
	case ARM_CLA_PARAM_DEV_ACCELS:
		if (param->index >= cla_nr_devs)
			return -EINVAL;
		break;
	case ARM_CLA_PARAM_ACCEL_IIDR:
	case ARM_CLA_PARAM_ACCEL_DEVARCH:
	case ARM_CLA_PARAM_ACCEL_REVIDR:
		dev_id = ARM_CLA_PARAM_INDEX_DEV(param->index);
		accel_id = ARM_CLA_PARAM_INDEX_ACCEL(param->index);
		if (dev_id >= cla_nr_devs || accel_id >= CLA_NUM_ACC)
			return -EINVAL;
		dev_id = dev_nospec(dev_id);
		if ((cla_lut_pg[dev_id]->accelerators & BIT(accel_id)) == 0)
			return -ENODEV;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static long cla_ioctl_get_param(unsigned long arg)
{
	struct arm_cla_param __user *uparam = (void __user *)arg;
	struct arm_cla_param param;
	int accel_id;
	int dev_id;
	int ret;

	if (copy_from_user(&param, uparam, sizeof(param)))
		return -EFAULT;

	ret = cla_ioctl_validate_param(&param);
	if (ret)
		return ret;

	dev_id = dev_nospec(ARM_CLA_PARAM_INDEX_DEV(param.index));
	accel_id = accel_nospec(ARM_CLA_PARAM_INDEX_ACCEL(param.index));

	switch (param.param) {
	case ARM_CLA_PARAM_UABI_VERSION:
		param.value = ARM_CLA_UABI_VERSION;
		break;
	case ARM_CLA_PARAM_DEV_NR:
		param.value = cla_nr_devs;
		break;
	case ARM_CLA_PARAM_DEV_CPU_ID:
		param.value = cla_lut_pg[dev_id]->cpu;
		break;
	case ARM_CLA_PARAM_DEV_DOMAIN_ID:
		param.value = cla_lut_pg[dev_id]->domain->id;
		break;
	case ARM_CLA_PARAM_DEV_PGOFF:
		param.value = cla_lut_pg[dev_id]->pg_offset;
		break;
	case ARM_CLA_PARAM_DEV_AIDR:
		param.value = cla_lut_pg[dev_id]->aidr;
		break;
	case ARM_CLA_PARAM_DEV_ACCELS:
		param.value = cla_lut_pg[dev_id]->accelerators;
		break;
	case ARM_CLA_PARAM_ACCEL_IIDR:
		param.value = cla_lut_pg[dev_id]->accel_descs[accel_id].iidr;
		break;
	case ARM_CLA_PARAM_ACCEL_DEVARCH:
		param.value = cla_lut_pg[dev_id]->accel_descs[accel_id].devarch;
		break;
	case ARM_CLA_PARAM_ACCEL_REVIDR:
		param.value = cla_lut_pg[dev_id]->accel_descs[accel_id].revidr;
		break;
	}

	if (copy_to_user(uparam, &param, sizeof(param)))
		return -EFAULT;

	return 0;
}

static void cla_vma_open(struct vm_area_struct *vma)
{
	/*
	 * A vma previously created with cla_file_mmap() has been duplicated
	 * within the same mm (most likely due to mremap). While this could also
	 * be called for duplication into a new mm (via fork), we set
	 * VM_DONTCOPY on the original mmap, so this will never happen. So the
	 * contexts covered by this new vma already exist.
	 */
	struct cla_domain *domain;

	cla_for_each_mapped_domain(vma, domain)
		cla_ctx_map(domain, vma->vm_mm, vma->vm_file);
}

static void cla_vma_close(struct vm_area_struct *vma)
{
	struct cla_domain *domain;

	/* On munmap() or exit_mmap(), kill the context. */
	cla_for_each_mapped_domain(vma, domain)
		cla_ctx_unmap(domain, vma->vm_mm, vma->vm_file);
}

static vm_fault_t cla_vma_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	struct cla_domain *domain;
	struct cla_dev *dev;
	struct cla_ctx *ctx;
	unsigned long pg;

	/*
	 * Reassignment needs mmap_lock, so we cannot wait while holding it.
	 * Luckily, this flag is only missing in some exotic cases which do not
	 * apply for this VM_IO | VM_PFNMAP vma. i.e. GUP without
	 * FOLL_UNLOCKABLE or FOLL_NOWAIT or fixup_user_fault(unlocked=NULL),
	 * used by futex fault-in path.
	 */
	if (WARN_ON(!(vmf->flags & FAULT_FLAG_ALLOW_RETRY)))
		return VM_FAULT_SIGBUS;

	pg = vma->vm_pgoff + ((vmf->address - vma->vm_start) >> PAGE_SHIFT);
	dev = cla_lut_pg[pg];
	domain = dev->domain;

	mutex_lock(&domain->lock);
	ctx = cla_domain_lookup_ctx(domain, vma->vm_mm, vma->vm_file);
	if (WARN_ON(!ctx) || cla_ctx_is_dying(ctx) || domain->broken) {
		mutex_unlock(&domain->lock);
		return domain->broken ? VM_FAULT_SIGBUS : VM_FAULT_SIGSEGV;
	}

	/* If our ctx is the assigned one, map the device into memory. */
	if (domain->assigned_ctx == ctx) {
		vm_fault_t ret;

		ret = vmf_insert_pfn(vma, vmf->address, dev->pfn);
		mutex_unlock(&domain->lock);
		return ret;
	}

	/* Enqueue if not already, starting current assignee's time slice. */
	if (list_empty(&ctx->queue_node)) {
		bool was_empty = list_empty(&domain->queued_ctxs);
		unsigned long delay;

		list_add_tail(&ctx->queue_node, &domain->queued_ctxs);
		if (was_empty) {
			delay = domain->assigned_ctx ? CLA_SLICE_MS : 0;
			cla_domain_schedule_reassignment(domain, delay);
		}
	}

	mutex_unlock(&domain->lock);

	/* If waiting is not permitted, return indicating we need to retry. */
	if (vmf->flags & FAULT_FLAG_RETRY_NOWAIT)
		return VM_FAULT_RETRY;

	/*
	 * Drop the fault lock prior to sleeping and return VM_FAULT_RETRY when
	 * we wake up to rerun the fault. We must get the ctx while sleeping to
	 * prevent it from being freed while we are asleep on the contained wait
	 * queue.
	 */
	cla_ctx_get(ctx);
	release_fault_lock(vmf);
	wait_event_interruptible(ctx->waitq,
				 READ_ONCE(domain->assigned_ctx) == ctx ||
				 cla_ctx_is_dying(ctx) ||
				 READ_ONCE(domain->broken));
	cla_ctx_put(ctx);
	return VM_FAULT_RETRY;
}

static int cla_vma_may_split(struct vm_area_struct *vma, unsigned long addr)
{
	/*
	 * Forbid splitting cla mappings to prevent refcount leaks.
	 * cla_file_mmap()/cla_vma_open() track a context per domain mapped by
	 * the vma. If there were fewer domains mapped by the vma at
	 * cla_vma_close() then contexts would get leaked.
	 */
	return -EINVAL;
}

static const struct vm_operations_struct cla_vma_ops = {
	.open = cla_vma_open,
	.close = cla_vma_close,
	.fault = cla_vma_fault,
	.may_split = cla_vma_may_split,
};

static int cla_file_mmap(struct file *file, struct vm_area_struct *vma)
{
	unsigned long nr_pages = vma_pages(vma);
	struct cla_domain *rollback;
	struct cla_domain *domain;
	struct cla_ctx *ctx;

	/* Ensure the requested mapping is within range. */
	if (!nr_pages)
		return -EINVAL;
	if (vma->vm_pgoff >= cla_nr_devs)
		return -EINVAL;
	if (nr_pages > cla_nr_devs - vma->vm_pgoff)
		return -EINVAL;

	/* Only allow shared RW mappings. Nothing else makes sense for CLA. */
	if (!(vma->vm_flags & VM_SHARED))
		return -EINVAL;
	if (!(vma->vm_flags & VM_READ))
		return -EINVAL;
	if (!(vma->vm_flags & VM_WRITE))
		return -EINVAL;
	if (vma->vm_flags & VM_EXEC)
		return -EINVAL;

	/*
	 * Iterate over each domain covered by the vma and get-or-alloc its
	 * context. If any fails, we need to rollback with a put.
	 */
	cla_for_each_mapped_domain(vma, domain) {
		ctx = cla_ctx_map(domain, vma->vm_mm, vma->vm_file);
		if (IS_ERR(ctx)) {
			cla_for_each_mapped_domain(vma, rollback) {
				if (rollback == domain)
					return PTR_ERR(ctx);
				cla_ctx_unmap(rollback, vma->vm_mm,
					      vma->vm_file);
			}
		}
	}

	vm_flags_mod(vma, VM_DONTCOPY | VM_DONTDUMP | VM_DONTEXPAND |
			  VM_IO | VM_PFNMAP, VM_MAYEXEC);
	vma->vm_page_prot = pgprot_device(vma->vm_page_prot);
	vma->vm_ops = &cla_vma_ops;

	return 0;
}

static long cla_file_ioctl(struct file *file, unsigned int cmd,
			   unsigned long arg)
{
	switch (cmd) {
	case ARM_CLA_IOCTL_GET_PARAM:
		return cla_ioctl_get_param(arg);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations cla_fops = {
	.owner = THIS_MODULE,
	.mmap = cla_file_mmap,
	.unlocked_ioctl = cla_file_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = cla_file_ioctl,
#endif
};

static char *cla_devnode(const struct device *dev, umode_t *mode)
{
	if (mode)
		*mode = 0666;
	return NULL;
}

int __init cla_user_init(void)
{
	int ret;

	cla_class = class_create("arm-cla");
	if (IS_ERR(cla_class))
		return PTR_ERR(cla_class);
	cla_class->devnode = cla_devnode;

	ret = register_chrdev(0, KBUILD_MODNAME, &cla_fops);
	if (ret < 0)
		goto err_class_destroy;

	cla_devt = MKDEV(ret, 0);

	if (IS_ERR(device_create(cla_class, NULL, cla_devt, NULL, "cla"))) {
		ret = -ENODEV;
		goto err_unregister_chrdev;
	}

	return 0;

err_unregister_chrdev:
	unregister_chrdev(MAJOR(cla_devt), KBUILD_MODNAME);
err_class_destroy:
	class_destroy(cla_class);
	return ret;
}

void __exit cla_user_exit(void)
{
	device_destroy(cla_class, cla_devt);
	unregister_chrdev(MAJOR(cla_devt), KBUILD_MODNAME);
	class_destroy(cla_class);
}
