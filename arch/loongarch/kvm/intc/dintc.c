// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2025 Loongson Technology Corporation Limited
 */

#include <linux/kvm_host.h>
#include <asm/kvm_dintc.h>
#include <asm/kvm_vcpu.h>

static int kvm_dintc_ctrl_access(struct kvm_device *dev,
				struct kvm_device_attr *attr,
				bool is_write)
{
	int addr = attr->attr;
	void __user *data;
	struct loongarch_dintc *s = dev->kvm->arch.dintc;
	u64 tmp;
	u32 cpu_bit;

	data = (void __user *)attr->addr;
	switch (addr) {
	case KVM_DEV_LOONGARCH_DINTC_MSG_ADDR_BASE:
		if (is_write) {
			if (copy_from_user(&tmp, data, sizeof(s->msg_addr_base)))
				return -EFAULT;
			if (s->msg_addr_base) {
				/* Duplicate setting are not allowed. */
				return -EFAULT;
			}
			if (tmp > (1UL << AVEC_CPU_SHIFT))
				s->msg_addr_base = tmp;
			else
				return  -EFAULT;
			s->msg_addr_base = tmp;
			cpu_bit = find_first_bit((unsigned long *)&(s->msg_addr_base), 64)
						- AVEC_CPU_SHIFT;
			cpu_bit = min(cpu_bit, AVEC_CPU_BIT);
			s->cpu_mask = GENMASK(cpu_bit - 1, 0) & AVEC_CPU_MASK;
		}
		break;
	case KVM_DEV_LOONGARCH_DINTC_MSG_ADDR_SIZE:
		if (is_write) {
			if (copy_from_user(&tmp, data, sizeof(s->msg_addr_size)))
				return -EFAULT;
			if (s->msg_addr_size) {
				/*Duplicate setting are not allowed. */
				return -EFAULT;
			}
			s->msg_addr_size = tmp;
		}
		break;
	default:
		kvm_err("%s: unknown dintc register, addr = %d\n", __func__, addr);
		return -ENXIO;
	}

	return 0;
}

static int kvm_dintc_set_attr(struct kvm_device *dev,
			struct kvm_device_attr *attr)
{
	switch (attr->group) {
	case KVM_DEV_LOONGARCH_DINTC_CTRL:
		return kvm_dintc_ctrl_access(dev, attr, true);
	default:
		kvm_err("%s: unknown group (%d)\n", __func__, attr->group);
		return -EINVAL;
	}
}

static int kvm_dintc_create(struct kvm_device *dev, u32 type)
{
	struct kvm *kvm;
	struct loongarch_dintc *s;

	if (!dev) {
		kvm_err("%s: kvm_device ptr is invalid!\n", __func__);
		return -EINVAL;
	}

	kvm = dev->kvm;
	if (kvm->arch.dintc) {
		kvm_err("%s: LoongArch DINTC has already been created!\n", __func__);
		return -EINVAL;
	}

	s = kzalloc(sizeof(struct loongarch_dintc), GFP_KERNEL);
	if (!s)
		return -ENOMEM;

	s->kvm = kvm;
	kvm->arch.dintc = s;
	return 0;
}

static void kvm_dintc_destroy(struct kvm_device *dev)
{

	if (!dev || !dev->kvm || !dev->kvm->arch.dintc)
		return;

	kfree(dev->kvm->arch.dintc);
}

static struct kvm_device_ops kvm_dintc_dev_ops = {
	.name = "kvm-loongarch-dintc",
	.create = kvm_dintc_create,
	.destroy = kvm_dintc_destroy,
	.set_attr = kvm_dintc_set_attr,
};

int kvm_loongarch_register_dintc_device(void)
{
	return kvm_register_device_ops(&kvm_dintc_dev_ops, KVM_DEV_TYPE_LOONGARCH_DINTC);
}
