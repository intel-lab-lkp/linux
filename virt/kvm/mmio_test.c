// SPDX-License-Identifier: GPL-2.0-only
/*
 * mmio_test.c - Kernel module side for testing the KVM riscv mmio functionality.
 */

#include <linux/kvm_host.h>

#include <kvm/iodev.h>
#include "mmio_test.h"

struct mmio_test {
	struct kvm *kvm;
	struct kvm_io_device dev;
	unsigned long start;
	unsigned long size;
	char cache[16];
};

static struct mmio_test *kvm_to_mmio_test_dev(const struct kvm_io_device *dev)
{
	return container_of(dev, struct mmio_test, dev);
}

static int mmio_read(struct kvm_vcpu *vcpu, struct kvm_io_device *dev,
		     gpa_t addr, int len, void *val)
{
	struct mmio_test *mmio_test = kvm_to_mmio_test_dev(dev);

	if ((addr - mmio_test->start + len) >= mmio_test->size)
		return -1;

	/* Write back cached value */
	memcpy(val, &mmio_test->cache[(addr - mmio_test->start)], len);
	return 0;
}

static int mmio_write(struct kvm_vcpu *vcpu, struct kvm_io_device *dev,
		      gpa_t addr, int len, const void *val)
{
	struct mmio_test *mmio_test = kvm_to_mmio_test_dev(dev);

	if ((addr - mmio_test->start + len) >= mmio_test->size)
		return -1;

	/* Cache value */
	memcpy(&mmio_test->cache[(addr - mmio_test->start)], val, len);
	return 0;
}

static const struct kvm_io_device_ops mmio_ops = {
	.read = mmio_read,
	.write = mmio_write,
};

static int mmio_test_create(struct kvm_device *dev, u32 type)
{
	struct mmio_test *mmio_test;
	int ret;

	mmio_test = kzalloc_obj(struct mmio_test, GFP_KERNEL);
	if (!mmio_test)
		return -ENOMEM;

	mmio_test->start = 0x20000000;
	mmio_test->size = 16;

	dev->private = mmio_test;

	kvm_iodevice_init(&mmio_test->dev, &mmio_ops);
	mutex_lock(&dev->kvm->slots_lock);
	ret = kvm_io_bus_register_dev(dev->kvm, KVM_MMIO_BUS, mmio_test->start,
				mmio_test->size, &mmio_test->dev);
	mutex_unlock(&dev->kvm->slots_lock);

	if (ret < 0)
		kfree(mmio_test);

	return ret;
}

static void mmio_test_destroy(struct kvm_device *dev)
{
	kvm_io_bus_unregister_dev(dev->kvm, KVM_MMIO_BUS, &((struct mmio_test *)dev->private)->dev);
	kfree(dev->private);
	kfree(dev);
}

struct kvm_device_ops kvm_riscv_mmio_test_device_ops = {
	.name = "kvm-riscv-mmio_test",
	.create = mmio_test_create,
	.destroy = mmio_test_destroy,
};

int kvm_mmio_test_ops_init(void)
{
	return kvm_register_device_ops(&kvm_riscv_mmio_test_device_ops,
					KVM_DEV_TYPE_TEST);
}

void kvm_mmio_test_ops_exit(void)
{
	kvm_unregister_device_ops(KVM_DEV_TYPE_TEST);
}
