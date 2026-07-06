/* SPDX-License-Identifier: GPL-2.0 */
#ifndef KVM_ARM_GUEST_H
#define KVM_ARM_GUEST_H

#include <linux/kvm_host.h>

unsigned long kvm_arm_num_regs(struct kvm_vcpu *vcpu);

#endif /* KVM_ARM_GUEST_H */
