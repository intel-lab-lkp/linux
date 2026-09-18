// SPDX-License-Identifier: GPL-2.0-only
#include <errno.h>

#include "kvm_util.h"
#include "processor.h"
#include "hyperv.h"

#define TEST_MEM_GPA		0x100000000ull
#define READONLY_GPA		(TEST_MEM_GPA + PAGE_SIZE)
#define UNMAPPED_IN_GPA		(TEST_MEM_GPA + 2 * PAGE_SIZE)
#define UNMAPPED_OUT_GPA	(TEST_MEM_GPA + 3 * PAGE_SIZE)

struct hcall_test {
	/* TEST_MEM_GPA in the guest */
	u64 control;
	u64 ingpa;
	u64 outgpa;
	u64 expected_status;
	u8 unused[PAGE_SIZE - 32];

	/* READONLY_GPA in the guest */
	u8 readonly_data[];
};

#define HV_FLUSH_ALL_PROCESSORS                 BIT(0)
#define HV_FLUSH_ALL_VIRTUAL_ADDRESS_SPACES     BIT(1)

/* HvFlushVirtualAddressSpace, HvFlushVirtualAddressList hypercalls */
struct hv_tlb_flush {
        u64 address_space;
        u64 flags;
        u64 processor_mask;
        u64 gva_list[];
} __packed;

static void guest_code(gpa_t hcall_page, struct hcall_test *test)
{
	u64 result;
	u8 vector;

	wrmsr(HV_X64_MSR_GUEST_OS_ID, HYPERV_LINUX_OS_ID);
	wrmsr(HV_X64_MSR_HYPERCALL, hcall_page);

	vector = __hyperv_hypercall(test->control, test->ingpa, test->outgpa, &result);
	GUEST_ASSERT(!vector);
	GUEST_ASSERT_EQ(result & 0xffff, test->expected_status);
	GUEST_DONE();
}

static void clock_pairing_guest(gpa_t gpa)
{
	kvm_hypercall(KVM_HC_CLOCK_PAIRING, gpa, KVM_CLOCK_PAIRING_WALLCLOCK, 0, 0);
	GUEST_DONE();
}

static gva_t create_test_regions(struct kvm_vm *vm)
{
	gva_t test_gva;

	vm_userspace_mem_region_add(vm, VM_MEM_SRC_ANONYMOUS, TEST_MEM_GPA,
				    10, 1, 0);
	memset(addr_gpa2hva(vm, TEST_MEM_GPA), 0, PAGE_SIZE);

	vm_userspace_mem_region_add(vm, VM_MEM_SRC_ANONYMOUS, READONLY_GPA,
				    11, 1, KVM_MEM_READONLY);
	memset(addr_gpa2hva(vm, READONLY_GPA), 0, PAGE_SIZE);

	test_gva = vm_unused_gva_gap(vm, PAGE_SIZE * 2, 1 << 24);
	for (int i = 0; i < 2; i++)
		virt_pg_map(vm, test_gva + PAGE_SIZE * i, TEST_MEM_GPA + PAGE_SIZE * i);

	return test_gva;
}

static struct kvm_vm *create_vm(struct kvm_vcpu **vcpu, struct hcall_test **test,
				gpa_t *hcall_page_gpa)
{
	gva_t hcall_page, test_gva;
	struct kvm_vm *vm;

	vm = vm_create_with_one_vcpu(vcpu, guest_code);
	vcpu_set_hv_cpuid(*vcpu);
	vm_enable_cap(vm, KVM_CAP_HCALL_FAULT_EXIT, 1);

	hcall_page = vm_alloc_page(vm);
	memset(addr_gva2hva(vm, hcall_page), 0, PAGE_SIZE);
	*hcall_page_gpa = addr_gva2gpa(vm, hcall_page);

	test_gva = create_test_regions(vm);
	*test = addr_gva2hva(vm, test_gva);

	vcpu_args_set(*vcpu, 2, *hcall_page_gpa, test_gva);
	return vm;
}

static void run_vm(struct kvm_vcpu *vcpu)
{
	struct ucall uc;

	vcpu_run(vcpu);
	TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_IO);

	switch (get_ucall(vcpu, &uc)) {
	case UCALL_ABORT:
		REPORT_GUEST_ASSERT(uc);
		break;
	case UCALL_DONE:
		break;
	default:
		TEST_FAIL("Unexpected ucall: %lu", uc.cmd);
	}
}

static void test_unused_gpas(void)
{
	struct hcall_test *test;
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	gpa_t hcall_page;

	vm = create_vm(&vcpu, &test, &hcall_page);
	test->control = HVCALL_NOTIFY_LONG_SPIN_WAIT;
	test->ingpa = UNMAPPED_IN_GPA;
	test->outgpa = UNMAPPED_OUT_GPA;
	test->expected_status = HV_STATUS_SUCCESS;
	run_vm(vcpu);
	kvm_vm_free(vm);

	vm = create_vm(&vcpu, &test, &hcall_page);
	test->control = 0xbeef;
	test->ingpa = UNMAPPED_IN_GPA;
	test->outgpa = UNMAPPED_OUT_GPA;
	test->expected_status = HV_STATUS_INVALID_HYPERCALL_CODE;
	run_vm(vcpu);
	kvm_vm_free(vm);
}

static void assert_memory_fault(struct kvm_vcpu *vcpu, gpa_t gpa, u64 flags)
{
	int r;

	r = _vcpu_run(vcpu);
	TEST_ASSERT(r == -1 && errno == EFAULT, KVM_IOCTL_ERROR(KVM_RUN, r));
	TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_MEMORY_FAULT);
	TEST_ASSERT_EQ(vcpu->run->memory_fault.flags, flags);
	TEST_ASSERT_EQ(vcpu->run->memory_fault.gpa, gpa & PAGE_MASK);
	TEST_ASSERT_EQ(vcpu->run->memory_fault.size, PAGE_SIZE);
}

/* Read-only hypercalls, read-only inputs, valid/invalid argument */
static void test_readonly_input(void)
{
	struct hv_tlb_flush *flush;
	struct hcall_test *test;
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	gpa_t hcall_page;

	vm = create_vm(&vcpu, &test, &hcall_page);
	flush = (struct hv_tlb_flush *)addr_gpa2hva(vm, READONLY_GPA);
	flush->flags = HV_FLUSH_ALL_VIRTUAL_ADDRESS_SPACES | HV_FLUSH_ALL_PROCESSORS;
	flush->processor_mask = 0;
	test->control = HVCALL_FLUSH_VIRTUAL_ADDRESS_SPACE;
	test->ingpa = READONLY_GPA;
	test->outgpa = UNMAPPED_OUT_GPA;
	test->expected_status = HV_STATUS_SUCCESS;
	run_vm(vcpu);
	kvm_vm_free(vm);

	/* fails immediately because it requires SynIC */
	vm = create_vm(&vcpu, &test, &hcall_page);
	test->control = HVCALL_POST_MESSAGE;
	test->ingpa = UNMAPPED_IN_GPA;
	test->outgpa = UNMAPPED_OUT_GPA;
	test->expected_status = HV_STATUS_INVALID_HYPERCALL_INPUT;
	run_vm(vcpu);
	kvm_vm_free(vm);
}

/* Read-only hypercall, unmapped input */
static void test_input_fault(void)
{
	struct hcall_test *test;
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	gpa_t hcall_page;

	vm = create_vm(&vcpu, &test, &hcall_page);
	test->control = HVCALL_SIGNAL_EVENT;
	test->ingpa = UNMAPPED_IN_GPA;
	test->outgpa = UNMAPPED_OUT_GPA;
	assert_memory_fault(vcpu, test->ingpa, KVM_MEMORY_EXIT_FLAG_READ);
	kvm_vm_free(vm);
}

/* Read-write hypercall, unmapped or readonly input and output */
static void test_output_fault(bool readonly_in, bool readonly_out)
{
	struct hcall_test *test;
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	gpa_t hcall_page;

	vm = create_vm(&vcpu, &test, &hcall_page);
	test->control = HV_EXT_CALL_QUERY_CAPABILITIES;
	test->ingpa = readonly_in ? READONLY_GPA : UNMAPPED_IN_GPA;
	test->outgpa = readonly_out ? READONLY_GPA : UNMAPPED_OUT_GPA;

	assert_memory_fault(vcpu, test->outgpa, KVM_MEMORY_EXIT_FLAG_WRITE);
	kvm_vm_free(vm);
}

static void test_clock_pairing_fault(bool readonly)
{
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	gpa_t gpa;

	/* Unlike create_vm do not enable Hyper-V hypercalls. */
	vm = vm_create_with_one_vcpu(&vcpu, clock_pairing_guest);
	create_test_regions(vm);
	vm_enable_cap(vm, KVM_CAP_HCALL_FAULT_EXIT, 1);
	gpa = readonly ? READONLY_GPA : UNMAPPED_OUT_GPA;
	vcpu_args_set(vcpu, 1, gpa);

	assert_memory_fault(vcpu, gpa, KVM_MEMORY_EXIT_FLAG_WRITE);
	kvm_vm_free(vm);
}

int main(void)
{
	TEST_REQUIRE(kvm_has_cap(KVM_CAP_HYPERV_CPUID));
	TEST_REQUIRE(kvm_has_cap(KVM_CAP_HCALL_FAULT_EXIT));
	TEST_REQUIRE(kvm_cpuid_has(kvm_get_supported_hv_cpuid(),
				   HV_ENABLE_EXTENDED_HYPERCALLS));

	test_unused_gpas();
	test_readonly_input();
	test_input_fault();
	test_output_fault(false, false);
	test_output_fault(true, false);
	test_output_fault(false, true);
	test_clock_pairing_fault(false);
	test_clock_pairing_fault(true);
	return 0;
}
