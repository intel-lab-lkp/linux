// SPDX-License-Identifier: GPL-2.0-only

#ifndef SELFTEST_KVM_HELPERS_H
#define SELFTEST_KVM_HELPERS_H

#include "kvm_util.h"
#include "processor.h"

static inline void __handle_ucall(struct kvm_vcpu *vcpu, uint64_t expect, struct ucall *uc)
{
	uint64_t ret;
	struct kvm_regs regs;

	ret = get_ucall(vcpu, uc);
	if (ret == expect)
		return;

	vcpu_regs_get(vcpu, &regs);
	fprintf(stderr, "Guest failure at NIA:0x%016llx MSR:0x%016llx\n", regs.pc, regs.msr);
	fprintf(stderr, "Expected ucall: %lu\n", expect);

	if (ret == UCALL_ABORT)
		REPORT_GUEST_ASSERT(*uc);
	else
		TEST_FAIL("Unexpected ucall: %lu exit_reason=%s",
			ret, exit_reason_str(vcpu->run->exit_reason));
}

static inline void handle_ucall(struct kvm_vcpu *vcpu, uint64_t expect)
{
	struct ucall uc;

	__handle_ucall(vcpu, expect, &uc);
}

static inline void host_sync(struct kvm_vcpu *vcpu, uint64_t sync)
{
	struct ucall uc;

	__handle_ucall(vcpu, UCALL_SYNC, &uc);

	TEST_ASSERT(uc.args[1] == (sync), "Sync failed host:%ld guest:%ld",
						(long)sync, (long)uc.args[1]);
}

#endif
