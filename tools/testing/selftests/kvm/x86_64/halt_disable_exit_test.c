// SPDX-License-Identifier: GPL-2.0-only
/*
 * KVM disable halt exit test
 *
 *  Copyright (C) 2024 Advanced Micro Devices, Inc.
 */
#include <pthread.h>
#include <signal.h>
#include "kvm_util.h"
#include "svm_util.h"
#include "processor.h"
#include "test_util.h"

pthread_t task_thread, vcpu_thread;
#define SIG_IPI SIGUSR1

static void guest_code(uint8_t is_hlt_exec)
{
	while (!READ_ONCE(is_hlt_exec))
		;

	safe_halt();
	GUEST_DONE();
}

static void *task_worker(void *arg)
{
	uint8_t *is_hlt_exec = (uint8_t *)arg;

	usleep(1000);
	WRITE_ONCE(*is_hlt_exec, 1);
	pthread_kill(vcpu_thread, SIG_IPI);
	return 0;
}

static void *vcpu_worker(void *arg)
{
	int ret;
	int sig = -1;
	uint8_t *is_hlt_exec = (uint8_t *)arg;
	struct kvm_vm *vm;
	struct kvm_run *run;
	struct kvm_vcpu *vcpu;
	struct kvm_signal_mask *sigmask = alloca(offsetof(struct kvm_signal_mask, sigset)
						 + sizeof(sigset_t));
	sigset_t *sigset = (sigset_t *) &sigmask->sigset;

	/* Create a VM without in kernel APIC support */
	vm = __vm_create(VM_SHAPE_DEFAULT, 1, 0, false);
	vm_enable_cap(vm, KVM_CAP_X86_DISABLE_EXITS, KVM_X86_DISABLE_EXITS_HLT);
	vcpu = vm_vcpu_add(vm, 0, guest_code);
	vcpu_args_set(vcpu, 1, *is_hlt_exec);

	/*
	 * SIG_IPI is unblocked atomically while in KVM_RUN.  It causes the
	 * ioctl to return with -EINTR, but it is still pending and we need
	 * to accept it with the sigwait.
	 */
	sigmask->len = 8;
	pthread_sigmask(0, NULL, sigset);
	sigdelset(sigset, SIG_IPI);
	vcpu_ioctl(vcpu, KVM_SET_SIGNAL_MASK, sigmask);
	sigemptyset(sigset);
	sigaddset(sigset, SIG_IPI);
	run = vcpu->run;

again:
	ret = __vcpu_run(vcpu);
	TEST_ASSERT_EQ(errno, EINTR);

	if (ret == -1 && errno == EINTR) {
		sigwait(sigset, &sig);
		assert(sig == SIG_IPI);
		TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_INTR);
		goto again;
	}

	if (run->exit_reason == KVM_EXIT_HLT)
		TEST_FAIL("Expected KVM_EXIT_INTR, got KVM_EXIT_HLT");

	TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_IO);
	kvm_vm_free(vm);
	return 0;
}

int main(int argc, char *argv[])
{
	int ret;
	void *retval;
	uint8_t is_halt_exec;
	sigset_t sigset;

	TEST_REQUIRE(kvm_has_cap(KVM_CAP_X86_DISABLE_EXITS));

	/* Ensure that vCPU threads start with SIG_IPI blocked.  */
	sigemptyset(&sigset);
	sigaddset(&sigset, SIG_IPI);
	pthread_sigmask(SIG_BLOCK, &sigset, NULL);

	ret = pthread_create(&vcpu_thread, NULL, vcpu_worker, &is_halt_exec);
	TEST_ASSERT(ret == 0, "pthread_create vcpu thread failed errno=%d", errno);

	ret = pthread_create(&task_thread, NULL, task_worker, &is_halt_exec);
	TEST_ASSERT(ret == 0, "pthread_create task thread failed errno=%d", errno);

	pthread_join(vcpu_thread, &retval);
	TEST_ASSERT(ret == 0, "pthread_join on vcpu thread failed with errno=%d", ret);

	pthread_join(task_thread, &retval);
	TEST_ASSERT(ret == 0, "pthread_join on task thread failed with errno=%d", ret);

	return 0;
}
