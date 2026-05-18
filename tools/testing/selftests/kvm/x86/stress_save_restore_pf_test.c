// SPDX-License-Identifier: GPL-2.0-only
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>

#include "test_util.h"
#include "kvm_util.h"
#include "processor.h"
#include "svm_util.h"
#include "vmx.h"

#define NR_ITERATIONS		500

#define CURSOR_UP		"\033[A"
#define PRINT_ITER(s, x)					\
({								\
	printf("%s\r%s%d\n", (x ? CURSOR_UP : ""), s, x);	\
	fflush(stdout);						\
})

#define TEST_MEM_BASE		0xc0000000ULL
#define NR_TEST_ADDRS		512
#define PATTERN			0xabcdefabcdefabcdULL

#define L2_GUEST_STACK_SIZE	64

#define PTRS_PER_PTE		512
#define PXD_INDEX(vaddr, level)	(((vaddr) >> PG_LEVEL_SHIFT(level)) & (PTRS_PER_PTE - 1))

static u64 pte_present_mask;
static u64 pte_huge_mask;

static u64 expected_vaddr;
static u64 guest_accesses;

static u64 *guest_get_pte(u64 vaddr)
{
	u64 *pgd, *p4d, *pud, *pmd, *pte;
	u64 pgde, p4de, pude, pmde;
	bool la57;

	la57 = !!(get_cr4() & X86_CR4_LA57);
	pgd = (u64 *)(get_cr3() & PHYSICAL_PAGE_MASK);

	if (la57) {
		pgde = pgd[PXD_INDEX(vaddr, PG_LEVEL_256T)];
		GUEST_ASSERT(pgde & pte_present_mask);
		p4d = (u64 *)PTE_GET_PA(pgde);
		p4de = p4d[PXD_INDEX(vaddr, PG_LEVEL_512G)];
	} else {
		pgde = pgd[PXD_INDEX(vaddr, PG_LEVEL_512G)];
		p4de = pgde;
	}

	GUEST_ASSERT(p4de & pte_present_mask);
	pud = (u64 *)PTE_GET_PA(p4de);

	pude = pud[PXD_INDEX(vaddr, PG_LEVEL_1G)];
	GUEST_ASSERT(pude & pte_present_mask);
	GUEST_ASSERT(!(pude & pte_huge_mask));
	pmd = (u64 *)PTE_GET_PA(pude);

	pmde = pmd[PXD_INDEX(vaddr, PG_LEVEL_2M)];
	GUEST_ASSERT(pmde & pte_present_mask);
	GUEST_ASSERT(!(pmde & pte_huge_mask));
	pte = (u64 *)PTE_GET_PA(pmde);

	return &pte[PXD_INDEX(vaddr, PG_LEVEL_4K)];
}

static void guest_pf_handler(struct ex_regs *regs)
{
	u64 fault_addr;
	u64 *ptep;

	fault_addr = get_cr2();
	GUEST_ASSERT_EQ(fault_addr, READ_ONCE(expected_vaddr));

	ptep = guest_get_pte(fault_addr);
	GUEST_ASSERT(ptep);
	GUEST_ASSERT(!(*ptep & pte_present_mask));

	*ptep |= pte_present_mask;
	invlpg(fault_addr);
}

static void guest_access_memory(void *arg)
{
	u64 vaddr, val;

	for (;; guest_accesses++) {
		vaddr = TEST_MEM_BASE + (guest_accesses % NR_TEST_ADDRS) * PAGE_SIZE;
		WRITE_ONCE(expected_vaddr, vaddr);

		/* Read to trigger #PF */
		val = READ_ONCE(*(u64 *)vaddr);
		GUEST_ASSERT_EQ(val, PATTERN);

		/* Clear the present bit again so it faults next time */
		*guest_get_pte(vaddr) &= ~pte_present_mask;
		invlpg(vaddr);
	}
}

static void l1_svm_code(struct svm_test_data *svm)
{
	unsigned long l2_guest_stack[L2_GUEST_STACK_SIZE];

	generic_svm_setup(svm, guest_access_memory, &l2_guest_stack[L2_GUEST_STACK_SIZE]);

	svm->vmcb->control.intercept_exceptions |= BIT(UD_VECTOR);

	while (1) {
		run_guest(svm->vmcb, svm->vmcb_gpa);
		GUEST_ASSERT_EQ(svm->vmcb->control.exit_code, (SVM_EXIT_EXCP_BASE + UD_VECTOR));
	}
}

static void l1_vmx_code(struct vmx_pages *vmx)
{
	unsigned long l2_guest_stack[L2_GUEST_STACK_SIZE];

	GUEST_ASSERT(prepare_for_vmx_operation(vmx));
	GUEST_ASSERT(load_vmcs(vmx));
	prepare_vmcs(vmx, guest_access_memory, &l2_guest_stack[L2_GUEST_STACK_SIZE]);

	/* Intercept UD, ignore any #PF */
	GUEST_ASSERT(!vmwrite(EXCEPTION_BITMAP, BIT(UD_VECTOR) | BIT(PF_VECTOR)));
	GUEST_ASSERT(!vmwrite(PAGE_FAULT_ERROR_CODE_MASK, 0));
	GUEST_ASSERT(!vmwrite(PAGE_FAULT_ERROR_CODE_MATCH, -1));

	GUEST_ASSERT(!vmlaunch());
	while (1) {
		GUEST_ASSERT_EQ(vmreadz(VM_EXIT_REASON), EXIT_REASON_EXCEPTION_NMI);
		GUEST_ASSERT_EQ(vmreadz(VM_EXIT_INTR_INFO) & 0xff, UD_VECTOR);
		GUEST_ASSERT(!vmresume());
	}
}

static void l1_guest_code(void *test_data)
{
	if (this_cpu_has(X86_FEATURE_SVM))
		l1_svm_code(test_data);
	else
		l1_vmx_code(test_data);
}

static void *sigusr_thread_fn(void *arg)
{
	pthread_t vcpu_thread = (pthread_t)arg;

	for (;;) {
		pthread_testcancel();
		pthread_kill(vcpu_thread, SIGUSR1);
		usleep(100);
	}
	return NULL;
}

static void dummy_signal_handler(int signo) {}
static struct sigaction sa;

static void vcpu_sigusr_listen(void)
{
	sa.sa_handler = dummy_signal_handler;
	sigaction(SIGUSR1, &sa, NULL);
}

static void vcpu_sigusr_ignore(void)
{
	sa.sa_handler = SIG_IGN;
	sigaction(SIGUSR1, &sa, NULL);
}

static bool vcpu_state_is_guest_mode(struct kvm_x86_state *state)
{
	return !!(state->nested.flags & KVM_STATE_NESTED_GUEST_MODE);
}

static void vcpu_state_inject_ud(struct kvm_x86_state *state)
{
	if (state->events.exception.pending || state->events.exception.injected)
		return;

	state->events.flags |= KVM_VCPUEVENT_VALID_PAYLOAD;
	state->events.exception.pending = true;
	state->events.exception.injected = false;
	state->events.exception.nr = UD_VECTOR;
	state->events.exception.has_error_code = false;
}

static bool parse_args_nested(int argc, char *argv[])
{
	bool nested = false;
	int opt;

	while ((opt = getopt(argc, argv, "n")) != -1) {
		switch (opt) {
		case 'n':
			nested = true;
			break;
		default:
			printf("Usage: %s [-n]\n", argv[0]);
			exit(1);
		}
	}

	return nested;
}

int main(int argc, char *argv[])
{
	struct kvm_x86_state *state;
	pthread_t sigusr_thread;
	struct kvm_vcpu *vcpu;
	int r, i, count = 0;
	struct kvm_vm *vm;
	struct ucall uc;
	bool nested;
	gva_t gva;
	gpa_t gpa;

	TEST_REQUIRE(kvm_has_cap(KVM_CAP_EXCEPTION_PAYLOAD));

	nested = parse_args_nested(argc, argv);

	vm = vm_create_with_one_vcpu(&vcpu, nested ? l1_guest_code : guest_access_memory);
	vm_install_exception_handler(vm, PF_VECTOR, guest_pf_handler);
	vm_enable_cap(vm, KVM_CAP_EXCEPTION_PAYLOAD, -2ul);

	if (nested) {
		TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_SVM) || kvm_cpu_has(X86_FEATURE_VMX));
		if (kvm_cpu_has(X86_FEATURE_SVM))
			vcpu_alloc_svm(vm, &gva);
		else
			vcpu_alloc_vmx(vm, &gva);
		vcpu_args_set(vcpu, 1, gva);
	}

	pte_present_mask = PTE_PRESENT_MASK(&vm->mmu);
	pte_huge_mask = PTE_HUGE_MASK(&vm->mmu);
	sync_global_to_guest(vm, pte_present_mask);
	sync_global_to_guest(vm, pte_huge_mask);

	/* Allocate a page and write the pattern to it */
	gva = vm_alloc_page(vm);
	*(u64 *)addr_gva2hva(vm, gva) = PATTERN;
	gpa = addr_gva2gpa(vm, gva);

	/*
	 * Map all virtual addresses to the pattern page and clear the present
	 * bit such that guest accesses will cause a #PF.
	 */
	for (i = 0; i < NR_TEST_ADDRS; i++) {
		gva = TEST_MEM_BASE + i * getpagesize();
		virt_pg_map(vm, gva, gpa);
		*vm_get_pte(vm, gva) &= ~pte_present_mask;
	}

	/* Map the page tables so that the guest #PF handler can walk them */
	virt_map_page_tables(vm);

	/* Initialize the thread sending SIGUSR and install the handler */
	pthread_create(&sigusr_thread, NULL, sigusr_thread_fn,
		       (void *)pthread_self());

	while (count++ < NR_ITERATIONS) {
		/*
		 * Only handle SIGUSR while the vCPU is running, otherwise
		 * ignore it to avoid interrupting other ioctls/syscalls.
		 */
		vcpu_sigusr_listen();
		r = __vcpu_run(vcpu);
		if (r == -1)
			TEST_ASSERT_EQ(errno, EINTR);
		vcpu_sigusr_ignore();

		/* The guest only exists due to a signal or failed assertion */
		if (!r) {
			TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_IO);
			TEST_ASSERT_EQ(get_ucall(vcpu, &uc), UCALL_ABORT);
			REPORT_GUEST_ASSERT(uc);
			break;
		}

		state = vcpu_save_state(vcpu);

		/*
		 * If the vCPU is in guest mode, inject a #UD to trigger an
		 * L2->L1 VM-Exit every other iteration.
		 */
		if (vcpu_state_is_guest_mode(state) && count % 2 == 0) {
			TEST_ASSERT(nested, "Unexpected guest mode");
			vcpu_state_inject_ud(state);
		}

		kvm_vm_release(vm);
		vcpu = vm_recreate_with_one_vcpu(vm);
		vm_enable_cap(vm, KVM_CAP_EXCEPTION_PAYLOAD, -2ul);
		vcpu_load_state(vcpu, state);
		kvm_x86_state_cleanup(state);

		PRINT_ITER("Save+restore iterations: ", count);
	}

	sync_global_from_guest(vm, guest_accesses);
	pr_info("Guest page accesses%s: %lu\n", nested ? " (from L2)" : "", guest_accesses);

	pthread_cancel(sigusr_thread);
	pthread_join(sigusr_thread, NULL);
	kvm_vm_free(vm);
	return 0;
}
