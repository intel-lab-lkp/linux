// SPDX-License-Identifier: GPL-2.0-only
/*
 * xen_vmcall_test
 *
 * Copyright © 2020 Amazon.com, Inc. or its affiliates.
 *
 * Userspace hypercall testing
 */

#include "test_util.h"
#include "kvm_util.h"
#include "processor.h"
#include "hyperv.h"

#include <string.h>

#define HCALL_REGION_GPA	0xc0000000ULL
#define HCALL_REGION_SLOT	10

#define INPUTVALUE 17
#define ARGVALUE(x) (0xdeadbeef5a5a0000UL + x)
#define RETVALUE 0xcafef00dfbfbffffUL

#define XEN_HYPERCALL_MSR	0x40000200
#define HV_GUEST_OS_ID_MSR	0x40000000
#define HV_HYPERCALL_MSR	0x40000001

#define HVCALL_SIGNAL_EVENT		0x005d
#define HV_STATUS_INVALID_ALIGNMENT	4

enum {
	TEST_WRITE_HYPERCALL_PAGE = 1,
};

static void guest_code(void)
{
	unsigned long rax = INPUTVALUE;
	unsigned long rdi = ARGVALUE(1);
	unsigned long rsi = ARGVALUE(2);
	unsigned long rdx = ARGVALUE(3);
	unsigned long rcx;
	register unsigned long r10 __asm__("r10") = ARGVALUE(4);
	register unsigned long r8 __asm__("r8") = ARGVALUE(5);
	register unsigned long r9 __asm__("r9") = ARGVALUE(6);

	/* First a direct invocation of 'vmcall' */
	__asm__ __volatile__("vmcall" :
			     "=a"(rax) :
			     "a"(rax), "D"(rdi), "S"(rsi), "d"(rdx),
			     "r"(r10), "r"(r8), "r"(r9));
	GUEST_ASSERT(rax == RETVALUE);

	/* Fill in the Xen hypercall page */
	__asm__ __volatile__("wrmsr" : : "c" (XEN_HYPERCALL_MSR),
			     "a" (HCALL_REGION_GPA & 0xffffffff),
			     "d" (HCALL_REGION_GPA >> 32));

	/* Set Hyper-V Guest OS ID */
	__asm__ __volatile__("wrmsr" : : "c" (HV_GUEST_OS_ID_MSR),
			     "a" (0x5a), "d" (0));

	/* Hyper-V hypercall page */
	u64 msrval = HCALL_REGION_GPA + PAGE_SIZE + 1;
	__asm__ __volatile__("wrmsr" : : "c" (HV_HYPERCALL_MSR),
			     "a" (msrval & 0xffffffff),
			     "d" (msrval >> 32));

	/* Invoke a Xen hypercall */
	__asm__ __volatile__("call *%1" : "=a"(rax) :
			     "r"(HCALL_REGION_GPA + INPUTVALUE * 32),
			     "a"(rax), "D"(rdi), "S"(rsi), "d"(rdx),
			     "r"(r10), "r"(r8), "r"(r9));
	GUEST_ASSERT(rax == RETVALUE);

	/* Invoke a Hyper-V hypercall */
	rax = 0;
	rcx = HVCALL_SIGNAL_EVENT;	/* code */
	rdx = 0x5a5a5a5a;		/* ingpa (badly aligned) */
	__asm__ __volatile__("call *%1" : "=a"(rax) :
			     "r"(HCALL_REGION_GPA + PAGE_SIZE),
			     "a"(rax), "c"(rcx), "d"(rdx),
			     "r"(r8));
	GUEST_ASSERT(rax == HV_STATUS_INVALID_ALIGNMENT);

	/*
	 * Test KVM_XEN_VCPU_ATTR_TYPE_WRITE_HYPERCALL_PAGE: ask userspace
	 * to set up MSR filtering, then write the MSR. The WRMSR will exit
	 * to userspace (not populate the page). Userspace verifies the page
	 * is empty, uses the attr to populate it, then resumes us.
	 */
	GUEST_SYNC(TEST_WRITE_HYPERCALL_PAGE);

	__asm__ __volatile__("wrmsr" : : "c" (XEN_HYPERCALL_MSR),
			     "a" (HCALL_REGION_GPA & 0xffffffff),
			     "d" (HCALL_REGION_GPA >> 32));

	/* Userspace populated the page via the attr — verify it works */
	rax = INPUTVALUE;
	rdi = ARGVALUE(1);
	rsi = ARGVALUE(2);
	rdx = ARGVALUE(3);
	r10 = ARGVALUE(4);
	r8 = ARGVALUE(5);
	r9 = ARGVALUE(6);
	__asm__ __volatile__("call *%1" : "=a"(rax) :
			     "r"(HCALL_REGION_GPA + INPUTVALUE * 32),
			     "a"(rax), "D"(rdi), "S"(rsi), "d"(rdx),
			     "r"(r10), "r"(r8), "r"(r9));
	GUEST_ASSERT(rax == RETVALUE);

	GUEST_DONE();
}

static void setup_msr_filter(struct kvm_vm *vm)
{
	uint64_t deny_bits = 0;
	struct kvm_msr_filter filter = {
		.flags = KVM_MSR_FILTER_DEFAULT_ALLOW,
		.ranges = {
			{
				.flags = KVM_MSR_FILTER_WRITE,
				.nmsrs = 1,
				.base = XEN_HYPERCALL_MSR,
				.bitmap = (uint8_t *)&deny_bits,
			},
		},
	};

	vm_ioctl(vm, KVM_X86_SET_MSR_FILTER, &filter);
}

int main(int argc, char *argv[])
{
	unsigned int xen_caps;
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	bool msr_filter_ready = false;

	xen_caps = kvm_check_cap(KVM_CAP_XEN_HVM);
	TEST_REQUIRE(xen_caps & KVM_XEN_HVM_CONFIG_INTERCEPT_HCALL);
	TEST_REQUIRE(xen_caps & KVM_XEN_HVM_CONFIG_WRITE_HYPERCALL_PAGE);
	TEST_REQUIRE(kvm_check_cap(KVM_CAP_X86_USER_SPACE_MSR));
	TEST_REQUIRE(kvm_check_cap(KVM_CAP_X86_MSR_FILTER));

	vm = vm_create_with_one_vcpu(&vcpu, guest_code);
	vcpu_set_hv_cpuid(vcpu);

	struct kvm_xen_hvm_config hvmc = {
		.flags = KVM_XEN_HVM_CONFIG_INTERCEPT_HCALL,
		.msr = XEN_HYPERCALL_MSR,
	};
	vm_ioctl(vm, KVM_XEN_HVM_CONFIG, &hvmc);

	/* Map a region for the hypercall pages */
	vm_userspace_mem_region_add(vm, VM_MEM_SRC_ANONYMOUS,
				    HCALL_REGION_GPA, HCALL_REGION_SLOT, 2, 0);
	virt_map(vm, HCALL_REGION_GPA, HCALL_REGION_GPA, 2);

	for (;;) {
		volatile struct kvm_run *run = vcpu->run;
		struct ucall uc;

		vcpu_run(vcpu);

		if (run->exit_reason == KVM_EXIT_XEN) {
			TEST_ASSERT_EQ(run->xen.type, KVM_EXIT_XEN_HCALL);
			TEST_ASSERT_EQ(run->xen.u.hcall.cpl, 0);
			TEST_ASSERT_EQ(run->xen.u.hcall.longmode, 1);
			TEST_ASSERT_EQ(run->xen.u.hcall.input, INPUTVALUE);
			TEST_ASSERT_EQ(run->xen.u.hcall.params[0], ARGVALUE(1));
			TEST_ASSERT_EQ(run->xen.u.hcall.params[1], ARGVALUE(2));
			TEST_ASSERT_EQ(run->xen.u.hcall.params[2], ARGVALUE(3));
			TEST_ASSERT_EQ(run->xen.u.hcall.params[3], ARGVALUE(4));
			TEST_ASSERT_EQ(run->xen.u.hcall.params[4], ARGVALUE(5));
			TEST_ASSERT_EQ(run->xen.u.hcall.params[5], ARGVALUE(6));
			run->xen.u.hcall.result = RETVALUE;
			continue;
		}

		if (run->exit_reason == KVM_EXIT_X86_WRMSR) {
			/* MSR filter caught the Xen hypercall MSR write */
			TEST_ASSERT(msr_filter_ready,
				    "Unexpected WRMSR exit before filter setup");
			TEST_ASSERT_EQ(run->msr.index, XEN_HYPERCALL_MSR);

			/*
			 * The host_initiated check should have prevented
			 * KVM from populating the page. Verify it's empty.
			 */
			uint8_t *hcall_page = addr_gpa2hva(vm, HCALL_REGION_GPA);
			TEST_ASSERT_EQ(hcall_page[0], 0);

			/*
			 * Now use the attr to populate the page, as a
			 * VMM would after intercepting the MSR write.
			 */
			struct kvm_xen_vcpu_attr attr = {
				.type = KVM_XEN_VCPU_ATTR_TYPE_WRITE_HYPERCALL_PAGE,
				.u.gpa = HCALL_REGION_GPA,
			};
			vcpu_ioctl(vcpu, KVM_XEN_VCPU_SET_ATTR, &attr);

			/* Verify the page is now populated */
			TEST_ASSERT_EQ(hcall_page[0], 0xb8);

			run->msr.error = 0;
			continue;
		}

		TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_IO);

		switch (get_ucall(vcpu, &uc)) {
		case UCALL_ABORT:
			REPORT_GUEST_ASSERT(uc);
			/* NOT REACHED */
		case UCALL_SYNC:
			TEST_ASSERT_EQ(uc.args[1], TEST_WRITE_HYPERCALL_PAGE);

			/*
			 * Guest is about to write the Xen MSR. Clear the
			 * hypercall page, install MSR filter to intercept
			 * the write, and enable userspace MSR exits.
			 */
			memset(addr_gpa2hva(vm, HCALL_REGION_GPA), 0, PAGE_SIZE);
			vm_enable_cap(vm, KVM_CAP_X86_USER_SPACE_MSR,
				      KVM_MSR_EXIT_REASON_FILTER);
			setup_msr_filter(vm);
			msr_filter_ready = true;
			break;
		case UCALL_DONE:
			goto done;
		default:
			TEST_FAIL("Unknown ucall 0x%lx.", uc.cmd);
		}
	}
done:
	kvm_vm_free(vm);
	return 0;
}
