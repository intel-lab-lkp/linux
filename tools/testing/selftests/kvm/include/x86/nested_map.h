/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * tools/testing/selftests/kvm/include/x86_64/nested_map.h
 *
 * Copyright (C) 2025, Google LLC.
 */

#ifndef SELFTEST_KVM_NESTED_MAP_H
#define SELFTEST_KVM_NESTED_MAP_H

#include "kvm_util.h"

void nested_map(void *root_hva, struct kvm_vm *vm,
		uint64_t nested_paddr, uint64_t paddr, uint64_t size);
void nested_map_memslot(void *root_hva, struct kvm_vm *vm,
			uint32_t memslot);
void nested_identity_map_1g(void *root_hva, struct kvm_vm *vm,
			    uint64_t addr, uint64_t size);

#endif /* SELFTEST_KVM_NESTED_MAP_H */
