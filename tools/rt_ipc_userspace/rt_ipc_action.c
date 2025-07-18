#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <limits.h>
#include <sys/resource.h>
#include <sys/sysinfo.h>
#include <sys/mman.h>

#include "rt_ipc_action.h"

#define RT_IPC_RESTORER 0x04000000

#define LP_SIZE "8"
# define CFI_STRINGIFY(Name) CFI_STRINGIFY2 (Name)
# define CFI_STRINGIFY2(Name) #Name
# define attribute_hidden __attribute__ ((visibility ("hidden")))

/* Align a value by rounding down to closest size.
   e.g. Using size of 4096, we get this behavior:
	{4095, 4096, 4097} = {0, 4096, 4096}.  */
#define ALIGN_DOWN(base, size)	((base) & -((__typeof__ (base)) (size)))

/* Align a value by rounding up to closest size.
   e.g. Using size of 4096, we get this behavior:
	{4095, 4096, 4097} = {4096, 4096, 8192}.

  Note: The size argument has side effects (expanded multiple times).  */
#define ALIGN_UP(base, size)	ALIGN_DOWN ((base) + (size) - 1, (size))

#define UCHAR_WIDTH 8
//#define ULONG_WIDTH 64
#define __NSIG_WORDS (ALIGN_UP ((_NSIG - 1), ULONG_WIDTH) / ULONG_WIDTH)
#define __NSIG_BYTES (__NSIG_WORDS * (ULONG_WIDTH / UCHAR_WIDTH))

#define STUB(act, sigsetsize) (sigsetsize)

#ifdef RT_IPC_RESTORER
#define HAS_RT_IPC_RESTORER 1
#endif

extern void restore_rt (void) asm ("__restore_rt") attribute_hidden;

#define SET_RT_IPC_RESTORER(kact, act)			\
  (kact)->flags = (act)->flags | RT_IPC_RESTORER;	\
  (kact)->rt_ipc_restorer = &restore_rt

#define do_cfa_expr						\
  "	.byte 0x0f\n"		/* DW_CFA_def_cfa_expression */	\
  "	.uleb128 2f-1f\n"	/* length */			\
  "1:	.byte 0x77\n"		/* DW_OP_breg7 */		\
  "	.byte 0x06\n"		/* DW_OP_deref */		\
  "2:"

#define do_expr(regno, offset)					\
  "	.byte 0x10\n"		/* DW_CFA_expression */		\
  "	.uleb128 " CFI_STRINGIFY (regno) "\n"			\
  "	.uleb128 2f-1f\n"	/* length */			\
  "1:	.byte 0x77\n"		/* DW_OP_breg7 */		\
  "2:"

#define RESTORE(name, syscall) RESTORE2 (name, syscall)
# define RESTORE2(name, syscall) \
asm									\
  (									\
   /* `nop' for debuggers assuming `call' should not disalign the code.  */ \
   "	nop\n"								\
   ".align 16\n"							\
   ".LSTART_" #name ":\n"						\
   "	.type __" #name ",@function\n"					\
   "__" #name ":\n"							\
   "	movq $" #syscall ", %rax\n"					\
   "	syscall\n"							\
   ".LEND_" #name ":\n"							\
   ".section .eh_frame,\"a\",@progbits\n"				\
   ".LSTARTFRAME_" #name ":\n"						\
   "	.long .LENDCIE_" #name "-.LSTARTCIE_" #name "\n"		\
   ".LSTARTCIE_" #name ":\n"						\
   "	.long 0\n"	/* CIE ID */					\
   "	.byte 1\n"	/* Version number */				\
   "	.string \"zRS\"\n" /* NUL-terminated augmentation string */	\
   "	.uleb128 1\n"	/* Code alignment factor */			\
   "	.sleb128 -8\n"	/* Data alignment factor */			\
   "	.uleb128 16\n"	/* Return address register column (rip) */	\
   /* Augmentation value length */					\
   "	.uleb128 .LENDAUGMNT_" #name "-.LSTARTAUGMNT_" #name "\n"	\
   ".LSTARTAUGMNT_" #name ":\n"						\
   "	.byte 0x1b\n"	/* DW_EH_PE_pcrel|DW_EH_PE_sdata4. */		\
   ".LENDAUGMNT_" #name ":\n"						\
   "	.align " LP_SIZE "\n"						\
   ".LENDCIE_" #name ":\n"						\
   "	.long .LENDFDE_" #name "-.LSTARTFDE_" #name "\n" /* FDE len */	\
   ".LSTARTFDE_" #name ":\n"						\
   "	.long .LSTARTFDE_" #name "-.LSTARTFRAME_" #name "\n" /* CIE */	\
   /* `LSTART_' is subtracted 1 as debuggers assume a `call' here.  */	\
   "	.long (.LSTART_" #name "-1)-.\n" /* PC-relative start addr.  */	\
   "	.long .LEND_" #name "-(.LSTART_" #name "-1)\n"			\
   "	.uleb128 0\n"			/* FDE augmentation length */	\
   do_cfa_expr								\
   do_expr (8 /* r8 */, oR8)						\
   do_expr (9 /* r9 */, oR9)						\
   do_expr (10 /* r10 */, oR10)						\
   do_expr (11 /* r11 */, oR11)						\
   do_expr (12 /* r12 */, oR12)						\
   do_expr (13 /* r13 */, oR13)						\
   do_expr (14 /* r14 */, oR14)						\
   do_expr (15 /* r15 */, oR15)						\
   do_expr (5 /* rdi */, oRDI)						\
   do_expr (4 /* rsi */, oRSI)						\
   do_expr (6 /* rbp */, oRBP)						\
   do_expr (3 /* rbx */, oRBX)						\
   do_expr (1 /* rdx */, oRDX)						\
   do_expr (0 /* rax */, oRAX)						\
   do_expr (2 /* rcx */, oRCX)						\
   do_expr (7 /* rsp */, oRSP)						\
   do_expr (16 /* rip */, oRIP)						\
   /* libgcc-4.1.1 has only `DWARF_FRAME_REGISTERS == 17'.  */		\
   /* do_expr (49 |* rflags *|, oEFL) */				\
   /* `cs'/`ds'/`fs' are unaligned and a different size.  */		\
   /* gas: Error: register save offset not a multiple of 8  */		\
   "	.align " LP_SIZE "\n"						\
   ".LENDFDE_" #name ":\n"						\
   "	.previous\n"							\
   );
/* The return code for realtime-signals.  */
RESTORE (restore_rt, __NR_rt_ipc_return)

void *test(void __maybe_unused *data)
{
    while (1) {
        //printf("[%s:%d] pid: %d tid: %d ppid: %d\n", __func__, __LINE__, getpid(), gettid(), getppid());
        sleep(INT_MAX);
    }
    return NULL;
}

int rt_ipc_action(const struct rt_ipc_action *act)
{
    int result;
    struct rt_ipc_action kact;
    pthread_t ptrd;

    if (!act) {
        return -EINVAL;
    }

    kact.activation = act->activation;

    SET_RT_IPC_RESTORER (&kact, act);

    for (int i = 0; i < RT_IPC_ACTIVATION_THREAD_NUM; ++i) {
        int res = pthread_create(&ptrd, NULL, test, NULL);
        assert(res == 0);
    }

    //pr_info("pid: %d activation: %p rt_ipc_restorer: %p\n", getpid(), kact.activation, kact.rt_ipc_restorer);

    result = syscall(SYS_rt_ipc_register, act ? &kact : NULL);
    return result;
}
