/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SELFTESTS_TDX_TDX_UTIL_H
#define SELFTESTS_TDX_TDX_UTIL_H

#include <stdbool.h>

#include "kvm_util.h"

static inline bool is_tdx_vm(struct kvm_vm *vm)
{
	return vm->type == KVM_X86_TDX_VM;
}

/*
 * Verify that TDX is supported by KVM.
 */
static inline bool is_tdx_enabled(void)
{
	return !!(kvm_check_cap(KVM_CAP_VM_TYPES) & BIT(KVM_X86_TDX_VM));
}

/*
 * TDX ioctls
 */

#define __vm_tdx_vm_ioctl(vm, cmd, metadata, arg)			\
({									\
	int r;								\
									\
	union {								\
		struct kvm_tdx_cmd c;					\
		unsigned long raw;					\
	} tdx_cmd = { .c = {						\
		.id = (cmd),						\
		.flags = (uint32_t)(metadata),				\
		.data = (uint64_t)(arg),				\
	} };								\
									\
	r = __vm_ioctl(vm, KVM_MEMORY_ENCRYPT_OP, &tdx_cmd.raw);	\
	r ?: tdx_cmd.c.hw_error;					\
})

#define vm_tdx_vm_ioctl(vm, cmd, flags, arg)				\
({									\
	int ret = __vm_tdx_vm_ioctl(vm, cmd, flags, arg);		\
									\
	__TEST_ASSERT_VM_VCPU_IOCTL(!ret, #cmd,	ret, vm);		\
})

#define __vm_tdx_vcpu_ioctl(vcpu, cmd, metadata, arg)			\
({									\
	int r;								\
									\
	union {								\
		struct kvm_tdx_cmd c;					\
		unsigned long raw;					\
	} tdx_cmd = { .c = {						\
		.id = (cmd),						\
		.flags = (uint32_t)(metadata),				\
		.data = (uint64_t)(arg),				\
	} };								\
									\
	r = __vcpu_ioctl(vcpu, KVM_MEMORY_ENCRYPT_OP, &tdx_cmd.raw);	\
	r ?: tdx_cmd.c.hw_error;					\
})

#define vm_tdx_vcpu_ioctl(vcpu, cmd, flags, arg)			\
({									\
	int ret = __vm_tdx_vcpu_ioctl(vcpu, cmd, flags, arg);		\
									\
	__TEST_ASSERT_VM_VCPU_IOCTL(!ret, #cmd, ret, (vcpu)->vm);	\
})

void vm_tdx_init_vm(struct kvm_vm *vm, uint64_t attributes);

void vm_tdx_setup_boot_code_region(struct kvm_vm *vm);
void vm_tdx_setup_boot_parameters_region(struct kvm_vm *vm, uint32_t nr_runnable_vcpus);
void vm_tdx_load_common_boot_parameters(struct kvm_vm *vm);
void vm_tdx_load_vcpu_boot_parameters(struct kvm_vm *vm, struct kvm_vcpu *vcpu);
void vm_tdx_set_vcpu_entry_point(struct kvm_vcpu *vcpu, void *guest_code);

void vm_tdx_finalize(struct kvm_vm *vm);
struct kvm_vm *vm_tdx_create_with_one_vcpu(void *guest_code,
					   struct kvm_vcpu **vcpu);

#endif // SELFTESTS_TDX_TDX_UTIL_H
