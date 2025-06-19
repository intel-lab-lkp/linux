// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2025 Loongson Technology Corporation Limited
 */
#include <asm/kvm_vcpu.h>
#include <asm/kvm_eiointc.h>
#include <asm/kvm_misc.h>

static int kvm_misc_read(struct kvm_vcpu *vcpu, struct kvm_io_device *dev,
			gpa_t addr, int len, void *val)
{
	unsigned long data;
	unsigned int ret;

	addr -= MISC_BASE;
	if (addr & (len - 1)) {
		kvm_err("%s: eiointc not aligned addr %llx len %d\n", __func__, addr, len);
		return -EINVAL;
	}

	ret = kvm_eiointc_get_status(vcpu, &data);
	if (ret)
		return ret;

	data = data >> ((addr & 7) * 8);
	switch (len) {
	case 1:
		*(unsigned char *)val = (unsigned char)data;
		break;

	case 2:
		*(unsigned short *)val = (unsigned short)data;
		break;

	case 4:
		*(unsigned int *)val = (unsigned int)data;
		break;

	default:
		*(unsigned long *)val = data;
		break;
	}

	return 0;
}

static int kvm_misc_write(struct kvm_vcpu *vcpu, struct kvm_io_device *dev,
		gpa_t addr, int len, const void *val)
{
	unsigned long data, mask;
	unsigned int shift;

	addr -= MISC_BASE;
	if (addr & (len - 1)) {
		kvm_err("%s: eiointc not aligned addr %llx len %d\n", __func__, addr, len);
		return -EINVAL;
	}

	shift = (addr & 7) * 8;
	switch (len) {
	case 1:
		data = *(unsigned char *)val;
		mask = 0xFF;
		mask = mask << shift;
		data = data << shift;
		break;

	case 2:
		data = *(unsigned short *)val;
		mask = 0xFFFF;
		mask = mask << shift;
		data = data << shift;
		break;

	case 4:
		data = *(unsigned int *)val;
		mask = UINT_MAX;
		mask = mask << shift;
		data = data << shift;
		break;

	default:
		data = *(unsigned long *)val;
		mask = ULONG_MAX;
		mask = mask << shift;
		data = data << shift;
		break;
	}

	return kvm_eiointc_update_status(vcpu, data, mask);
}

static const struct kvm_io_device_ops kvm_misc_ops = {
	.read   = kvm_misc_read,
	.write  = kvm_misc_write,
};

int kvm_loongarch_create_misc(struct kvm *kvm)
{
	struct kvm_io_device *device;
	int ret;

	if (kvm->arch.misc_created)
		return 0;

	device = &kvm->arch.misc;
	kvm_iodevice_init(device, &kvm_misc_ops);
	ret = kvm_io_bus_register_dev(kvm, KVM_IOCSR_BUS, MISC_BASE, MISC_SIZE, device);
	if (ret < 0)
		return ret;

	kvm->arch.misc_created = true;
	return 0;
}

void kvm_loongarch_destroy_misc(struct kvm *kvm)
{
	struct kvm_io_device *device;

	if (kvm->arch.misc_created) {
		device = &kvm->arch.misc;
		kvm_io_bus_unregister_dev(kvm, KVM_IOCSR_BUS, device);
		kvm->arch.misc_created = false;
	}
}
