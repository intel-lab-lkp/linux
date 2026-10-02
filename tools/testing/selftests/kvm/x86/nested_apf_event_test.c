// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026, Ryzome
 *
 * Reproduce a lost nested asynchronous page-fault notification.
 *
 * L2 faults on a page held missing by userfaultfd.  KVM writes
 * PAGE_NOT_PRESENT to L1's APF record and queues a synthetic #PF VM-exit.
 * The vulnerable path omits KVM_REQ_EVENT, re-enters L2, and loses the
 * notification before L1 can consume it.  A fixed L0 exits to L1 immediately.
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <asm/kvm_para.h>

#include "kvm_util.h"
#include "processor.h"
#include "test_util.h"
#include "vmx.h"

#define APF_VECTOR 0xF1U
#define RUN_TIMEOUT_SECONDS 3U
#define VMX_INTR_INFO_VECTOR_MASK 0xFFU
#define VMX_INTR_INFO_TYPE_MASK 0x700U
#define VMX_INTR_INFO_HARD_EXCEPTION (3U << 8)
#define VMX_INTR_INFO_DELIVER_CODE BIT(11)
#define VMX_INTR_INFO_VALID BIT(31)

enum test_stage {
	STAGE_APF_ARMED = 1,
	STAGE_APF_DELIVERED,
};

static struct kvm_vcpu_pv_apf_data apf_data __aligned(64);
static gva_t l2_target_gva;

static void l2_guest_code(void)
{
	u64 value;

	value = READ_ONCE(*(u64 *)READ_ONCE(l2_target_gva));
	GUEST_FAIL("L2 resumed after the missing page was resolved: %#lx", value);
}

static void l1_guest_code(struct vmx_pages *vmx, gpa_t apf_data_gpa)
{
	u64 apf_msr;
	u64 intr_info;
	u64 reason;
	u64 token;

	GUEST_ASSERT(this_cpu_has(X86_FEATURE_KVM_ASYNC_PF));
	GUEST_ASSERT(this_cpu_has(X86_FEATURE_KVM_ASYNC_PF_VMEXIT));
	GUEST_ASSERT(this_cpu_has(X86_FEATURE_KVM_ASYNC_PF_INT));
	prepare_for_vmx_operation(vmx);
	load_vmcs(vmx);
	prepare_vmcs(vmx, l2_guest_code);
	vmwrite(GUEST_RFLAGS,
		X86_EFLAGS_FIXED | X86_EFLAGS_IF);

	wrmsr(MSR_KVM_ASYNC_PF_INT, APF_VECTOR);
	apf_msr = apf_data_gpa | KVM_ASYNC_PF_ENABLED |
		  KVM_ASYNC_PF_SEND_ALWAYS |
		  KVM_ASYNC_PF_DELIVERY_AS_PF_VMEXIT |
		  KVM_ASYNC_PF_DELIVERY_AS_INT;
	wrmsr(MSR_KVM_ASYNC_PF_EN, apf_msr);

	/*
	 * Let userspace discard and userfaultfd-register the target only after
	 * all L1 setup exits have completed.  The following KVM_RUN therefore
	 * has no pending event request to accidentally deliver the nested APF.
	 */
	GUEST_SYNC(STAGE_APF_ARMED);

	vmlaunch();
	GUEST_ASSERT_EQ(vmread(VM_EXIT_REASON), EXIT_REASON_EXCEPTION_NMI);

	intr_info = vmread(VM_EXIT_INTR_INFO);
	GUEST_ASSERT(intr_info & VMX_INTR_INFO_VALID);
	GUEST_ASSERT_EQ(intr_info & VMX_INTR_INFO_VECTOR_MASK, PF_VECTOR);
	GUEST_ASSERT_EQ(intr_info & VMX_INTR_INFO_TYPE_MASK,
			VMX_INTR_INFO_HARD_EXCEPTION);
	GUEST_ASSERT(intr_info & VMX_INTR_INFO_DELIVER_CODE);
	GUEST_ASSERT_EQ(vmread(VM_EXIT_INTR_ERROR_CODE), 0);

	token = vmread(EXIT_QUALIFICATION);
	reason = READ_ONCE(apf_data.flags);
	GUEST_ASSERT(token);
	GUEST_ASSERT_EQ(reason, KVM_PV_REASON_PAGE_NOT_PRESENT);

	/* Release the APF record slot before userspace resolves the page. */
	WRITE_ONCE(apf_data.flags, 0);
	GUEST_SYNC_ARGS(STAGE_APF_DELIVERED, token, reason, 0, 0);
}

#ifdef __NR_userfaultfd
static int register_missing_page(void *hva)
{
	struct uffdio_register uffd_register = {
		.range = {
			.start = (uintptr_t)hva,
			.len = PAGE_SIZE,
		},
		.mode = UFFDIO_REGISTER_MODE_MISSING,
	};
	struct uffdio_api uffd_api = {
		.api = UFFD_API,
	};
	int uffd;

	uffd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
	if (uffd < 0 && (errno == EPERM || errno == EACCES))
		ksft_exit_skip("userfaultfd is unavailable: %s\n", strerror(errno));
	TEST_ASSERT(uffd >= 0, "userfaultfd failed: %s", strerror(errno));

	TEST_ASSERT(ioctl(uffd, UFFDIO_API, &uffd_api) == 0,
		    "UFFDIO_API failed: %s", strerror(errno));
	TEST_ASSERT(ioctl(uffd, UFFDIO_REGISTER, &uffd_register) == 0,
		    "UFFDIO_REGISTER failed: %s", strerror(errno));
	TEST_ASSERT(uffd_register.ioctls & (1ULL << _UFFDIO_COPY),
		    "userfaultfd does not support UFFDIO_COPY");

	return uffd;
}

static bool read_missing_page_event(int uffd, void *target_hva)
{
	struct pollfd pollfd = {
		.fd = uffd,
		.events = POLLIN,
	};
	struct uffd_msg msg;
	ssize_t bytes;
	int ret;

	ret = poll(&pollfd, 1, 3000);
	if (ret != 1 || !(pollfd.revents & POLLIN))
		return false;

	bytes = read(uffd, &msg, sizeof(msg));
	if (bytes != sizeof(msg) || msg.event != UFFD_EVENT_PAGEFAULT)
		return false;

	return (msg.arg.pagefault.address & ~(PAGE_SIZE - 1ULL)) ==
	       (uintptr_t)target_hva;
}

static void resolve_missing_page(int uffd, void *target_hva)
{
	struct uffdio_copy uffd_copy = {
		.dst = (uintptr_t)target_hva,
		.len = PAGE_SIZE,
	};
	void *page = NULL;
	int ret;

	ret = posix_memalign(&page, PAGE_SIZE, PAGE_SIZE);
	TEST_ASSERT(!ret, "posix_memalign failed: %s", strerror(ret));
	memset(page, 0, PAGE_SIZE);
	uffd_copy.src = (uintptr_t)page;

	TEST_ASSERT(ioctl(uffd, UFFDIO_COPY, &uffd_copy) == 0,
		    "UFFDIO_COPY failed: %s", strerror(errno));
	free(page);
}

static void alarm_handler(int signal)
{
	(void)signal;
}

static void assert_sync_stage(struct kvm_vcpu *vcpu, u64 expected_stage)
{
	struct ucall uc;

	TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_IO);
	switch (get_ucall(vcpu, &uc)) {
	case UCALL_SYNC:
		TEST_ASSERT_EQ(uc.args[1], expected_stage);
		break;
	case UCALL_ABORT:
		REPORT_GUEST_ASSERT(uc);
		break;
	default:
		TEST_FAIL("Unexpected ucall command %lu", uc.cmd);
	}
}

static void run_test(void)
{
	struct kvm_vcpu_pv_apf_data *apf_hva;
	struct sigaction action = {
		.sa_handler = alarm_handler,
	};
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	struct ucall uc = {};
	sigset_t blocked_signals;
	sigset_t old_signals;
	gva_t nested_gva;
	gva_t target_gva;
	gpa_t apf_gpa;
	void *target_hva;
	u32 stale_reason;
	bool uffd_event;
	bool run_timed_out;
	int run_errno = 0;
	int run_ret;
	int uffd;
	u64 ucall_cmd = UCALL_NONE;

	vm = vm_create_with_one_vcpu(&vcpu, l1_guest_code);
	vm_enable_tdp(vm);
	vcpu_alloc_vmx(vm, &nested_gva);
	target_gva = vm_alloc_page(vm);
	tdp_identity_map_default_memslots(vm);

	l2_target_gva = target_gva;
	sync_global_to_guest(vm, l2_target_gva);
	apf_gpa = addr_gva2gpa(vm, (gva_t)&apf_data);
	TEST_ASSERT(!(apf_gpa & 63U), "APF data GPA %#lx is misaligned",
		    apf_gpa);
	apf_hva = addr_gva2hva(vm, (gva_t)&apf_data);
	target_hva = addr_gva2hva(vm, target_gva);
	vcpu_args_set(vcpu, 2, nested_gva, apf_gpa);

	vcpu_run(vcpu);
	assert_sync_stage(vcpu, STAGE_APF_ARMED);

	TEST_ASSERT(madvise(target_hva, PAGE_SIZE, MADV_DONTNEED) == 0,
		    "madvise failed: %s", strerror(errno));
	uffd = register_missing_page(target_hva);

	TEST_ASSERT(sigaction(SIGALRM, &action, NULL) == 0,
		    "sigaction failed: %s", strerror(errno));
	sigfillset(&blocked_signals);
	sigdelset(&blocked_signals, SIGALRM);
	TEST_ASSERT(sigprocmask(SIG_SETMASK, &blocked_signals, &old_signals) == 0,
		    "sigprocmask failed: %s", strerror(errno));

	alarm(RUN_TIMEOUT_SECONDS);
	run_ret = __vcpu_run(vcpu);
	run_errno = errno;
	alarm(0);
	run_timed_out = run_ret < 0 && run_errno == EINTR;
	TEST_ASSERT(sigprocmask(SIG_SETMASK, &old_signals, NULL) == 0,
		    "sigprocmask restore failed: %s", strerror(errno));

	stale_reason = READ_ONCE(apf_hva->flags);
	uffd_event = read_missing_page_event(uffd, target_hva);
	if (uffd_event)
		resolve_missing_page(uffd, target_hva);
	close(uffd);

	if (!run_ret)
		ucall_cmd = get_ucall(vcpu, &uc);
	if (ucall_cmd == UCALL_ABORT)
		REPORT_GUEST_ASSERT(uc);
	kvm_vm_free(vm);

	TEST_ASSERT(uffd_event,
		    "L2 access did not produce the held userfaultfd fault");
	TEST_ASSERT(!run_timed_out ||
		    stale_reason == KVM_PV_REASON_PAGE_NOT_PRESENT,
		    "KVM_RUN timed out without a stale APF reason: reason=%u",
		    stale_reason);
	TEST_ASSERT(!run_timed_out,
		    "Lost nested APF event: KVM re-entered L2 and blocked; APF reason=%u",
		    stale_reason);
	TEST_ASSERT(!run_ret, "KVM_RUN failed: %s", strerror(run_errno));
	TEST_ASSERT_EQ(ucall_cmd, UCALL_SYNC);
	TEST_ASSERT_EQ(uc.args[1], STAGE_APF_DELIVERED);
	TEST_ASSERT_EQ(uc.args[3], KVM_PV_REASON_PAGE_NOT_PRESENT);
	TEST_ASSERT(uc.args[2], "KVM returned a zero APF token");
}
#endif

int main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;

#ifndef __NR_userfaultfd
	ksft_exit_skip("__NR_userfaultfd is unavailable\n");
#else
	TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_VMX));
	TEST_REQUIRE(kvm_cpu_has_ept());
	TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_KVM_ASYNC_PF));
	TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_KVM_ASYNC_PF_VMEXIT));
	TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_KVM_ASYNC_PF_INT));

	run_test();
#endif
	return 0;
}
