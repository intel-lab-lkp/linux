/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2018 - Arm Ltd */

#ifndef __ARM64_KVM_RAS_H__
#define __ARM64_KVM_RAS_H__

#include <linux/kvm_host.h>

/*
 * For synchrnous external abort taken to KVM at EL2, not on translation
 * table walk or hardware update of translation table, is FAR_EL2 valid?
 */
bool kvm_vcpu_sea_far_valid(const struct kvm_vcpu *vcpu);

/*
 * Handle synchronous external abort (SEA) in the following order:
 * 1. Delegate to APEI/GHES to see if they can claim SEA. If so, all done.
 * 2. Send SIGBUS to current with si_code=BUS_OBJERR and si_addr set to
 *    the poisoned host virtual address. When accurate HVA is unavailable,
 *    si_addr will be 0.
 *
 * Note this applies to both instruction and data abort (ESR_ELx_EC_IABT_*
 * and ESR_ELx_EC_DABT_*). As the name suggests, KVM must be taking the SEA
 * when calling into this function, e.g. kvm_vcpu_abt_issea == true.
 */
void kvm_handle_guest_sea(struct kvm_vcpu *vcpu);

#endif /* __ARM64_KVM_RAS_H__ */
