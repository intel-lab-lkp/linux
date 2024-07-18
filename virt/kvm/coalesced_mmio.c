// SPDX-License-Identifier: GPL-2.0
/*
 * KVM coalesced MMIO
 *
 * Copyright (c) 2008 Bull S.A.S.
 * Copyright 2009 Red Hat, Inc. and/or its affiliates.
 * Copyright 2024 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 *  Author: Laurent Vivier <Laurent.Vivier@bull.net>
 *
 */

#include <kvm/iodev.h>

#include <linux/kvm_host.h>
#include <linux/slab.h>
#include <linux/kvm.h>
#include <linux/anon_inodes.h>
#include <linux/poll.h>

#include "coalesced_mmio.h"

static inline struct kvm_coalesced_mmio_dev *to_mmio(struct kvm_io_device *dev)
{
	return container_of(dev, struct kvm_coalesced_mmio_dev, dev);
}

static int coalesced_mmio_in_range(struct kvm_coalesced_mmio_dev *dev,
				   gpa_t addr, int len)
{
	/* is it in a batchable area ?
	 * (addr,len) is fully included in
	 * (zone->addr, zone->size)
	 */
	if (len < 0)
		return 0;
	if (addr + len < addr)
		return 0;
	if (addr < dev->zone.addr)
		return 0;
	if (addr + len > dev->zone.addr + dev->zone.size)
		return 0;
	return 1;
}

static int coalesced_mmio_has_room(struct kvm_coalesced_mmio_ring *ring, u32 last)
{
	/* Are we able to batch it ? */

	/* last is the first free entry
	 * check if we don't meet the first used entry
	 * there is always one unused entry in the buffer
	 */
	if ((last + 1) % KVM_COALESCED_MMIO_MAX == READ_ONCE(ring->first)) {
		/* full */
		return 0;
	}

	return 1;
}

static int coalesced_mmio_write(struct kvm_vcpu *vcpu,
				struct kvm_io_device *this, gpa_t addr,
				int len, const void *val)
{
	struct kvm_coalesced_mmio_dev *dev = to_mmio(this);
	struct kvm_coalesced_mmio_ring *ring = dev->kvm->coalesced_mmio_ring;
	spinlock_t *lock = dev->buffer_dev ?
			   &dev->buffer_dev->ring_lock :
			   &dev->kvm->ring_lock;
	__u32 insert;

	if (!coalesced_mmio_in_range(dev, addr, len))
		return -EOPNOTSUPP;

	spin_lock(lock);

	if (dev->buffer_dev) {
		ring = dev->buffer_dev->ring;
		if (!ring) {
			spin_unlock(lock);
			return -EOPNOTSUPP;
		}
	}

	insert = READ_ONCE(ring->last);
	if (!coalesced_mmio_has_room(ring, insert) ||
	    insert >= KVM_COALESCED_MMIO_MAX) {
		spin_unlock(lock);
		return -EOPNOTSUPP;
	}

	/* copy data in first free entry of the ring */

	ring->coalesced_mmio[insert].phys_addr = addr;
	ring->coalesced_mmio[insert].len = len;
	memcpy(ring->coalesced_mmio[insert].data, val, len);
	ring->coalesced_mmio[insert].pio = dev->zone.pio;
	smp_wmb();
	ring->last = (insert + 1) % KVM_COALESCED_MMIO_MAX;
	spin_unlock(lock);

	if (dev->buffer_dev)
		wake_up_interruptible(&dev->buffer_dev->wait_queue);

	return 0;
}

static void coalesced_mmio_destructor(struct kvm_io_device *this)
{
	struct kvm_coalesced_mmio_dev *dev = to_mmio(this);

	list_del(&dev->list);

	kfree(dev);
}

static const struct kvm_io_device_ops coalesced_mmio_ops = {
	.write      = coalesced_mmio_write,
	.destructor = coalesced_mmio_destructor,
};

int kvm_coalesced_mmio_init(struct kvm *kvm)
{
	struct page *page;

	page = alloc_page(GFP_KERNEL_ACCOUNT | __GFP_ZERO);
	if (!page)
		return -ENOMEM;

	kvm->coalesced_mmio_ring = page_address(page);

	/*
	 * We're using this spinlock to sync access to the coalesced ring.
	 * The list doesn't need its own lock since device registration and
	 * unregistration should only happen when kvm->slots_lock is held.
	 */
	spin_lock_init(&kvm->ring_lock);
	INIT_LIST_HEAD(&kvm->coalesced_zones);
	INIT_LIST_HEAD(&kvm->coalesced_buffers);

	return 0;
}

void kvm_coalesced_mmio_free(struct kvm *kvm)
{
	if (kvm->coalesced_mmio_ring)
		free_page((unsigned long)kvm->coalesced_mmio_ring);
}

static void coalesced_mmio_buffer_vma_close(struct vm_area_struct *vma)
{
	struct kvm_coalesced_mmio_buffer_dev *dev = vma->vm_private_data;

	spin_lock(&dev->ring_lock);

	vfree(dev->ring);
	dev->ring = NULL;

	spin_unlock(&dev->ring_lock);
}

static const struct vm_operations_struct coalesced_mmio_buffer_vm_ops = {
	.close = coalesced_mmio_buffer_vma_close,
};

static int coalesced_mmio_buffer_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct kvm_coalesced_mmio_buffer_dev *dev = file->private_data;
	unsigned long pfn;
	int ret = 0;

	spin_lock(&dev->ring_lock);

	if (dev->ring) {
		ret = -EBUSY;
		goto out_unlock;
	}

	dev->ring = vmalloc_user(PAGE_SIZE);
	if (!dev->ring) {
		ret = -ENOMEM;
		goto out_unlock;
	}

	pfn = vmalloc_to_pfn(dev->ring);

	if (remap_pfn_range(vma, vma->vm_start, pfn, PAGE_SIZE,
			    vma->vm_page_prot)) {
		vfree(dev->ring);
		dev->ring = NULL;
		ret = -EAGAIN;
		goto out_unlock;
	}

	vma->vm_ops = &coalesced_mmio_buffer_vm_ops;
	vma->vm_private_data = dev;

out_unlock:
	spin_unlock(&dev->ring_lock);

	return ret;
}

static int coalesced_mmio_buffer_release(struct inode *inode, struct file *file)
{

	struct kvm_coalesced_mmio_buffer_dev *buffer_dev = file->private_data;
	struct kvm_coalesced_mmio_dev *mmio_dev, *tmp;
	struct kvm *kvm = buffer_dev->kvm;

	/* Deregister all zones associated with this ring buffer */
	mutex_lock(&kvm->slots_lock);

	list_for_each_entry_safe(mmio_dev, tmp, &kvm->coalesced_zones, list) {
		if (mmio_dev->buffer_dev == buffer_dev) {
			if (kvm_io_bus_unregister_dev(kvm,
			    mmio_dev->zone.pio ? KVM_PIO_BUS : KVM_MMIO_BUS,
			    &mmio_dev->dev))
				break;
		}
	}

	list_del(&buffer_dev->list);
	kfree(buffer_dev);

	mutex_unlock(&kvm->slots_lock);

	return 0;
}

static __poll_t coalesced_mmio_buffer_poll(struct file *file, struct poll_table_struct *wait)
{
	struct kvm_coalesced_mmio_buffer_dev *dev = file->private_data;
	__poll_t mask = 0;

	poll_wait(file, &dev->wait_queue, wait);

	spin_lock(&dev->ring_lock);
	if (dev->ring && (READ_ONCE(dev->ring->first) != READ_ONCE(dev->ring->last)))
		mask = POLLIN | POLLRDNORM;
	spin_unlock(&dev->ring_lock);

	return mask;
}

static const struct file_operations coalesced_mmio_buffer_ops = {
	.mmap = coalesced_mmio_buffer_mmap,
	.release = coalesced_mmio_buffer_release,
	.poll = coalesced_mmio_buffer_poll,
};

int kvm_vm_ioctl_create_coalesced_mmio_buffer(struct kvm *kvm)
{
	int ret;
	struct kvm_coalesced_mmio_buffer_dev *dev;

	dev = kzalloc(sizeof(struct kvm_coalesced_mmio_buffer_dev),
		      GFP_KERNEL_ACCOUNT);
	if (!dev)
		return -ENOMEM;

	dev->kvm = kvm;
	init_waitqueue_head(&dev->wait_queue);
	spin_lock_init(&dev->ring_lock);

	ret = anon_inode_getfd("coalesced_mmio_buf", &coalesced_mmio_buffer_ops,
			       dev, O_RDWR | O_CLOEXEC);
	if (ret < 0) {
		kfree(dev);
		return ret;
	}

	mutex_lock(&kvm->slots_lock);
	list_add_tail(&dev->list, &kvm->coalesced_buffers);
	mutex_unlock(&kvm->slots_lock);

	return ret;
}

int kvm_vm_ioctl_register_coalesced_mmio(struct kvm *kvm,
					 struct kvm_coalesced_mmio_zone *zone)
{
	int ret;
	struct kvm_coalesced_mmio_dev *dev;
	struct kvm_coalesced_mmio_buffer_dev *buffer_dev = NULL;

	if (zone->pio != 1 && zone->pio != 0)
		return -EINVAL;

	dev = kzalloc(sizeof(struct kvm_coalesced_mmio_dev),
		      GFP_KERNEL_ACCOUNT);
	if (!dev)
		return -ENOMEM;

	kvm_iodevice_init(&dev->dev, &coalesced_mmio_ops);
	dev->kvm = kvm;
	dev->zone = *zone;
	dev->buffer_dev = buffer_dev;

	mutex_lock(&kvm->slots_lock);
	ret = kvm_io_bus_register_dev(kvm,
				zone->pio ? KVM_PIO_BUS : KVM_MMIO_BUS,
				zone->addr, zone->size, &dev->dev);
	if (ret < 0)
		goto out_free_dev;
	list_add_tail(&dev->list, &kvm->coalesced_zones);
	mutex_unlock(&kvm->slots_lock);

	return 0;

out_free_dev:
	mutex_unlock(&kvm->slots_lock);
	kfree(dev);

	return ret;
}

int kvm_vm_ioctl_unregister_coalesced_mmio(struct kvm *kvm,
					   struct kvm_coalesced_mmio_zone *zone)
{
	struct kvm_coalesced_mmio_dev *dev, *tmp;
	int r;

	if (zone->pio != 1 && zone->pio != 0)
		return -EINVAL;

	mutex_lock(&kvm->slots_lock);

	list_for_each_entry_safe(dev, tmp, &kvm->coalesced_zones, list) {
		if (zone->pio == dev->zone.pio &&
		    coalesced_mmio_in_range(dev, zone->addr, zone->size)) {
			r = kvm_io_bus_unregister_dev(kvm,
				zone->pio ? KVM_PIO_BUS : KVM_MMIO_BUS, &dev->dev);
			/*
			 * On failure, unregister destroys all devices on the
			 * bus, including the target device. There's no need
			 * to restart the walk as there aren't any zones left.
			 */
			if (r)
				break;
		}
	}

	mutex_unlock(&kvm->slots_lock);

	/*
	 * Ignore the result of kvm_io_bus_unregister_dev(), from userspace's
	 * perspective, the coalesced MMIO is most definitely unregistered.
	 */
	return 0;
}
