/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __ARM64_KVM_RAS_H__
#define __ARM64_KVM_RAS_H__

#include <linux/acpi.h>
#include <linux/errno.h>
#include <linux/types.h>

#include <asm/acpi.h>

/*
 * For synchrnous external abort taken to KVM at EL2, not on translation
 * table walk or hardware update of translation table, is FAR_EL2 valid?
 */
bool kvm_vcpu_sea_far_valid(const struct kvm_vcpu *vcpu);

/*
 * Handle synchronous external abort (SEA) in the following order:
 * 1. Delegate to APEI/GHES to see if they can claim SEA. If so, all done.
 * 2. If the SEA is NOT about S2 translation table, send SIGBUS to current
 *    with BUS_OBJERR and si_addr set to faulting/poisoned host virtual
 *    address. When accurate HVA is unavailable, si_addr will be 0.
 * 3. Otherwise, directly inject an async SError to guest.
 *
 * Note this applies to both instruction and data abort (ESR_ELx_EC_IABT_*
 * and ESR_ELx_EC_DABT_*).
 */
void kvm_handle_guest_sea(struct kvm_vcpu *vcpu);

#endif /* __ARM64_KVM_RAS_H__ */
