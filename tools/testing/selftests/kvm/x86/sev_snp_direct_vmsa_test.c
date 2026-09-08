// SPDX-License-Identifier: GPL-2.0-only
#include <errno.h>
#include <stdlib.h>

#include "kvm_util.h"
#include "processor.h"
#include "sev.h"
#include "svm_util.h"

#define DIRECT_MARKER	0x444952454354564dULL
#define LEGACY_MARKER	0x4c4547414359564dULL
#define SNP_ACTIVE_SEV_FEATURE	BIT_ULL(0)
#define VMSA_PMD_SIZE	BIT_ULL(21)
#define VMSA_MIN_GPA	(VMSA_PMD_SIZE + PAGE_SIZE)

struct test_vmsa {
	struct vmcb_seg es, cs, ss, ds, fs, gs;
	struct vmcb_seg gdtr, ldtr, idtr, tr;
	u64 pl0_ssp, pl1_ssp, pl2_ssp, pl3_ssp;
	u64 u_cet;
	u8 reserved_0xc8[2];
	u8 vmpl;
	u8 cpl;
	u8 reserved_0xcc[4];
	u64 efer;
	u8 reserved_0xd8[104];
	u64 xss;
	u64 cr4, cr3, cr0, dr7, dr6, rflags, rip;
	u64 dr0, dr1, dr2, dr3;
	u64 dr0_addr_mask, dr1_addr_mask, dr2_addr_mask, dr3_addr_mask;
	u8 reserved_0x1c0[24];
	u64 rsp, s_cet, ssp, isst_addr, rax;
	u64 star, lstar, cstar, sfmask, kernel_gs_base;
	u64 sysenter_cs, sysenter_esp, sysenter_eip, cr2;
	u8 reserved_0x248[32];
	u64 g_pat, dbgctl, br_from, br_to, last_excp_from, last_excp_to;
	u8 reserved_0x298[80];
	u32 pkru, tsc_aux;
	u64 tsc_scale, tsc_offset;
	u8 reserved_0x300[8];
	u64 rcx, rdx, rbx, reserved_0x320, rbp, rsi, rdi;
	u64 r8, r9, r10, r11, r12, r13, r14, r15;
	u8 reserved_0x380[16];
	u64 guest_exit_info_1, guest_exit_info_2, guest_exit_int_info, guest_nrip;
	u64 sev_features, vintr_ctrl, guest_exit_code, virtual_tom, tlb_id, pcpu_id;
	u64 event_inj, xcr0;
	u8 reserved_0x3f0[16];
	u64 x87_dp;
	u32 mxcsr;
	u16 x87_ftw, x87_fsw, x87_fcw, x87_fop, x87_ds, x87_cs;
	u64 x87_rip;
	u8 fpreg_x87[80];
	u8 fpreg_xmm[256];
	u8 fpreg_ymm[256];
} __packed;

static_assert(offsetof(struct test_vmsa, vmpl) == 0xca);
static_assert(offsetof(struct test_vmsa, rip) == 0x178);
static_assert(offsetof(struct test_vmsa, sev_features) == 0x3b0);
static_assert(offsetof(struct test_vmsa, xcr0) == 0x3e8);

static void guest_direct_entry(u64 *marker)
{
	*marker = DIRECT_MARKER;
	wrmsr(MSR_AMD64_SEV_ES_GHCB, GHCB_MSR_TERM_REQ);
	vmgexit();
}

static void guest_legacy_entry(u64 *marker)
{
	*marker = LEGACY_MARKER;
	wrmsr(MSR_AMD64_SEV_ES_GHCB, GHCB_MSR_TERM_REQ);
	vmgexit();
}

static void copy_segment(struct vmcb_seg *dst, const struct kvm_segment *src)
{
	dst->selector = src->selector;
	dst->base = src->base;
	dst->limit = src->limit;
	dst->attrib = src->type |
		(src->s << SVM_SELECTOR_S_SHIFT) |
		(src->dpl << SVM_SELECTOR_DPL_SHIFT) |
		((src->present && !src->unusable) << SVM_SELECTOR_P_SHIFT) |
		(src->avl << SVM_SELECTOR_AVL_SHIFT) |
		(src->l << SVM_SELECTOR_L_SHIFT) |
		(src->db << SVM_SELECTOR_DB_SHIFT) |
		(src->g << SVM_SELECTOR_G_SHIFT);
}

static void copy_dtable(struct vmcb_seg *dst, const struct kvm_dtable *src)
{
	dst->base = src->base;
	dst->limit = src->limit;
}

static void prepare_vmsa(struct kvm_vcpu *vcpu, struct test_vmsa *vmsa,
			 void *entry)
{
	struct kvm_sregs sregs;
	struct kvm_regs regs;

	memset(vmsa, 0, PAGE_SIZE);
	vcpu_sregs_get(vcpu, &sregs);
	vcpu_regs_get(vcpu, &regs);

	copy_segment(&vmsa->es, &sregs.es);
	copy_segment(&vmsa->cs, &sregs.cs);
	copy_segment(&vmsa->ss, &sregs.ss);
	copy_segment(&vmsa->ds, &sregs.ds);
	copy_segment(&vmsa->fs, &sregs.fs);
	copy_segment(&vmsa->gs, &sregs.gs);
	copy_dtable(&vmsa->gdtr, &sregs.gdt);
	copy_segment(&vmsa->ldtr, &sregs.ldt);
	copy_dtable(&vmsa->idtr, &sregs.idt);
	copy_segment(&vmsa->tr, &sregs.tr);

	vmsa->cpl = sregs.cs.dpl;
	/*
	 * KVM_GET_SREGS exposes the guest-visible EFER and therefore omits
	 * SVME, which KVM normally adds to the hardware VMSA itself.
	 */
	vmsa->efer = sregs.efer | EFER_SVME;
	vmsa->cr4 = sregs.cr4;
	vmsa->cr3 = sregs.cr3;
	vmsa->cr0 = sregs.cr0;
	vmsa->dr7 = 0x400;
	vmsa->dr6 = 0xffff0ff0;
	vmsa->rflags = regs.rflags;
	vmsa->rip = (u64)entry;
	vmsa->rsp = regs.rsp;
	vmsa->rax = regs.rax;
	vmsa->rcx = regs.rcx;
	vmsa->rdx = regs.rdx;
	vmsa->rbx = regs.rbx;
	vmsa->rbp = regs.rbp;
	vmsa->rsi = regs.rsi;
	vmsa->rdi = regs.rdi;
	vmsa->r8 = regs.r8;
	vmsa->r9 = regs.r9;
	vmsa->r10 = regs.r10;
	vmsa->r11 = regs.r11;
	vmsa->r12 = regs.r12;
	vmsa->r13 = regs.r13;
	vmsa->r14 = regs.r14;
	vmsa->r15 = regs.r15;
	vmsa->g_pat = 0x0007040600070406ULL;
	vmsa->sev_features = SNP_ACTIVE_SEV_FEATURE;
	vmsa->xcr0 = 1;
	vmsa->mxcsr = 0x1f80;
	vmsa->x87_fcw = 0x37f;
}

static void expect_launch_update_vmsa_error(struct kvm_vm *vm, gpa_t gpa,
					    void *vmsa, u64 size)
{
	struct kvm_sev_snp_launch_update update = {
		.gfn_start = gpa >> PAGE_SHIFT,
		.uaddr = (u64)vmsa,
		.len = size,
		.type = KVM_SEV_SNP_PAGE_TYPE_VMSA,
	};

	errno = 0;
	TEST_ASSERT_EQ(__vm_sev_ioctl(vm, KVM_SEV_SNP_LAUNCH_UPDATE, &update), -1);
	TEST_ASSERT_EQ(errno, EINVAL);
}

static void exclude_from_normal_launch(struct kvm_vm *vm, gpa_t gpa,
				       unsigned int npages)
{
	struct userspace_mem_region *region;

	region = memslot2region(vm, vm->memslots[MEM_REGION_TEST_DATA]);
	sparsebit_clear_num(region->protected_phy_pages, gpa >> PAGE_SHIFT, npages);
}

static void assert_vcpu_terminated(struct kvm_vcpu *vcpu)
{
	vcpu_run(vcpu);
	TEST_ASSERT_EQ(vcpu->run->exit_reason, KVM_EXIT_SYSTEM_EVENT);
	TEST_ASSERT_EQ(vcpu->run->system_event.type, KVM_SYSTEM_EVENT_SEV_TERM);
}

static struct kvm_vm *create_direct_vmsa_vm(unsigned int nr_vcpus,
					    void *guest_code,
					    struct kvm_vcpu **vcpus)
{
	struct vm_shape shape = {
		.mode = VM_MODE_DEFAULT,
		.type = KVM_X86_SNP_VM,
	};
	struct kvm_vm *vm;
	unsigned int i;

	vm = __vm_create(shape, nr_vcpus, 0);
	vm_enable_cap(vm, KVM_CAP_SNP_DIRECT_VMSA, 0);
	for (i = 0; i < nr_vcpus; i++)
		vcpus[i] = vm_vcpu_add(vm, i, guest_code);
	kvm_arch_vm_finalize_vcpus(vm);

	return vm;
}

static void test_direct_vmsa(void)
{
	struct test_vmsa *vmsas, *selected_vmsa;
	struct kvm_sev_snp_vcpu_state state = {};
	struct kvm_mp_state mp_state = {
		.mp_state = KVM_MP_STATE_UNINITIALIZED,
	};
	struct kvm_vcpu *vcpus[2];
	struct kvm_vcpu *vcpu, *ap;
	struct kvm_vm *vm;
	gva_t marker_gva;
	gpa_t vmsa_gpa;
	u64 *marker;

	vm = create_direct_vmsa_vm(ARRAY_SIZE(vcpus), guest_legacy_entry, vcpus);
	vcpu = vcpus[0];
	ap = vcpus[1];
	/* Restore normal AP state after vm_vcpu_add() makes it runnable. */
	vcpu_mp_state_set(ap, &mp_state);
	marker_gva = vm_alloc_shared(vm, PAGE_SIZE, KVM_UTIL_MIN_VADDR,
				     MEM_REGION_TEST_DATA);
	marker = addr_gva2hva(vm, marker_gva);
	vcpu_args_set(vcpu, 1, marker_gva);

	vmsa_gpa = vm_phy_pages_alloc(vm, 2, VMSA_MIN_GPA,
				      vm->memslots[MEM_REGION_TEST_DATA]);
	TEST_ASSERT(vmsa_gpa & (VMSA_PMD_SIZE - 1), "unsafe VMSA GPA");
	vmsas = aligned_alloc(PAGE_SIZE, 2 * PAGE_SIZE);
	TEST_ASSERT(vmsas, "Failed to allocate VMSA source pages");
	selected_vmsa = (void *)vmsas + PAGE_SIZE;
	prepare_vmsa(vcpu, &vmsas[0], guest_legacy_entry);
	prepare_vmsa(vcpu, selected_vmsa, guest_direct_entry);

	snp_vm_launch_start(vm, snp_default_policy());
	vm_mem_set_private(vm, vmsa_gpa, 2 * PAGE_SIZE);
	expect_launch_update_vmsa_error(vm, vmsa_gpa, vmsas, 2 * PAGE_SIZE);

	vmsas[0].vmpl = 1;
	expect_launch_update_vmsa_error(vm, vmsa_gpa, vmsas, PAGE_SIZE);
	vmsas[0].vmpl = 0;
	vmsas[0].sev_features = 0;
	expect_launch_update_vmsa_error(vm, vmsa_gpa, vmsas, PAGE_SIZE);
	vmsas[0].sev_features = SNP_ACTIVE_SEV_FEATURE;

	snp_launch_update_vmsa(vm, vmsa_gpa, vmsas);
	snp_launch_update_vmsa(vm, vmsa_gpa + PAGE_SIZE, selected_vmsa);
	exclude_from_normal_launch(vm, vmsa_gpa, 2);
	snp_vm_launch_update(vm);

	/* Rebinding is allowed; the second, selected VMSA must win. */
	snp_set_vcpu_state(vcpu, vmsa_gpa);
	snp_set_vcpu_state(vcpu, vmsa_gpa + PAGE_SIZE);
	snp_get_vcpu_state(vcpu, &state);
	TEST_ASSERT_EQ(state.valid_fields,
		       KVM_SEV_SNP_VCPU_STATE_VMSA_VALID |
		       KVM_SEV_SNP_VCPU_STATE_GHCB_VALID);
	TEST_ASSERT_EQ(state.vmsa_gpa, vmsa_gpa + PAGE_SIZE);
	TEST_ASSERT_EQ(state.ghcb_gpa, 0);
	snp_vm_launch_finish(vm);

	vcpu_mp_state_get(ap, &mp_state);
	TEST_ASSERT_EQ(mp_state.mp_state, KVM_MP_STATE_UNINITIALIZED);
	*marker = 0;
	assert_vcpu_terminated(vcpu);
	TEST_ASSERT_EQ(*marker, DIRECT_MARKER);

	state = (struct kvm_sev_snp_vcpu_state) {
		.vmsa_gpa = vmsa_gpa,
		.valid_fields = KVM_SEV_SNP_VCPU_STATE_VMSA_VALID,
	};
	errno = 0;
	TEST_ASSERT_EQ(__vcpu_sev_ioctl(vcpu, KVM_SEV_SNP_SET_VCPU_STATE,
					&state), -1);
	TEST_ASSERT_EQ(errno, EINVAL);

	free(vmsas);
	kvm_vm_free(vm);
}

static void expect_set_vcpu_state_error(struct kvm_vcpu *vcpu,
					struct kvm_sev_snp_vcpu_state *state,
					int expected_errno)
{
	errno = 0;
	TEST_ASSERT_EQ(__vcpu_sev_ioctl(vcpu, KVM_SEV_SNP_SET_VCPU_STATE, state), -1);
	TEST_ASSERT_EQ(errno, expected_errno);
}

static void test_invalid_requests(void)
{
	struct kvm_sev_snp_vcpu_state state = {
		.valid_fields = KVM_SEV_SNP_VCPU_STATE_VMSA_VALID,
	};
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	gva_t shared_gva;
	gpa_t private_gpa;

	vm = vm_create_with_one_vcpu(&vcpu, guest_legacy_entry);
	expect_set_vcpu_state_error(vcpu, &state, ENOTTY);
	kvm_vm_free(vm);

	vm = vm_sev_create_with_one_vcpu(KVM_X86_SNP_VM, guest_legacy_entry, &vcpu);
	expect_set_vcpu_state_error(vcpu, &state, EINVAL);
	TEST_ASSERT_EQ(__vm_enable_cap(vm, KVM_CAP_SNP_DIRECT_VMSA, 0), -1);
	TEST_ASSERT_EQ(errno, EINVAL);
	snp_vm_launch_start(vm, snp_default_policy());
	expect_set_vcpu_state_error(vcpu, &state, EINVAL);
	kvm_vm_free(vm);

	vm = create_direct_vmsa_vm(1, guest_legacy_entry, &vcpu);
	expect_set_vcpu_state_error(vcpu, &state, EINVAL);
	snp_vm_launch_start(vm, snp_default_policy());

	state.pad[4] = 1;
	expect_set_vcpu_state_error(vcpu, &state, EINVAL);
	state.pad[4] = 0;
	state.valid_fields |= BIT_ULL(2);
	expect_set_vcpu_state_error(vcpu, &state, EINVAL);
	state.valid_fields &= ~BIT_ULL(2);
	state.vmsa_gpa = PAGE_SIZE + 1;
	expect_set_vcpu_state_error(vcpu, &state, EINVAL);
	state.vmsa_gpa = VMSA_PMD_SIZE;
	expect_set_vcpu_state_error(vcpu, &state, EINVAL);
	state.vmsa_gpa = BIT_ULL(40) + PAGE_SIZE;
	expect_set_vcpu_state_error(vcpu, &state, EINVAL);

	shared_gva = vm_alloc_shared(vm, 2 * PAGE_SIZE, KVM_UTIL_MIN_VADDR,
				     MEM_REGION_TEST_DATA);
	state.vmsa_gpa = addr_gva2gpa(vm, shared_gva);
	if (!(state.vmsa_gpa & (VMSA_PMD_SIZE - 1)))
		state.vmsa_gpa += PAGE_SIZE;
	state.ghcb_gpa = BIT_ULL(40);
	state.valid_fields |= KVM_SEV_SNP_VCPU_STATE_GHCB_VALID;
	vcpu_sev_ioctl(vcpu, KVM_SEV_SNP_SET_VCPU_STATE, &state);

	private_gpa = vm_phy_page_alloc(vm, VMSA_MIN_GPA,
					vm->memslots[MEM_REGION_TEST_DATA]);
	vm_mem_set_private(vm, private_gpa, PAGE_SIZE);
	state.vmsa_gpa = private_gpa;
	vcpu_sev_ioctl(vcpu, KVM_SEV_SNP_SET_VCPU_STATE, &state);

	memset(&state, 0, sizeof(state));
	snp_get_vcpu_state(vcpu, &state);
	TEST_ASSERT_EQ(state.valid_fields,
		       KVM_SEV_SNP_VCPU_STATE_VMSA_VALID |
		       KVM_SEV_SNP_VCPU_STATE_GHCB_VALID);
	TEST_ASSERT_EQ(state.vmsa_gpa, private_gpa);
	TEST_ASSERT_EQ(state.ghcb_gpa, BIT_ULL(40));

	kvm_vm_free(vm);
}

static void test_direct_vmsa_capability(void)
{
	struct kvm_enable_cap cap = {
		.cap = KVM_CAP_SNP_DIRECT_VMSA,
	};
	struct vm_shape shape = {
		.mode = VM_MODE_DEFAULT,
		.type = KVM_X86_SNP_VM,
	};
	struct kvm_vm *vm;

	vm = vm_create_barebones();
	TEST_ASSERT_EQ(__vm_enable_cap(vm, KVM_CAP_SNP_DIRECT_VMSA, 0), -1);
	TEST_ASSERT_EQ(errno, EINVAL);
	kvm_vm_free(vm);

	vm = __vm_create(shape, 1, 0);
	TEST_ASSERT_EQ(__vm_enable_cap(vm, KVM_CAP_SNP_DIRECT_VMSA, 1), -1);
	TEST_ASSERT_EQ(errno, EINVAL);
	cap.args[3] = 1;
	TEST_ASSERT_EQ(__vm_ioctl(vm, KVM_ENABLE_CAP, &cap), -1);
	TEST_ASSERT_EQ(errno, EINVAL);
	cap.args[3] = 0;
	cap.flags = 1;
	TEST_ASSERT_EQ(__vm_ioctl(vm, KVM_ENABLE_CAP, &cap), -1);
	TEST_ASSERT_EQ(errno, EINVAL);
	vm_enable_cap(vm, KVM_CAP_SNP_DIRECT_VMSA, 0);
	vm_vcpu_add(vm, 0, guest_legacy_entry);
	TEST_ASSERT_EQ(__vm_enable_cap(vm, KVM_CAP_SNP_DIRECT_VMSA, 0), -1);
	TEST_ASSERT_EQ(errno, EINVAL);
	kvm_vm_free(vm);
}

static void test_legacy_launch(void)
{
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	gva_t marker_gva;
	u64 *marker;

	vm = vm_sev_create_with_one_vcpu(KVM_X86_SNP_VM, guest_legacy_entry, &vcpu);
	marker_gva = vm_alloc_shared(vm, PAGE_SIZE, KVM_UTIL_MIN_VADDR,
				     MEM_REGION_TEST_DATA);
	marker = addr_gva2hva(vm, marker_gva);
	vcpu_args_set(vcpu, 1, marker_gva);
	vm_sev_launch(vm, snp_default_policy(), NULL);
	*marker = 0;
	assert_vcpu_terminated(vcpu);
	TEST_ASSERT_EQ(*marker, LEGACY_MARKER);
	kvm_vm_free(vm);
}

int main(int argc, char *argv[])
{
	TEST_REQUIRE(kvm_check_cap(KVM_CAP_SNP_VCPU_STATE));
	TEST_REQUIRE(kvm_check_cap(KVM_CAP_SNP_DIRECT_VMSA));
	TEST_ASSERT(sizeof(struct test_vmsa) <= PAGE_SIZE, "VMSA structure is too large");

	test_direct_vmsa_capability();
	test_invalid_requests();
	test_direct_vmsa();
	test_legacy_launch();
	return 0;
}
