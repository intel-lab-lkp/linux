/* SPDX-License-Identifier: GPL-2.0 */

#include <kvm_util.h>

static inline struct kvm_vcpu *vm_vcpu_add_with_vpmu(struct kvm_vm *vm,
						     uint32_t vcpu_id,
						     void *guest_code)
{
	struct kvm_vcpu_init init;

	/* Create vCPU with PMUv3 */
	vm_ioctl(vm, KVM_ARM_PREFERRED_TARGET, &init);
	init.features[0] |= (1 << KVM_ARM_VCPU_PMU_V3);

	return aarch64_vcpu_add(vm, 0, &init, guest_code);
}

static void vpmu_set_irq(struct kvm_vcpu *vcpu, int irq)
{
	kvm_device_attr_set(vcpu->fd, KVM_ARM_VCPU_PMU_V3_CTRL,
			    KVM_ARM_VCPU_PMU_V3_IRQ, &irq);
}

static void vpmu_init(struct kvm_vcpu *vcpu)
{
	kvm_device_attr_set(vcpu->fd, KVM_ARM_VCPU_PMU_V3_CTRL,
			    KVM_ARM_VCPU_PMU_V3_INIT, NULL);
}
