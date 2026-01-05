// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitmap.h>

#include "test_util.h"
#include "kvm_util.h"
#include "sev.h"

#define GUEST_NR_PAGES (1024)
#define DEFAULT_GUEST_TEST_MEM 0xC0000000
#define TEST_MEM_SLOT_INDEX 1

/*
 * Guest/Host shared variables.
 */
static uint64_t guest_page_size;
static uint64_t guest_num_pages;

/* Points to the test VM memory region on which we track dirty logs */
static void *host_test_mem;

/* Host variables */
static pthread_t vcpu_thread;
static bool vcpu_thread_done;

/*
 * Guest physical memory offset of the testing memory slot.
 * This will be set to the topmost valid physical address minus
 * the test memory size.
 */
static uint64_t guest_test_phys_mem;

/*
 * Guest virtual memory offset of the testing memory slot.
 * Must not conflict with identity mapped test code.
 */
static uint64_t guest_test_virt_mem = DEFAULT_GUEST_TEST_MEM;

/*
 * Continuously write to the first 8 bytes of a random pages within
 * the testing memory region.
 */
static void guest_pml_code(void)
{
	uint64_t addr;
	int write = 0;

	while (write++ != (guest_num_pages * 10)) {
		addr = guest_test_virt_mem;
		addr += (guest_random_u64(&guest_rng) % guest_num_pages) * guest_page_size;

		vcpu_arch_put_guest(*(uint64_t *)addr, 0xAA);
	}
}

static void guest_pml_sev_code(void)
{
	GUEST_ASSERT(rdmsr(MSR_AMD64_SEV) & MSR_AMD64_SEV_ENABLED);

	guest_pml_code();

	GUEST_DONE();
}

static void guest_pml_sev_es_code(void)
{
	GUEST_ASSERT(rdmsr(MSR_AMD64_SEV) & MSR_AMD64_SEV_ENABLED);
	GUEST_ASSERT(rdmsr(MSR_AMD64_SEV) & MSR_AMD64_SEV_ES_ENABLED);

	guest_pml_code();

	wrmsr(MSR_AMD64_SEV_ES_GHCB, GHCB_MSR_TERM_REQ);
	vmgexit();
}

static void guest_pml_sev_snp_code(void)
{
	GUEST_ASSERT(rdmsr(MSR_AMD64_SEV) & MSR_AMD64_SEV_ENABLED);
	GUEST_ASSERT(rdmsr(MSR_AMD64_SEV) & MSR_AMD64_SEV_ES_ENABLED);
	GUEST_ASSERT(rdmsr(MSR_AMD64_SEV) & MSR_AMD64_SEV_SNP_ENABLED);

	guest_pml_code();

	wrmsr(MSR_AMD64_SEV_ES_GHCB, GHCB_MSR_TERM_REQ);
	vmgexit();
}

static unsigned long *bmap;
static void *vcpu_worker(void *data)
{
	struct kvm_vcpu *vcpu = data;
	struct kvm_vm *vm;
	struct ucall uc;

	vm = vcpu->vm;
	while (1) {
		/* Let the guest dirty the random pages */
		vcpu_run(vcpu);

		if (is_sev_es_vm(vm)) {
			TEST_ASSERT(vcpu->run->exit_reason == KVM_EXIT_SYSTEM_EVENT,
				    "Wanted SYSTEM_EVENT, got %s",
				    exit_reason_str(vcpu->run->exit_reason));
			TEST_ASSERT_EQ(vcpu->run->system_event.type, KVM_SYSTEM_EVENT_SEV_TERM);
			TEST_ASSERT_EQ(vcpu->run->system_event.ndata, 1);
			TEST_ASSERT_EQ(vcpu->run->system_event.data[0], GHCB_MSR_TERM_REQ);
			break;
		}

		switch (get_ucall(vcpu, &uc)) {
		case UCALL_SYNC:
			continue;
		case UCALL_DONE:
			goto exit_done;
		case UCALL_ABORT:
			REPORT_GUEST_ASSERT(uc);
		default:
			TEST_FAIL("Unexpected exit: %s", exit_reason_str(vcpu->run->exit_reason));
		}
	}

exit_done:
	WRITE_ONCE(vcpu_thread_done, true);
	return NULL;
}

static void vm_dirty_log_verify(void)
{
	uint64_t page, nr_dirty_pages = 0, nr_clean_pages = 0;

	for (page = 0; page < guest_num_pages; page++) {
		uint64_t val = *(uint64_t *)(host_test_mem + page * guest_page_size);
		bool bmap_dirty = __test_and_clear_bit(page, bmap);

		if (bmap_dirty && val == 0xAA)
			nr_dirty_pages++;
		else
			nr_clean_pages++;
	}
	pr_debug("Dirty pages %ld clean pages %ld\n", nr_dirty_pages, nr_clean_pages);
}

void test_pml(void *guest_code, uint32_t type, uint64_t policy)
{
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;

	vm = vm_sev_create_with_one_vcpu_extramem(type, guest_code, &vcpu, 2 * GUEST_NR_PAGES);

	guest_page_size = vm->page_size;
	guest_num_pages = GUEST_NR_PAGES;
	guest_test_phys_mem = (vm->max_gfn - guest_num_pages) * guest_page_size;

	bmap = bitmap_zalloc(guest_num_pages);

	/* Add an extra memory slot for testing dirty logging */
	vm_userspace_mem_region_add(vm, VM_MEM_SRC_ANONYMOUS,
				guest_test_phys_mem,
				TEST_MEM_SLOT_INDEX,
				guest_num_pages,
				KVM_MEM_LOG_DIRTY_PAGES);

	/* Do mapping for the dirty track memory slot */
	virt_map(vm, guest_test_virt_mem, guest_test_phys_mem, guest_num_pages);
	host_test_mem = addr_gpa2hva(vm, (vm_paddr_t)guest_test_phys_mem);

	/* Export the shared variables to the guest */
	sync_global_to_guest(vm, guest_page_size);
	sync_global_to_guest(vm, guest_test_virt_mem);
	sync_global_to_guest(vm, guest_num_pages);

	WRITE_ONCE(vcpu_thread_done, false);
	vm_sev_launch(vm, policy, NULL);

	pthread_create(&vcpu_thread, NULL, vcpu_worker, vcpu);
	while (!READ_ONCE(vcpu_thread_done)) {
		usleep(1000);
		kvm_vm_get_dirty_log(vcpu->vm, TEST_MEM_SLOT_INDEX, bmap);
	}
	pthread_join(vcpu_thread, NULL);

	vm_dirty_log_verify();
	free(bmap);

	kvm_vm_free(vm);
}

int main(int argc, char *argv[])
{
	TEST_REQUIRE(get_kvm_amd_param_bool("pml"));

	if (kvm_cpu_has(X86_FEATURE_SEV))
		test_pml(guest_pml_sev_code, KVM_X86_SEV_VM, SEV_POLICY_NO_DBG);

	if (kvm_cpu_has(X86_FEATURE_SEV_ES))
		test_pml(guest_pml_sev_es_code, KVM_X86_SEV_ES_VM,
			 SEV_POLICY_ES | SEV_POLICY_NO_DBG);

	if (kvm_cpu_has(X86_FEATURE_SEV_SNP))
		test_pml(guest_pml_sev_snp_code, KVM_X86_SNP_VM,
			 snp_default_policy() | SNP_POLICY_DBG);

	return 0;
}
