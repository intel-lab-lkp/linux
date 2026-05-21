// SPDX-License-Identifier: GPL-2.0-only
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <ucontext.h>
#include <sys/time.h>
#include <linux/ptrace.h>
#include "kselftest_harness.h"

#define RISCV_V_MAGIC		0x53465457
#define END_MAGIC		0
#define DEFAULT_VALUE		2
#define SIGNAL_HANDLER_OVERRIDE	3

static void simple_handle(int sig_no, siginfo_t *info, void *vcontext)
{
	ucontext_t *context = vcontext;

	context->uc_mcontext.__gregs[REG_PC] = context->uc_mcontext.__gregs[REG_PC] + 4;
}

static void vector_override(int sig_no, siginfo_t *info, void *vcontext)
{
	ucontext_t *context = vcontext;

	// vector state
	struct __riscv_extra_ext_header *ext;
	struct __riscv_v_ext_state *v_ext_state;

	/* Find the vector context. */
	ext = (void *)(&context->uc_mcontext.__fpregs);
	if (ext->hdr.magic != RISCV_V_MAGIC) {
		fprintf(stderr, "bad vector magic: %x\n", ext->hdr.magic);
		abort();
	}

	v_ext_state = (void *)((char *)(ext) + sizeof(*ext));

	*(int *)v_ext_state->datap = SIGNAL_HANDLER_OVERRIDE;

	context->uc_mcontext.__gregs[REG_PC] = context->uc_mcontext.__gregs[REG_PC] + 4;
}

static int vector_sigreturn(int data, void (*handler)(int, siginfo_t *, void *))
{
	int after_sigreturn;
	struct sigaction sig_action = {
		.sa_sigaction = handler,
		.sa_flags = SA_SIGINFO
	};

	sigaction(SIGSEGV, &sig_action, 0);

	asm(".option push				\n\
		.option		arch, +v		\n\
		vsetivli	x0, 1, e32, m1, ta, ma	\n\
		vmv.s.x		v0, %1			\n\
		# Generate SIGSEGV			\n\
		lw		a0, 0(x0)		\n\
		vmv.x.s		%0, v0			\n\
		.option pop" : "=r" (after_sigreturn) : "r" (data));

	return after_sigreturn;
}

#define V_TEST_PATTERN_SIGNAL 0x98
int nulled_val;
static void sigalrm_handler(int sig, siginfo_t *info, void *vcontext)
{
	ucontext_t *context = vcontext;
	struct __riscv_extra_ext_header *ext;
	struct __riscv_ctx_hdr *hdr;
	uint8_t *ext_ptr;

	/* Find the vector context */
	ext = (void *)(&context->uc_mcontext.__fpregs);
	ext_ptr = (uint8_t *)ext;
	hdr = &ext->hdr;

	while (hdr->magic != END_MAGIC) {
		if (hdr->magic == RISCV_V_MAGIC) {
			struct __riscv_v_ext_state *v_state = (struct __riscv_v_ext_state *)(hdr + 1);
			/* Assume a valid datap */
			nulled_val = *(int *)v_state->datap;
			/* Fill all vector registers with magic pattern */
			memset(v_state->datap, V_TEST_PATTERN_SIGNAL, v_state->vlenb * 32);
			/*
			 * We must also set the vector configuration so that when
			 * userspace reads v0, it uses a valid element width (e8).
			 */
			v_state->vl = v_state->vlenb;
			v_state->vtype = 0; /* e8, m1, tu, mu */
			break;
		}
		/* Move to the next extension header */
		ext_ptr += hdr->size;
		hdr = (struct __riscv_ctx_hdr *)ext_ptr;
	}
}

TEST(test_signal_syscall_ucontext) {
	struct sigaction sa;

	/* Make sure we get V in ucontext by executing vsetvli */
	asm volatile (
	    ".option push\n\t"
	    ".option		arch, +v\n\t"
	    "vsetivli	x0, 1, e32, m1, ta, ma\n\t"
	    ".option pop\n\t" : : :);

	sa.sa_flags = SA_SIGINFO;
	sa.sa_sigaction = sigalrm_handler;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGALRM, &sa, NULL) == -1)
		ksft_exit_fail_msg("Failed to register signal handler\n");

	/* Setup a 10ms timer to unblock us from pause() */
	struct itimerval itv = {{0, 0}, {0, 10000}};

	setitimer(ITIMER_REAL, &itv, NULL);

	pause();

	/*
	 * If the kernel successfully parsed and restored our modified ucontext,
	 * v0 will contain V_TEST_PATTERN_SIGNAL.
	 */
	unsigned char v0_val;

	asm volatile(
			".option push\n\t"
			".option arch, +zve32x\n\t"
			"vmv.x.s %0, v0\n\t"
			".option pop\n\t"
			: "=r" (v0_val)
		    );

	EXPECT_EQ(v0_val, V_TEST_PATTERN_SIGNAL);
	EXPECT_EQ(nulled_val, -1);
}

TEST(vector_restore)
{
	int result;

	result = vector_sigreturn(DEFAULT_VALUE, &simple_handle);

	EXPECT_EQ(DEFAULT_VALUE, result);
}

TEST(vector_restore_signal_handler_override)
{
	int result;

	result = vector_sigreturn(DEFAULT_VALUE, &vector_override);

	EXPECT_EQ(SIGNAL_HANDLER_OVERRIDE, result);
}

TEST_HARNESS_MAIN
