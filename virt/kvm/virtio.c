// SPDX-License-Identifier: GPL-2.0-only
/*
 * KVM-VIRTIO device
 *
 */
#include <linux/kvm_host.h>
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include "virtio.h"

struct kvm_virtio {
	struct mutex lock;
	bool noncoherent;
};

static int kvm_virtio_set_noncoherent(struct kvm_device *dev, long attr,
				      void __user *arg)
{
	struct kvm_virtio *kv = dev->private;

	/*
	 * Currently, only set to noncoherent is allowed, and therefore a virtio
	 * device is not allowed to switch back to coherent once it's set to
	 * noncoherent.
	 * User arg is also not checked as the attr name has indicated that the
	 * purpose is to set to noncoherent.
	 */
	if (attr != KVM_DEV_VIRTIO_NONCOHERENT_SET)
		return -ENXIO;

	mutex_lock(&kv->lock);
	if (kv->noncoherent)
		goto out;

	kv->noncoherent = true;
	kvm_arch_register_noncoherent_dma(dev->kvm);
out:
	mutex_unlock(&kv->lock);
	return 0;
}

static int kvm_virtio_set_attr(struct kvm_device *dev,
			       struct kvm_device_attr *attr)
{
	switch (attr->group) {
	case KVM_DEV_VIRTIO_NONCOHERENT:
		return kvm_virtio_set_noncoherent(dev, attr->attr,
						  u64_to_user_ptr(attr->addr));
	}

	return -ENXIO;
}

static int kvm_virtio_has_attr(struct kvm_device *dev,
			     struct kvm_device_attr *attr)
{
	switch (attr->group) {
	case KVM_DEV_VIRTIO_NONCOHERENT:
		switch (attr->attr) {
		case KVM_DEV_VIRTIO_NONCOHERENT_SET:
			return 0;
		}

		break;
	}

	return -ENXIO;
}

static void kvm_virtio_release(struct kvm_device *dev)
{
	struct kvm_virtio *kv = dev->private;

	if (kv->noncoherent)
		kvm_arch_unregister_noncoherent_dma(dev->kvm);
	kfree(kv);
	kfree(dev); /* alloc by kvm_ioctl_create_device, free by .release */
}

static int kvm_virtio_create(struct kvm_device *dev, u32 type);

static struct kvm_device_ops kvm_virtio_ops = {
	.name = "kvm-virtio",
	.create = kvm_virtio_create,
	.release = kvm_virtio_release,
	.set_attr = kvm_virtio_set_attr,
	.has_attr = kvm_virtio_has_attr,
};

static int kvm_virtio_create(struct kvm_device *dev, u32 type)
{
	struct kvm_virtio *kv;

	if (type != KVM_DEV_TYPE_VIRTIO)
		return -ENODEV;

	/*
	 * This kvm_virtio device is created per virtio device.
	 * Its default noncoherent state is false.
	 */
	kv = kzalloc(sizeof(*kv), GFP_KERNEL_ACCOUNT);
	if (!kv)
		return -ENOMEM;

	mutex_init(&kv->lock);

	dev->private = kv;

	return 0;
}

int kvm_virtio_ops_init(void)
{
	return kvm_register_device_ops(&kvm_virtio_ops, KVM_DEV_TYPE_VIRTIO);
}

void kvm_virtio_ops_exit(void)
{
	kvm_unregister_device_ops(KVM_DEV_TYPE_VIRTIO);
}
