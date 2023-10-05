/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Macros for defining a KVM test
 *
 * Copyright (C) 2022, Google LLC.
 */

#ifndef SELFTEST_KVM_TEST_HARNESS_H
#define SELFTEST_KVM_TEST_HARNESS_H

#include "kselftest_harness.h"

#define KVM_ONE_VCPU_TEST_SUITE(name, guest_code)			\
	FIXTURE(name) {							\
		struct kvm_vcpu *vcpu;					\
	};								\
									\
	FIXTURE_SETUP(name) {						\
		(void)vm_create_with_one_vcpu(&self->vcpu, guest_code);	\
	}								\
									\
	FIXTURE_TEARDOWN(name) {					\
		kvm_vm_free(self->vcpu->vm);				\
	}

#define KVM_ONE_VCPU_TEST(suite, test)					\
static void __suite##_##test(struct kvm_vcpu *vcpu);			\
									\
TEST_F(suite, test)							\
{									\
	__suite##_##test(self->vcpu);					\
}									\
static void __suite##_##test(struct kvm_vcpu *vcpu)

#endif /* SELFTEST_KVM_TEST_HARNESS_H */
