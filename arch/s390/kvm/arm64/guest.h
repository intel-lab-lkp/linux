/* SPDX-License-Identifier: GPL-2.0 */
#ifndef KVM_ARM_GUEST_H
#define KVM_ARM_GUEST_H

#include <linux/kvm_host.h>

unsigned long kvm_arm_num_regs(struct kvm_vcpu *vcpu);
int kvm_arm_get_reg(struct kvm_vcpu *vcpu, const struct kvm_one_reg *reg);
int kvm_arm_set_reg(struct kvm_vcpu *vcpu, const struct kvm_one_reg *reg);
int kvm_arm_vcpu_set_attr(struct kvm_vcpu *vcpu, struct kvm_device_attr *attr);
int kvm_arm_vcpu_get_attr(struct kvm_vcpu *vcpu, struct kvm_device_attr *attr);
int kvm_arm_vcpu_has_attr(struct kvm_vcpu *vcpu, struct kvm_device_attr *attr);

#endif /* KVM_ARM_GUEST_H */
