/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Definition of structs for KVM lifecycle tests.
 *
 * Copyright IBM Corp. 2026
 *
 * Authors:
 *  Nitya Khamar (nkhamar@linux.ibm.com)
 *  Aaron Malik Brown (aaronmbr@linux.ibm.com)
 */
#ifndef SELFTEST_KVM_LIFECYCLE_H
#define SELFTEST_KVM_LIFECYCLE_H

#include "kvm_util.h"
#include <stdbool.h>

struct kvm_lifecycle_testdef {
	const char *name;
	bool (*test)(void);
	const char *skip_reason;
};

struct kvm_parameters {
	struct kvm_vm *vm;
	struct kvm_vcpu *vcpu;
	void *guest_code;
	struct kvm_run *run;
	unsigned int interrupt_type;
	struct kvm_s390_irq *irq;
	struct kvm_s390_irq_state *irq_state;
	struct {
		struct kvm_s390_sie_block *sie_block;
		struct kvm_s390_ucas_mapping map;
		bool immediate_exit_condition;
		void *vm_mem;
		int run_size;
		int vcpu_fd;
		int kvm_fd;
		int vm_fd;
	} manual_parameters;
};

#endif /* SELFTEST_KVM_LIFECYCLE_H */
