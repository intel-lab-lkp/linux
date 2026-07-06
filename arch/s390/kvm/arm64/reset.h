/* SPDX-License-Identifier: GPL-2.0 */
#ifndef KVM_ARM_RESET_H
#define KVM_ARM_RESET_H

#include <linux/kvm_host.h>

bool kvm_arm_vcpu_is_finalized(struct kvm_vcpu *vcpu);
void kvm_reset_vcpu(struct kvm_vcpu *vcpu);
int kvm_arm_vcpu_finalize(struct kvm_vcpu *vcpu, int feature);

#endif /* KVM_ARM_RESET_H */
