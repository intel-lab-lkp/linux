// SPDX-License-Identifier: GPL-2.0-only
/*
 * Test nested VMX context switching between multiple VMCS
 */

#include "test_util.h"
#include "kvm_util.h"
#include "processor.h"
#include "vmx.h"

#define L2_GUEST_STACK_SIZE 64
#define L2_VCPU_MAX 16

struct l2_vcpu_config {
	vm_vaddr_t hv_pages_gva;	/* Guest VA for eVMCS */
	vm_vaddr_t vmx_pages_gva;	/* Guest VA for VMX pages */
	unsigned long stack[L2_GUEST_STACK_SIZE];
	uint16_t vpid;
};

struct l1_test_config {
	struct l2_vcpu_config l2_vcpus[L2_VCPU_MAX];
	uint64_t hypercall_gpa;
	uint32_t nr_l2_vcpus;
	uint32_t nr_switches;
	bool enable_vpid;
	bool use_evmcs;
	bool sched_only;
};

static void l2_guest(void)
{
	while (1)
		vmcall();
}

static void run_l2_guest_evmcs(struct hyperv_test_pages *hv_pages,
			       struct vmx_pages *vmx,
			       void *guest_rip,
			       void *guest_rsp,
			       uint16_t vpid)
{
	GUEST_ASSERT(load_evmcs(hv_pages));
	prepare_vmcs(vmx, guest_rip, guest_rsp);
	current_evmcs->hv_enlightenments_control.msr_bitmap = 1;
	vmwrite(VIRTUAL_PROCESSOR_ID, vpid);

	GUEST_ASSERT(!vmlaunch());
	GUEST_ASSERT_EQ(vmreadz(VM_EXIT_REASON), EXIT_REASON_VMCALL);
	current_evmcs->guest_rip += 3;	/* vmcall */

	GUEST_ASSERT(!vmresume());
	GUEST_ASSERT_EQ(vmreadz(VM_EXIT_REASON), EXIT_REASON_VMCALL);
}

static void run_l2_guest_vmx_migrate(struct vmx_pages *vmx,
				     void *guest_rip,
				     void *guest_rsp,
				     uint16_t vpid,
				     bool start)
{
	uint32_t control;

	/*
	 * Emulate L2 vCPU migration: vmptrld/vmlaunch/vmclear
	 */

	if (start)
		GUEST_ASSERT(load_vmcs(vmx));
	else
		GUEST_ASSERT(!vmptrld(vmx->vmcs_gpa));

	prepare_vmcs(vmx, guest_rip, guest_rsp);

	control = vmreadz(CPU_BASED_VM_EXEC_CONTROL);
	control |= CPU_BASED_USE_MSR_BITMAPS;
	vmwrite(CPU_BASED_VM_EXEC_CONTROL, control);
	vmwrite(VIRTUAL_PROCESSOR_ID, vpid);

	GUEST_ASSERT(!vmlaunch());
	GUEST_ASSERT_EQ(vmreadz(VM_EXIT_REASON), EXIT_REASON_VMCALL);

	GUEST_ASSERT(vmptrstz() == vmx->vmcs_gpa);
	GUEST_ASSERT(!vmclear(vmx->vmcs_gpa));
}

static void run_l2_guest_vmx_sched(struct vmx_pages *vmx,
				   void *guest_rip,
				   void *guest_rsp,
				   uint16_t vpid,
				   bool start)
{
	/*
	 * Emulate L2 vCPU multiplexing: vmptrld/vmresume
	 */

	if (start) {
		uint32_t control;

		GUEST_ASSERT(load_vmcs(vmx));
		prepare_vmcs(vmx, guest_rip, guest_rsp);

		control = vmreadz(CPU_BASED_VM_EXEC_CONTROL);
		control |= CPU_BASED_USE_MSR_BITMAPS;
		vmwrite(CPU_BASED_VM_EXEC_CONTROL, control);
		vmwrite(VIRTUAL_PROCESSOR_ID, vpid);

		GUEST_ASSERT(!vmlaunch());
	} else {
		GUEST_ASSERT(!vmptrld(vmx->vmcs_gpa));
		GUEST_ASSERT(!vmresume());
	}

	GUEST_ASSERT_EQ(vmreadz(VM_EXIT_REASON), EXIT_REASON_VMCALL);

	vmwrite(GUEST_RIP,
		vmreadz(GUEST_RIP) + vmreadz(VM_EXIT_INSTRUCTION_LEN));
}

static void l1_guest_evmcs(struct l1_test_config *config)
{
	struct hyperv_test_pages *hv_pages;
	struct vmx_pages *vmx_pages;
	uint32_t i, j;

	/* Initialize Hyper-V MSRs */
	wrmsr(HV_X64_MSR_GUEST_OS_ID, HYPERV_LINUX_OS_ID);
	wrmsr(HV_X64_MSR_HYPERCALL, config->hypercall_gpa);

	/* Enable VP assist page */
	hv_pages = (struct hyperv_test_pages *)config->l2_vcpus[0].hv_pages_gva;
	enable_vp_assist(hv_pages->vp_assist_gpa, hv_pages->vp_assist);

	/* Enable evmcs */
	evmcs_enable();

	vmx_pages = (struct vmx_pages *)config->l2_vcpus[0].vmx_pages_gva;
	GUEST_ASSERT(prepare_for_vmx_operation(vmx_pages));

	for (i = 0; i < config->nr_switches; i++) {
		for (j = 0; j < config->nr_l2_vcpus; j++) {
			struct l2_vcpu_config *l2 = &config->l2_vcpus[j];

			hv_pages = (struct hyperv_test_pages *)l2->hv_pages_gva;
			vmx_pages = (struct vmx_pages *)l2->vmx_pages_gva;

			run_l2_guest_evmcs(hv_pages, vmx_pages, l2_guest,
					   &l2->stack[L2_GUEST_STACK_SIZE],
					   l2->vpid);
		}
	}

	GUEST_DONE();
}

static void l1_guest_vmx(struct l1_test_config *config)
{
	struct vmx_pages *vmx_pages;
	uint32_t i, j;

	vmx_pages = (struct vmx_pages *)config->l2_vcpus[0].vmx_pages_gva;
	GUEST_ASSERT(prepare_for_vmx_operation(vmx_pages));

	for (i = 0; i < config->nr_switches; i++) {
		for (j = 0; j < config->nr_l2_vcpus; j++) {
			struct l2_vcpu_config *l2 = &config->l2_vcpus[j];

			vmx_pages = (struct vmx_pages *)l2->vmx_pages_gva;

			if (config->sched_only)
				run_l2_guest_vmx_sched(vmx_pages, l2_guest,
						       &l2->stack[L2_GUEST_STACK_SIZE],
						       l2->vpid, i == 0);
			else
				run_l2_guest_vmx_migrate(vmx_pages, l2_guest,
							 &l2->stack[L2_GUEST_STACK_SIZE],
							 l2->vpid, i == 0);
		}
	}

	if (config->sched_only) {
		for (j = 0; j < config->nr_l2_vcpus; j++) {
			struct l2_vcpu_config *l2 = &config->l2_vcpus[j];

			vmx_pages = (struct vmx_pages *)l2->vmx_pages_gva;
			vmclear(vmx_pages->vmcs_gpa);
		}
	}

	GUEST_DONE();
}

static void vcpu_clone_hyperv_test_pages(struct kvm_vm *vm,
					 vm_vaddr_t src_gva,
					 vm_vaddr_t *dst_gva)
{
	struct hyperv_test_pages *src, *dst;
	vm_vaddr_t evmcs_gva;

	*dst_gva = vm_vaddr_alloc_page(vm);

	src = addr_gva2hva(vm, src_gva);
	dst = addr_gva2hva(vm, *dst_gva);
	memcpy(dst, src, sizeof(*dst));

	/* Allocate a new evmcs page */
	evmcs_gva = vm_vaddr_alloc_page(vm);
	dst->enlightened_vmcs = (void *)evmcs_gva;
	dst->enlightened_vmcs_hva = addr_gva2hva(vm, evmcs_gva);
	dst->enlightened_vmcs_gpa = addr_gva2gpa(vm, evmcs_gva);
}

static void prepare_vcpu(struct kvm_vm *vm, struct kvm_vcpu *vcpu,
			 uint32_t nr_l2_vcpus, uint32_t nr_switches,
			 bool enable_vpid, bool use_evmcs,
			 bool sched_only)
{
	vm_vaddr_t config_gva;
	struct l1_test_config *config;
	vm_vaddr_t hypercall_page_gva = 0;
	uint32_t i;

	TEST_ASSERT(nr_l2_vcpus <= L2_VCPU_MAX,
		    "Too many L2 vCPUs: %u (max %u)", nr_l2_vcpus, L2_VCPU_MAX);

	/* Allocate config structure in guest memory */
	config_gva = vm_vaddr_alloc(vm, sizeof(*config), 0x1000);
	config = addr_gva2hva(vm, config_gva);
	memset(config, 0, sizeof(*config));

	if (use_evmcs) {
		/* Allocate hypercall page */
		hypercall_page_gva = vm_vaddr_alloc_page(vm);
		memset(addr_gva2hva(vm, hypercall_page_gva), 0, getpagesize());
		config->hypercall_gpa = addr_gva2gpa(vm, hypercall_page_gva);

		/* Enable Hyper-V enlightenments */
		vcpu_set_hv_cpuid(vcpu);
		vcpu_enable_evmcs(vcpu);
	}

	/* Allocate resources for each L2 vCPU */
	for (i = 0; i < nr_l2_vcpus; i++) {
		vm_vaddr_t vmx_pages_gva;

		/* Allocate VMX pages (needed for both VMX and eVMCS) */
		vcpu_alloc_vmx(vm, &vmx_pages_gva);
		config->l2_vcpus[i].vmx_pages_gva = vmx_pages_gva;

		if (use_evmcs) {
			vm_vaddr_t hv_pages_gva;

			/* Allocate or clone hyperv_test_pages */
			if (i == 0) {
				vcpu_alloc_hyperv_test_pages(vm, &hv_pages_gva);
			} else {
				vm_vaddr_t first_hv_gva =
				    config->l2_vcpus[0].hv_pages_gva;
				vcpu_clone_hyperv_test_pages(vm, first_hv_gva,
							     &hv_pages_gva);
			}
			config->l2_vcpus[i].hv_pages_gva = hv_pages_gva;
		}

		/* Set VPID */
		config->l2_vcpus[i].vpid = enable_vpid ? (i + 3) : 0;
	}

	config->nr_l2_vcpus = nr_l2_vcpus;
	config->nr_switches = nr_switches;
	config->enable_vpid = enable_vpid;
	config->use_evmcs = use_evmcs;
	config->sched_only = use_evmcs ? false : sched_only;

	/* Pass single pointer to config structure */
	vcpu_args_set(vcpu, 1, config_gva);

	if (use_evmcs)
		vcpu_set_msr(vcpu, HV_X64_MSR_VP_INDEX, vcpu->id);
}

static bool opt_enable_vpid = true;
static const char *progname;

static void check_stats(struct kvm_vm *vm,
			uint32_t nr_l2_vcpus,
			uint32_t nr_switches,
			bool use_evmcs,
			bool sched_only)
{
	uint64_t reuse = 0;
	uint64_t recycle = 0;

	reuse = vm_get_stat(vm, nested_context_reuse);
	recycle = vm_get_stat(vm, nested_context_recycle);

	if (nr_l2_vcpus <= KVM_NESTED_OVERSUB_RATIO) {
		GUEST_ASSERT_EQ(reuse, nr_l2_vcpus * (nr_switches - 1));
		GUEST_ASSERT_EQ(recycle, 0);
	} else {
		if (sched_only) {
			/*
			 * When scheduling only no L2 vCPU vmcs is cleared so
			 * we reuse up to the max. number of contexts, but we
			 * cannot recycle any of them.
			 */
			GUEST_ASSERT_EQ(reuse,
					KVM_NESTED_OVERSUB_RATIO *
					(nr_switches - 1));
			GUEST_ASSERT_EQ(recycle, 0);
		} else {
			/*
			 * When migration we cycle in LRU order so no context
			 * can be reused they are all recycled.
			 */
			GUEST_ASSERT_EQ(reuse, 0);
			GUEST_ASSERT_EQ(recycle,
					(nr_l2_vcpus * nr_switches) -
					KVM_NESTED_OVERSUB_RATIO);
		}
	}

	printf("%s %u switches with %u L2 vCPUS (%s) reuse %" PRIu64
	       " recycle %" PRIu64 "\n", progname, nr_switches, nr_l2_vcpus,
	       use_evmcs ? "evmcs" : (sched_only ? "vmx sched" : "vmx migrate"),
	       reuse, recycle);
}

static void run_test(uint32_t nr_l2_vcpus, uint32_t nr_switches,
		     bool use_evmcs, bool sched_only)
{
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	struct ucall uc;

	vm = vm_create_with_one_vcpu(&vcpu, use_evmcs
				     ? l1_guest_evmcs : l1_guest_vmx);

	prepare_vcpu(vm, vcpu, nr_l2_vcpus, nr_switches,
		     opt_enable_vpid, use_evmcs, sched_only);

	for (;;) {
		vcpu_run(vcpu);
		TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_IO);

		switch (get_ucall(vcpu, &uc)) {
		case UCALL_DONE:
			goto done;
		case UCALL_ABORT:
			REPORT_GUEST_ASSERT(uc);
		default:
			TEST_FAIL("Unexpected ucall: %lu", uc.cmd);
		}
	}

done:
	check_stats(vm, nr_l2_vcpus, nr_switches, use_evmcs, sched_only);
	kvm_vm_free(vm);
}

int main(int argc, char *argv[])
{
	uint32_t opt_nr_l2_vcpus = 0;
	uint32_t opt_nr_switches = 0;
	bool opt_sched_only = true;
	int opt;
	int i;

	TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_VMX));

	progname = argv[0];

	while ((opt = getopt(argc, argv, "c:rs:v")) != -1) {
		switch (opt) {
		case 'c':
			opt_nr_l2_vcpus = atoi_paranoid(optarg);
			break;
		case 'r':
			opt_sched_only = false;
			break;
		case 's':
			opt_nr_switches = atoi_paranoid(optarg);
			break;
		case 'v':
			opt_enable_vpid = false;
			break;
		default:
			break;
		}
	}

	if (opt_nr_l2_vcpus && opt_nr_switches) {
		run_test(opt_nr_l2_vcpus, opt_nr_switches, false,
			 opt_sched_only);

		if (kvm_has_cap(KVM_CAP_HYPERV_ENLIGHTENED_VMCS))
			run_test(opt_nr_l2_vcpus, opt_nr_switches,
				 true, false);
	} else {
		/* VMX vmlaunch */
		for (i = 2; i <= 16; i++)
			run_test(i, 4, false, false);

		/* VMX vmresume */
		for (i = 2; i <= 16; i++)
			run_test(i, 4, false, true);

		/* eVMCS */
		if (kvm_has_cap(KVM_CAP_HYPERV_ENLIGHTENED_VMCS)) {
			for (i = 2; i <= 16; i++)
				run_test(i, 4, true, false);
		}
	}

	return 0;
}
