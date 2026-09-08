// SPDX-License-Identifier: GPL-2.0-only
#include <stddef.h>
#include <stdint.h>

#include "kvm_util.h"
#include "processor.h"
#include "sev.h"
#include "svm_util.h"

#define GHCB_SAVE_RAX_OFFSET		0x1f8
#define GHCB_SAVE_SW_EXIT_CODE_OFFSET	0x390
#define GHCB_SAVE_SW_EXIT_INFO_1_OFFSET	0x398
#define GHCB_SAVE_SW_EXIT_INFO_2_OFFSET	0x3a0
#define GHCB_SAVE_VALID_BITMAP_OFFSET	0x3f0

#define GHCB_MSR_REG_GPA_REQ		0x012
#define GHCB_MSR_REG_GPA_RESP		0x013
#define GHCB_MSR_INFO_MASK		GENMASK_ULL(11, 0)

#define GHCB_HV_RESP_MALFORMED_INPUT	2
#define GHCB_ERR_MISSING_INPUT		4
#define GHCB_ERR_INVALID_INPUT		5

struct apic_id_desc {
	u32 nr_entries;
	u32 apic_ids[];
};

struct apic_id_results {
	u64 missing_info1;
	u64 missing_info2;
	u64 zero_info1;
	u64 zero_info2;
	u64 zero_rax;
	u64 invalid_info1;
	u64 invalid_info2;
	u64 valid_info1;
	u64 valid_info2;
	u32 nr_entries;
	u32 first_apic_id;
	u32 last_apic_id;
};

static void ghcb_set_field(void *ghcb, size_t offset, u64 value, bool valid)
{
	u8 *valid_bitmap = ghcb + GHCB_SAVE_VALID_BITMAP_OFFSET;

	*(u64 *)(ghcb + offset) = value;
	if (valid)
		valid_bitmap[(offset / sizeof(u64)) / 8] |=
			BIT((offset / sizeof(u64)) % 8);
}

static u64 ghcb_get_field(void *ghcb, size_t offset)
{
	return *(u64 *)(ghcb + offset);
}

static void do_get_apic_ids(void *ghcb, gpa_t buffer_gpa, u64 pages,
			    bool rax_valid)
{
	memset(ghcb, 0, PAGE_SIZE);
	ghcb_set_field(ghcb, GHCB_SAVE_SW_EXIT_CODE_OFFSET,
		       SVM_VMGEXIT_GET_APIC_IDS, true);
	ghcb_set_field(ghcb, GHCB_SAVE_SW_EXIT_INFO_1_OFFSET, buffer_gpa, true);
	ghcb_set_field(ghcb, GHCB_SAVE_SW_EXIT_INFO_2_OFFSET, 0, true);
	ghcb_set_field(ghcb, GHCB_SAVE_RAX_OFFSET, pages, rax_valid);
	vmgexit();
}

static void guest_code(void *ghcb, gpa_t ghcb_gpa, void *list,
		       gpa_t list_gpa, struct apic_id_results *results,
		       u64 expected_vcpus)
{
	struct apic_id_desc *desc = list;
	u64 msr;

	wrmsr(MSR_AMD64_SEV_ES_GHCB,
	      (ghcb_gpa >> PAGE_SHIFT) << PAGE_SHIFT | GHCB_MSR_REG_GPA_REQ);
	vmgexit();
	msr = rdmsr(MSR_AMD64_SEV_ES_GHCB);
	if ((msr & GHCB_MSR_INFO_MASK) != GHCB_MSR_REG_GPA_RESP)
		goto terminate;

	wrmsr(MSR_AMD64_SEV_ES_GHCB, ghcb_gpa);

	do_get_apic_ids(ghcb, list_gpa, 1, false);
	results->missing_info1 = ghcb_get_field(ghcb, GHCB_SAVE_SW_EXIT_INFO_1_OFFSET);
	results->missing_info2 = ghcb_get_field(ghcb, GHCB_SAVE_SW_EXIT_INFO_2_OFFSET);

	do_get_apic_ids(ghcb, list_gpa, 0, true);
	results->zero_info1 = ghcb_get_field(ghcb, GHCB_SAVE_SW_EXIT_INFO_1_OFFSET);
	results->zero_info2 = ghcb_get_field(ghcb, GHCB_SAVE_SW_EXIT_INFO_2_OFFSET);
	results->zero_rax = ghcb_get_field(ghcb, GHCB_SAVE_RAX_OFFSET);

	do_get_apic_ids(ghcb, BIT_ULL(52), 2, true);
	results->invalid_info1 = ghcb_get_field(ghcb, GHCB_SAVE_SW_EXIT_INFO_1_OFFSET);
	results->invalid_info2 = ghcb_get_field(ghcb, GHCB_SAVE_SW_EXIT_INFO_2_OFFSET);

	do_get_apic_ids(ghcb, list_gpa, 2, true);
	results->valid_info1 = ghcb_get_field(ghcb, GHCB_SAVE_SW_EXIT_INFO_1_OFFSET);
	results->valid_info2 = ghcb_get_field(ghcb, GHCB_SAVE_SW_EXIT_INFO_2_OFFSET);
	results->nr_entries = desc->nr_entries;
	results->first_apic_id = desc->apic_ids[0];
	results->last_apic_id = desc->apic_ids[expected_vcpus - 1];

terminate:
	wrmsr(MSR_AMD64_SEV_ES_GHCB, GHCB_MSR_TERM_REQ);
	vmgexit();
}

static void run_apic_id_test(unsigned int nr_vcpus)
{
	struct apic_id_results *results;
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	gva_t ghcb_gva, list_gva, results_gva;
	gpa_t ghcb_gpa, list_gpa;
	unsigned int i;

	kvm_set_files_rlimit(nr_vcpus);
	vm = vm_sev_create_with_one_vcpu(KVM_X86_SNP_VM, guest_code, &vcpu);
	for (i = 1; i < nr_vcpus; i++)
		__vm_vcpu_add(vm, i);

	ghcb_gva = vm_alloc_shared(vm, PAGE_SIZE, KVM_UTIL_MIN_VADDR,
				   MEM_REGION_TEST_DATA);
	list_gva = vm_alloc_shared(vm, 2 * PAGE_SIZE, KVM_UTIL_MIN_VADDR,
				   MEM_REGION_TEST_DATA);
	results_gva = vm_alloc_shared(vm, PAGE_SIZE, KVM_UTIL_MIN_VADDR,
				      MEM_REGION_TEST_DATA);
	ghcb_gpa = addr_gva2gpa(vm, ghcb_gva);
	list_gpa = addr_gva2gpa(vm, list_gva);
	results = addr_gva2hva(vm, results_gva);

	vcpu_args_set(vcpu, 6, ghcb_gva, ghcb_gpa, list_gva, list_gpa,
		      results_gva, nr_vcpus);
	memset(addr_gva2hva(vm, ghcb_gva), 0, PAGE_SIZE);
	memset(addr_gva2hva(vm, list_gva), 0, 2 * PAGE_SIZE);
	memset(results, 0, PAGE_SIZE);
	vm_sev_launch(vm, snp_default_policy(), NULL);

	vcpu_run(vcpu);
	TEST_ASSERT_EQ(vcpu->run->exit_reason, KVM_EXIT_SYSTEM_EVENT);
	TEST_ASSERT_EQ(vcpu->run->system_event.type, KVM_SYSTEM_EVENT_SEV_TERM);

	TEST_ASSERT_EQ(results->missing_info1, GHCB_HV_RESP_MALFORMED_INPUT);
	TEST_ASSERT_EQ(results->missing_info2, GHCB_ERR_MISSING_INPUT);
	TEST_ASSERT_EQ(results->zero_info1, 0);
	TEST_ASSERT_EQ(results->zero_info2, 0);
	TEST_ASSERT_EQ(results->zero_rax,
		       DIV_ROUND_UP(sizeof(struct apic_id_desc) + nr_vcpus * sizeof(u32),
				    PAGE_SIZE));
	TEST_ASSERT_EQ(results->invalid_info1, GHCB_HV_RESP_MALFORMED_INPUT);
	TEST_ASSERT_EQ(results->invalid_info2, GHCB_ERR_INVALID_INPUT);
	TEST_ASSERT_EQ(results->valid_info1, 0);
	TEST_ASSERT_EQ(results->valid_info2, 0);
	TEST_ASSERT_EQ(results->nr_entries, nr_vcpus);
	TEST_ASSERT_EQ(results->first_apic_id, 0);
	TEST_ASSERT_EQ(results->last_apic_id, nr_vcpus - 1);

	kvm_vm_free(vm);
}

int main(int argc, char *argv[])
{
	unsigned int max_vcpus;

	TEST_REQUIRE(kvm_check_cap(KVM_CAP_VM_TYPES) & BIT(KVM_X86_SNP_VM));
	run_apic_id_test(2);

	/* 1024 IDs cross the one-page descriptor boundary. */
	max_vcpus = kvm_check_cap(KVM_CAP_MAX_VCPUS);
	if (max_vcpus >= 1024)
		run_apic_id_test(1024);
	else
		pr_info("Skipping vCPU-count boundary test (max vCPUs: %u)\n", max_vcpus);

	return 0;
}
