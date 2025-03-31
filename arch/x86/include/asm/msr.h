/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_MSR_H
#define _ASM_X86_MSR_H

#include "msr-index.h"

#ifndef __ASSEMBLER__

#include <asm/asm.h>
#include <asm/errno.h>
#include <asm/cpufeature.h>
#include <asm/cpumask.h>
#include <uapi/asm/msr.h>
#include <asm/shared/msr.h>

#include <linux/percpu.h>

struct msr_info {
	u32			msr_no;
	struct msr		reg;
	struct msr __percpu	*msrs;
	int			err;
};

struct msr_regs_info {
	u32 *regs;
	int err;
};

struct saved_msr {
	bool valid;
	struct msr_info info;
};

struct saved_msrs {
	unsigned int num;
	struct saved_msr *array;
};

/*
 * both i386 and x86_64 returns 64-bit value in edx:eax, but gcc's "A"
 * constraint has different meanings. For i386, "A" means exactly
 * edx:eax, while for x86_64 it doesn't mean rdx:rax or edx:eax. Instead,
 * it means rax *or* rdx.
 */
#ifdef CONFIG_X86_64
/* Using 64-bit values saves one instruction clearing the high half of low */
#define DECLARE_ARGS(val, low, high)	unsigned long low, high
#define EAX_EDX_VAL(val, low, high)	((low) | (high) << 32)
#define EAX_EDX_RET(val, low, high)	"=a" (low), "=d" (high)
#else
#define DECLARE_ARGS(val, low, high)	unsigned long long val
#define EAX_EDX_VAL(val, low, high)	(val)
#define EAX_EDX_RET(val, low, high)	"=A" (val)
#endif

/*
 * Be very careful with includes. This header is prone to include loops.
 */
#include <asm/atomic.h>
#include <linux/tracepoint-defs.h>

#ifdef CONFIG_TRACEPOINTS
DECLARE_TRACEPOINT(read_msr);
DECLARE_TRACEPOINT(write_msr);
DECLARE_TRACEPOINT(rdpmc);
extern void do_trace_write_msr(unsigned int msr, u64 val, int failed);
extern void do_trace_read_msr(unsigned int msr, u64 val, int failed);
extern void do_trace_rdpmc(unsigned int msr, u64 val, int failed);
#else
static inline void do_trace_write_msr(unsigned int msr, u64 val, int failed) {}
static inline void do_trace_read_msr(unsigned int msr, u64 val, int failed) {}
static inline void do_trace_rdpmc(unsigned int msr, u64 val, int failed) {}
#endif

#ifdef CONFIG_CC_IS_GCC
#define ASM_WRMSRNS_IMM			\
	" .insn VEX.128.F3.M7.W0 0xf6 /0, %[val], %[msr]%{:u32}\n\t"
#endif

#ifdef CONFIG_CC_IS_CLANG
/*
 * clang doesn't support the insn directive.
 *
 * The register operand is encoded as %rax because all uses of the immediate
 * form MSR access instructions reference %rax as the register operand.
 */
#define ASM_WRMSRNS_IMM			\
	" .byte 0xc4,0xe7,0x7a,0xf6,0xc0; .long %c[msr]"
#endif

#define PREPARE_RDX_FOR_WRMSR		\
	"mov %%rax, %%rdx\n\t"		\
	"shr $0x20, %%rdx\n\t"

#define PREPARE_RCX_RDX_FOR_WRMSR	\
	"mov %[msr], %%ecx\n\t"		\
	PREPARE_RDX_FOR_WRMSR

enum pv_msr_action {
	PV_MSR_NATIVE,
	PV_MSR_PV,
	PV_MSR_IGNORE,
};

#ifdef CONFIG_XEN_PV
static __always_inline enum pv_msr_action get_pv_msr_action(const u32 msr)
{
	if (!__builtin_constant_p(msr)) {
		/* Is it safe to blindly do so? */
		return PV_MSR_NATIVE;
	}

	switch (msr) {
	case MSR_FS_BASE:
	case MSR_KERNEL_GS_BASE:
	case MSR_GS_BASE:
	case MSR_CORE_PERF_GLOBAL_OVF_CTRL:
	case MSR_CORE_PERF_GLOBAL_STATUS:
	case MSR_CORE_PERF_GLOBAL_CTRL:
	case MSR_CORE_PERF_FIXED_CTR_CTRL:
	case MSR_IA32_APICBASE:
		return PV_MSR_PV;

	case MSR_STAR:
	case MSR_CSTAR:
	case MSR_LSTAR:
	case MSR_SYSCALL_MASK:
	case MSR_IA32_SYSENTER_CS:
	case MSR_IA32_SYSENTER_ESP:
	case MSR_IA32_SYSENTER_EIP:
		return PV_MSR_IGNORE;

	default:
		/*
		 * MSR access instructions RDMSR/WRMSR/WRMSRNS will be used.
		 *
		 * The hypervisor will trap and inject #GP into the guest and
		 * the MSR access instruction will be skipped.
		 */
		return PV_MSR_NATIVE;
	}
}

extern void asm_xen_write_msr(void);
#else
static __always_inline enum pv_msr_action get_pv_msr_action(const u32 msr)
{
	return PV_MSR_NATIVE;
}
#endif

static __always_inline unsigned long long __rdmsr(unsigned int msr)
{
	DECLARE_ARGS(val, low, high);

	asm volatile("1: rdmsr\n"
		     "2:\n"
		     _ASM_EXTABLE_TYPE(1b, 2b, EX_TYPE_RDMSR)
		     : EAX_EDX_RET(val, low, high) : "c" (msr));

	return EAX_EDX_VAL(val, low, high);
}

#define native_rdmsr(msr, val1, val2)			\
do {							\
	u64 __val = __rdmsr((msr));			\
	(void)((val1) = (u32)__val);			\
	(void)((val2) = (u32)(__val >> 32));		\
} while (0)

static __always_inline u64 native_rdmsrl(const u32 msr)
{
	return __rdmsr(msr);
}

static inline unsigned long long native_read_msr(unsigned int msr)
{
	unsigned long long val;

	val = __rdmsr(msr);

	if (tracepoint_enabled(read_msr))
		do_trace_read_msr(msr, val, 0);

	return val;
}

static inline unsigned long long native_read_msr_safe(unsigned int msr,
						      int *err)
{
	DECLARE_ARGS(val, low, high);

	asm volatile("1: rdmsr ; xor %[err],%[err]\n"
		     "2:\n\t"
		     _ASM_EXTABLE_TYPE_REG(1b, 2b, EX_TYPE_RDMSR_SAFE, %[err])
		     : [err] "=r" (*err), EAX_EDX_RET(val, low, high)
		     : "c" (msr));
	if (tracepoint_enabled(read_msr))
		do_trace_read_msr(msr, EAX_EDX_VAL(val, low, high), *err);
	return EAX_EDX_VAL(val, low, high);
}

/*
 * Non-serializing WRMSR, when available.
 * Falls back to a serializing WRMSR.
 */
static __always_inline bool __native_wrmsr_variable(const u32 msr, const u64 val, const int type)
{
#ifdef CONFIG_X86_64
	BUILD_BUG_ON(__builtin_constant_p(msr));
#endif

	asm_inline volatile goto(
		"1:\n"
		ALTERNATIVE("ds wrmsr",
			    ASM_WRMSRNS,
			    X86_FEATURE_WRMSRNS)
		_ASM_EXTABLE_TYPE(1b, %l[badmsr], %c[type])

		:
		: "c" (msr), "a" ((u32)val), "d" ((u32)(val >> 32)), [type] "i" (type)
		: "memory"
		: badmsr);

	return false;

badmsr:
	return true;
}

#ifdef CONFIG_X86_64
/*
 * Non-serializing WRMSR or its immediate form, when available.
 * Falls back to a serializing WRMSR.
 */
static __always_inline bool __native_wrmsr_constant(const u32 msr, const u64 val, const int type)
{
	BUILD_BUG_ON(!__builtin_constant_p(msr));

	/*
	 * WRMSR is 2 bytes.  WRMSRNS is 3 bytes.  Pad WRMSR with a redundant
	 * DS prefix to avoid a trailing NOP.
	 */
	asm_inline volatile goto(
		"1:\n"
		ALTERNATIVE_2(PREPARE_RCX_RDX_FOR_WRMSR
			      "2: ds wrmsr",
			      PREPARE_RCX_RDX_FOR_WRMSR
			      ASM_WRMSRNS,
			      X86_FEATURE_WRMSRNS,
			      ASM_WRMSRNS_IMM,
			      X86_FEATURE_MSR_IMM)
		_ASM_EXTABLE_TYPE(1b, %l[badmsr], %c[type])	/* For WRMSRNS immediate */
		_ASM_EXTABLE_TYPE(2b, %l[badmsr], %c[type])	/* For WRMSR(NS) */

		:
		: [val] "a" (val), [msr] "i" (msr), [type] "i" (type)
		: "memory", "ecx", "rdx"
		: badmsr);

	return false;

badmsr:
	return true;
}
#endif

static __always_inline bool __native_wrmsr(const u32 msr, const u64 val, const int type)
{
#ifdef CONFIG_X86_64
	if (__builtin_constant_p(msr))
		return __native_wrmsr_constant(msr, val, type);
#endif

	return __native_wrmsr_variable(msr, val, type);
}

static __always_inline void native_wrmsr(const u32 msr, const u32 low, const u32 high)
{
	__native_wrmsr(msr, (u64)high << 32 | low, EX_TYPE_WRMSR);
}

static __always_inline void native_wrmsrl(const u32 msr, const u64 val)
{
	__native_wrmsr(msr, val, EX_TYPE_WRMSR);
}

static inline void notrace native_write_msr(const u32 msr, const u64 val)
{
	__native_wrmsr(msr, val, EX_TYPE_WRMSR);

	if (tracepoint_enabled(write_msr))
		do_trace_write_msr(msr, val, 0);
}

static inline int notrace native_write_msr_safe(const u32 msr, const u64 val)
{
	int err = __native_wrmsr(msr, val, EX_TYPE_WRMSR_SAFE) ? -EIO : 0;

	if (tracepoint_enabled(write_msr))
		do_trace_write_msr(msr, val, err);

	return err;
}

static __always_inline bool __wrmsr_variable(const u32 msr, const u64 val, const int type)
{
#ifdef CONFIG_X86_64
	BUILD_BUG_ON(__builtin_constant_p(msr));

	asm_inline volatile goto(
		ALTERNATIVE(PREPARE_RDX_FOR_WRMSR,
			    "call asm_xen_write_msr\n\t"
			    "jnz 2f\n\t",
			    X86_FEATURE_XENPV)
		ALTERNATIVE("1: ds wrmsr",
			    ASM_WRMSRNS,
			    X86_FEATURE_WRMSRNS)
		"2:\n"
		_ASM_EXTABLE_TYPE(1b, %l[badmsr], %c[type])	/* For WRMSR(NS) */

		: ASM_CALL_CONSTRAINT
		: "a" (val), "c" (msr), [type] "i" (type)
		: "memory", "rdx"
		: badmsr);

	return false;

badmsr:
	return true;
#else
	return __native_wrmsr_variable(msr, val, type);
#endif
}

static __always_inline bool __wrmsr_variable_all(const u32 msr, const u64 val, const int type)
{
	const enum pv_msr_action action = get_pv_msr_action(msr);

	if (action == PV_MSR_PV) {
		return __wrmsr_variable(msr, val, type);
	} else if (action == PV_MSR_IGNORE) {
		if (cpu_feature_enabled(X86_FEATURE_XENPV))
			return false;
	}

	return __native_wrmsr_variable(msr, val, type);
}

#ifdef CONFIG_X86_64
static __always_inline bool __wrmsr_constant(const u32 msr, const u64 val, const int type)
{
	BUILD_BUG_ON(!__builtin_constant_p(msr));

	asm_inline volatile goto(
		"1:\n"
		ALTERNATIVE_2(PREPARE_RCX_RDX_FOR_WRMSR,
			      "",
			      X86_FEATURE_MSR_IMM,
			      "mov %[msr], %%ecx\n\t"
			      "call asm_xen_write_msr\n\t"
			      "jnz 3f\n\t",
			      X86_FEATURE_XENPV)
		ALTERNATIVE_2("2: ds wrmsr",
			      ASM_WRMSRNS,
			      X86_FEATURE_WRMSRNS,
			      ASM_WRMSRNS_IMM,
			      X86_FEATURE_MSR_IMM)
		"3:\n"
		_ASM_EXTABLE_TYPE(1b, %l[badmsr], %c[type])	/* For WRMSRNS immediate */
		_ASM_EXTABLE_TYPE(2b, %l[badmsr], %c[type])	/* For WRMSR(NS) */

		: ASM_CALL_CONSTRAINT
		: [val] "a" (val), [msr] "i" (msr), [type] "i" (type)
		: "memory", "ecx", "rdx"
		: badmsr);

	return false;

badmsr:
	return true;
}

static __always_inline bool __wrmsr_constant_all(const u32 msr, const u64 val, const int type)
{
	const enum pv_msr_action action = get_pv_msr_action(msr);

	if (action == PV_MSR_PV) {
		return __wrmsr_constant(msr, val, type);
	} else if (action == PV_MSR_IGNORE) {
		if (cpu_feature_enabled(X86_FEATURE_XENPV))
			return false;
	}

	return __native_wrmsr_constant(msr, val, type);
}
#endif

static __always_inline bool __wrmsr(const u32 msr, const u64 val, const int type)
{
#ifdef CONFIG_X86_64
	if (__builtin_constant_p(msr))
		return __wrmsr_constant_all(msr, val, type);
#endif

	return __wrmsr_variable_all(msr, val, type);
}

static __always_inline void wrmsr(const u32 msr, const u32 low, const u32 high)
{
	__wrmsr(msr, (u64)high << 32 | low, EX_TYPE_WRMSR);
}

static __always_inline void wrmsrl(const u32 msr, const u64 val)
{
	__wrmsr(msr, val, EX_TYPE_WRMSR);
}

static __always_inline int wrmsr_safe(const u32 msr, const u32 low, const u32 high)
{
	return __wrmsr(msr, (u64)high << 32 | low, EX_TYPE_WRMSR_SAFE) ? -EIO : 0;
}

static __always_inline int wrmsrl_safe(const u32 msr, const u64 val)
{
	return __wrmsr(msr, val, EX_TYPE_WRMSR_SAFE) ? -EIO : 0;
}

extern int rdmsr_safe_regs(u32 regs[8]);
extern int wrmsr_safe_regs(u32 regs[8]);

/**
 * rdtsc() - returns the current TSC without ordering constraints
 *
 * rdtsc() returns the result of RDTSC as a 64-bit integer.  The
 * only ordering constraint it supplies is the ordering implied by
 * "asm volatile": it will put the RDTSC in the place you expect.  The
 * CPU can and will speculatively execute that RDTSC, though, so the
 * results can be non-monotonic if compared on different CPUs.
 */
static __always_inline unsigned long long rdtsc(void)
{
	DECLARE_ARGS(val, low, high);

	asm volatile("rdtsc" : EAX_EDX_RET(val, low, high));

	return EAX_EDX_VAL(val, low, high);
}

/**
 * rdtsc_ordered() - read the current TSC in program order
 *
 * rdtsc_ordered() returns the result of RDTSC as a 64-bit integer.
 * It is ordered like a load to a global in-memory counter.  It should
 * be impossible to observe non-monotonic rdtsc_unordered() behavior
 * across multiple CPUs as long as the TSC is synced.
 */
static __always_inline unsigned long long rdtsc_ordered(void)
{
	DECLARE_ARGS(val, low, high);

	/*
	 * The RDTSC instruction is not ordered relative to memory
	 * access.  The Intel SDM and the AMD APM are both vague on this
	 * point, but empirically an RDTSC instruction can be
	 * speculatively executed before prior loads.  An RDTSC
	 * immediately after an appropriate barrier appears to be
	 * ordered as a normal load, that is, it provides the same
	 * ordering guarantees as reading from a global memory location
	 * that some other imaginary CPU is updating continuously with a
	 * time stamp.
	 *
	 * Thus, use the preferred barrier on the respective CPU, aiming for
	 * RDTSCP as the default.
	 */
	asm volatile(ALTERNATIVE_2("rdtsc",
				   "lfence; rdtsc", X86_FEATURE_LFENCE_RDTSC,
				   "rdtscp", X86_FEATURE_RDTSCP)
			: EAX_EDX_RET(val, low, high)
			/* RDTSCP clobbers ECX with MSR_TSC_AUX. */
			:: "ecx");

	return EAX_EDX_VAL(val, low, high);
}

static inline unsigned long long native_read_pmc(int counter)
{
	DECLARE_ARGS(val, low, high);

	asm volatile("rdpmc" : EAX_EDX_RET(val, low, high) : "c" (counter));
	if (tracepoint_enabled(rdpmc))
		do_trace_rdpmc(counter, EAX_EDX_VAL(val, low, high), 0);
	return EAX_EDX_VAL(val, low, high);
}

#ifdef CONFIG_PARAVIRT_XXL
#include <asm/paravirt.h>
#else
#include <linux/errno.h>
/*
 * Access to machine-specific registers (available on 586 and better only)
 * Note: the rd* operations modify the parameters directly (without using
 * pointer indirection), this allows gcc to optimize better
 */

#define rdmsr(msr, low, high)					\
do {								\
	u64 __val = native_read_msr((msr));			\
	(void)((low) = (u32)__val);				\
	(void)((high) = (u32)(__val >> 32));			\
} while (0)

#define rdmsrl(msr, val)			\
	((val) = native_read_msr((msr)))

static inline void wrmsrl(u32 msr, u64 val)
{
	native_write_msr(msr, val);
}

/* rdmsr with exception handling */
#define rdmsr_safe(msr, low, high)				\
({								\
	int __err;						\
	u64 __val = native_read_msr_safe((msr), &__err);	\
	(*low) = (u32)__val;					\
	(*high) = (u32)(__val >> 32);				\
	__err;							\
})

static inline int rdmsrl_safe(unsigned int msr, unsigned long long *p)
{
	int err;

	*p = native_read_msr_safe(msr, &err);
	return err;
}

#define rdpmc(counter, low, high)			\
do {							\
	u64 _l = native_read_pmc((counter));		\
	(low)  = (u32)_l;				\
	(high) = (u32)(_l >> 32);			\
} while (0)

#define rdpmcl(counter, val) ((val) = native_read_pmc(counter))

#endif	/* !CONFIG_PARAVIRT_XXL */

struct msr __percpu *msrs_alloc(void);
void msrs_free(struct msr __percpu *msrs);
int msr_set_bit(u32 msr, u8 bit);
int msr_clear_bit(u32 msr, u8 bit);

#ifdef CONFIG_SMP
int rdmsr_on_cpu(unsigned int cpu, u32 msr_no, u32 *l, u32 *h);
int wrmsr_on_cpu(unsigned int cpu, u32 msr_no, u32 l, u32 h);
int rdmsrl_on_cpu(unsigned int cpu, u32 msr_no, u64 *q);
int wrmsrl_on_cpu(unsigned int cpu, u32 msr_no, u64 q);
void rdmsr_on_cpus(const struct cpumask *mask, u32 msr_no, struct msr __percpu *msrs);
void wrmsr_on_cpus(const struct cpumask *mask, u32 msr_no, struct msr __percpu *msrs);
int rdmsr_safe_on_cpu(unsigned int cpu, u32 msr_no, u32 *l, u32 *h);
int wrmsr_safe_on_cpu(unsigned int cpu, u32 msr_no, u32 l, u32 h);
int rdmsrl_safe_on_cpu(unsigned int cpu, u32 msr_no, u64 *q);
int wrmsrl_safe_on_cpu(unsigned int cpu, u32 msr_no, u64 q);
int rdmsr_safe_regs_on_cpu(unsigned int cpu, u32 regs[8]);
int wrmsr_safe_regs_on_cpu(unsigned int cpu, u32 regs[8]);
#else  /*  CONFIG_SMP  */
static inline int rdmsr_on_cpu(unsigned int cpu, u32 msr_no, u32 *l, u32 *h)
{
	rdmsr(msr_no, *l, *h);
	return 0;
}
static inline int wrmsr_on_cpu(unsigned int cpu, u32 msr_no, u32 l, u32 h)
{
	wrmsr(msr_no, l, h);
	return 0;
}
static inline int rdmsrl_on_cpu(unsigned int cpu, u32 msr_no, u64 *q)
{
	rdmsrl(msr_no, *q);
	return 0;
}
static inline int wrmsrl_on_cpu(unsigned int cpu, u32 msr_no, u64 q)
{
	wrmsrl(msr_no, q);
	return 0;
}
static inline void rdmsr_on_cpus(const struct cpumask *m, u32 msr_no,
				struct msr __percpu *msrs)
{
	rdmsr_on_cpu(0, msr_no, raw_cpu_ptr(&msrs->l), raw_cpu_ptr(&msrs->h));
}
static inline void wrmsr_on_cpus(const struct cpumask *m, u32 msr_no,
				struct msr __percpu *msrs)
{
	wrmsr_on_cpu(0, msr_no, raw_cpu_read(msrs->l), raw_cpu_read(msrs->h));
}
static inline int rdmsr_safe_on_cpu(unsigned int cpu, u32 msr_no,
				    u32 *l, u32 *h)
{
	return rdmsr_safe(msr_no, l, h);
}
static inline int wrmsr_safe_on_cpu(unsigned int cpu, u32 msr_no, u32 l, u32 h)
{
	return wrmsrl_safe(msr_no, (u64)h << 32 | l);
}
static inline int rdmsrl_safe_on_cpu(unsigned int cpu, u32 msr_no, u64 *q)
{
	return rdmsrl_safe(msr_no, q);
}
static inline int wrmsrl_safe_on_cpu(unsigned int cpu, u32 msr_no, u64 q)
{
	return wrmsrl_safe(msr_no, q);
}
static inline int rdmsr_safe_regs_on_cpu(unsigned int cpu, u32 regs[8])
{
	return rdmsr_safe_regs(regs);
}
static inline int wrmsr_safe_regs_on_cpu(unsigned int cpu, u32 regs[8])
{
	return wrmsr_safe_regs(regs);
}
#endif  /* CONFIG_SMP */
#endif /* __ASSEMBLER__ */
#endif /* _ASM_X86_MSR_H */
