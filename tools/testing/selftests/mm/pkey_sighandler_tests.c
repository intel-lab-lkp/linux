#define _GNU_SOURCE
#define __SANE_USERSPACE_TYPES__
#include <errno.h>
#include <sys/syscall.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <signal.h>
#include <assert.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <limits.h>

#include "pkey-helpers.h"

/*
 * Compile with:
 * gcc -mxsave      -o pkey_sighandler_tests -O2 -g -std=gnu99 -pthread -Wall pkey_sighandler_tests.c -lrt -ldl -lm
 */

#define STACK_SIZE PTHREAD_STACK_MIN

void expected_pkey_fault(int pkey) {}

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
siginfo_t siginfo = {0};

static inline __attribute__((always_inline)) long
syscall_raw(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{
	unsigned long ret;
	register long r10 asm("r10") = a4;
	register long r8 asm("r8") = a5;
	register long r9 asm("r9") = a6;
	asm volatile ("syscall"
		      : "=a"(ret)
		      : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
		      : "rcx", "r11", "memory");
	return ret;
}


static void sigsegv_handler(int signo, siginfo_t *info, void *ucontext)
{
	pthread_mutex_lock(&mutex);

	memcpy(&siginfo, info, sizeof(siginfo_t));

	pthread_cond_signal(&cond);
	pthread_mutex_unlock(&mutex);

	syscall_raw(SYS_exit, 0, 0, 0, 0, 0, 0);
}

static void sigusr1_handler(int signo, siginfo_t *info, void *ucontext)
{
	pthread_mutex_lock(&mutex);

	memcpy(&siginfo, info, sizeof(siginfo_t));

	pthread_cond_signal(&cond);
	pthread_mutex_unlock(&mutex);
}

static void *thread_segv_with_pkey0_disabled(void *ptr)
{
	/* Disable MPK 0 (and all others too) */
	__write_pkey_reg(0x55555555);

        /* Segfault (with SEGV_MAPERR) */
	*(int *) (0x1) = 1;
	return NULL;
}

static void *thread_segv_pkuerr_stack(void *ptr)
{
	/* Disable MPK 0 (and all others too) */
	__write_pkey_reg(0x55555555);

        /* After we disable MPK 0, we can't access the stack to return */
	return NULL;
}

static void *thread_segv_maperr_ptr(void *ptr)
{
	stack_t *stack = ptr;
	int *bad = (int *) 1;

	/*
	 * Setup alternate signal stack, which should be pkey_mprotect()ed by
	 * MPK 0. The thread's stack cannot be used for signals because it is
	 * not accessible by the default init_pkru value of 0x55555554.
	 */
        syscall_raw(SYS_sigaltstack, (long)stack, 0, 0, 0, 0, 0);

        /* Disable MPK 0.  Only MPK 1 is enabled. */
	__write_pkey_reg(0x55555551);

        /* Segfault */
	*bad = 1;
	syscall_raw(SYS_exit, 0, 0, 0, 0, 0, 0);
	return NULL;
}

/*
 * Verify that the sigsegv handler is invoked when pkey 0 is disabled.
 * Note that the new thread stack and the alternate signal stack is
 * protected by MPK 0.
 */
static void test_sigsegv_handler_with_pkey0_disabled(void)
{
	struct sigaction sa;

	sa.sa_flags = SA_SIGINFO;

	sa.sa_sigaction = sigsegv_handler;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGSEGV, &sa, NULL) == -1) {
		perror("sigaction");
		exit(EXIT_FAILURE);
	}

	memset(&siginfo, 0, sizeof(siginfo));

	pthread_t thr;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

	pthread_create(&thr, &attr, thread_segv_with_pkey0_disabled, NULL);

	pthread_mutex_lock(&mutex);
	while(siginfo.si_signo == 0)
		pthread_cond_wait(&cond, &mutex);
	pthread_mutex_unlock(&mutex);

	assert(siginfo.si_signo == SIGSEGV);
	assert(siginfo.si_code == SEGV_MAPERR);
	assert(siginfo.si_addr == (void *)1);
	printf("%s passed!\n", __func__);
}

/*
 * Verify that the sigsegv handler is invoked when pkey 0 is disabled.
 * Note that the new thread stack and the alternate signal stack is
 * protected by MPK 0, which renders them inaccessible when MPK 0
 * is disabled. So just the return from the thread should cause a
 * segfault with SEGV_PKUERR.
 */
static void test_sigsegv_handler_cannot_access_stack(void)
{
	struct sigaction sa;

	sa.sa_flags = SA_SIGINFO;

	sa.sa_sigaction = sigsegv_handler;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGSEGV, &sa, NULL) == -1) {
		perror("sigaction");
		exit(EXIT_FAILURE);
	}

	memset(&siginfo, 0, sizeof(siginfo));

	pthread_t thr;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

	pthread_create(&thr, &attr, thread_segv_pkuerr_stack, NULL);

	pthread_mutex_lock(&mutex);
	while(siginfo.si_signo == 0)
		pthread_cond_wait(&cond, &mutex);
	pthread_mutex_unlock(&mutex);

	assert(siginfo.si_signo == SIGSEGV);
	assert(siginfo.si_code == SEGV_PKUERR);
	printf("%s passed!\n", __func__);
}

/*
 * Verify that the sigsegv handler that uses an alternate signal stack
 * is correctly invoked for a thread which uses a non-zero MPK to protect
 * its own stack, and disables all other MPKs (including 0).
 */
static void test_sigsegv_handler_with_different_pkey_for_stack(void)
{
	struct sigaction sa;
	static stack_t sigstack;
        void *stack;
	int pkey;
	int parentPid = 0;
	int childPid = 0;

	sa.sa_flags = SA_SIGINFO | SA_ONSTACK;

	sa.sa_sigaction = sigsegv_handler;

	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGSEGV, &sa, NULL) == -1) {
		perror("sigaction");
		exit(EXIT_FAILURE);
	}

	stack = mmap(0, STACK_SIZE, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	assert(stack != MAP_FAILED);

	/* Allow access to MPK 0 and MPK 1 */
	__write_pkey_reg(0x55555550);

	/* Protect the new stack with MPK 1 */
	pkey = pkey_alloc(0, 0);
	pkey_mprotect(stack, STACK_SIZE, PROT_READ | PROT_WRITE, pkey);

        /* Set up alternate signal stack that will use the default MPK */
	sigstack.ss_sp = mmap(0, STACK_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
			      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	sigstack.ss_flags = 0;
	sigstack.ss_size = STACK_SIZE;

	memset(&siginfo, 0, sizeof(siginfo));

        /* Use clone to avoid newer glibcs using rseq on new threads */
	long ret = syscall_raw(SYS_clone,
			       CLONE_VM | CLONE_FS | CLONE_FILES |
			       CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM |
			       CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID |
			       CLONE_DETACHED,
			       (long)((char *)(stack) + STACK_SIZE),
			       (long)&parentPid,
			       (long)&childPid, 0, 0);

	if (ret < 0) {
		errno = -ret;
		perror("clone");
	}  else if (ret == 0) {
		thread_segv_maperr_ptr(&sigstack);
		syscall_raw(SYS_exit, 0, 0, 0, 0, 0, 0);
	}

	pthread_mutex_lock(&mutex);
	while(siginfo.si_signo == 0)
		pthread_cond_wait(&cond, &mutex);
	pthread_mutex_unlock(&mutex);

	assert(siginfo.si_signo == SIGSEGV);
	assert(siginfo.si_code == SEGV_MAPERR);
	assert(siginfo.si_addr == (void *)1);
	printf("%s passed!\n", __func__);
}

/*
 * Verify that the PKRU value set by the application is correctly
 * restored upon return from signal handling.
 */
static void test_pkru_preserved_after_sigusr1(void)
{
	struct sigaction sa;
	unsigned long pkru = 0x45454544;

	sa.sa_flags = SA_SIGINFO;

	sa.sa_sigaction = sigusr1_handler;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGUSR1, &sa, NULL) == -1) {
		perror("sigaction");
		exit(EXIT_FAILURE);
	}

	memset(&siginfo, 0, sizeof(siginfo));

	__write_pkey_reg(pkru);

	raise(SIGUSR1);

	pthread_mutex_lock(&mutex);
	while(siginfo.si_signo == 0)
		pthread_cond_wait(&cond, &mutex);
	pthread_mutex_unlock(&mutex);

	/* Ensure the pkru value is the same after returning from signal. */
	assert(pkru == __read_pkey_reg());
	assert(siginfo.si_signo == SIGUSR1);
	printf("%s passed!\n", __func__);
}


void (*pkey_tests[])(void)  = {
	test_sigsegv_handler_with_pkey0_disabled,
	test_sigsegv_handler_cannot_access_stack,
	test_sigsegv_handler_with_different_pkey_for_stack,
	test_pkru_preserved_after_sigusr1,
};

int main(int argc, char *argv[])
{
	size_t i;

	for (i = 0; i < sizeof(pkey_tests)/sizeof(pkey_tests[0]); i++) {
		(*pkey_tests[i])();
	}

	printf("All pkey-signal-handling tests PASSED!\n");
	return 0;
}

