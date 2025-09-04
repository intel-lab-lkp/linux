/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SELFTESTS_TDX_TDX_UTIL_H
#define SELFTESTS_TDX_TDX_UTIL_H

#include <stdbool.h>

#include "kvm_util.h"

static inline bool is_tdx_vm(struct kvm_vm *vm)
{
	return vm->type == KVM_X86_TDX_VM;
}

void vm_tdx_setup_boot_code_region(struct kvm_vm *vm);
void vm_tdx_setup_boot_parameters_region(struct kvm_vm *vm, uint32_t nr_runnable_vcpus);
void vm_tdx_load_common_boot_parameters(struct kvm_vm *vm);
void vm_tdx_load_vcpu_boot_parameters(struct kvm_vm *vm, struct kvm_vcpu *vcpu);
void vm_tdx_set_vcpu_entry_point(struct kvm_vcpu *vcpu, void *guest_code);

#endif // SELFTESTS_TDX_TDX_UTIL_H
