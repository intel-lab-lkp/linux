/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_MSR_H
#define _ASM_X86_MSR_H

#include "msr-index.h"

#ifndef __ASSEMBLER__

#include <asm/asm.h>
#include <asm/cpumask.h>
#include <uapi/asm/msr.h>
#include <asm/shared/msr.h>

#include <linux/errno.h>
#include <linux/types.h>
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
 * Be very careful with includes. This header is prone to include loops.
 */
#include <asm/atomic.h>
#include <linux/tracepoint-defs.h>

#ifdef CONFIG_TRACEPOINTS
DECLARE_TRACEPOINT(read_msr);
DECLARE_TRACEPOINT(write_msr);
DECLARE_TRACEPOINT(rdpmc);
extern void do_trace_write_msr(u32 msr, u64 val, int failed);
extern void do_trace_read_msr(u32 msr, u64 val, int failed);
extern void do_trace_rdpmc(u32 msr, u64 val, int failed);
#else
static inline void do_trace_write_msr(u32 msr, u64 val, int failed) {}
static inline void do_trace_read_msr(u32 msr, u64 val, int failed) {}
static inline void do_trace_rdpmc(u32 msr, u64 val, int failed) {}
#endif

/* The GNU Assembler (Gas) with Binutils 2.40 adds WRMSRNS support */
#if defined(CONFIG_AS_IS_GNU) && CONFIG_AS_VERSION >= 24000
#define ASM_WRMSRNS		"wrmsrns\n\t"
#else
#define ASM_WRMSRNS		_ASM_BYTES(0x0f,0x01,0xc6)
#endif

/* The GNU Assembler (Gas) with Binutils 2.41 adds the .insn directive support */
#if defined(CONFIG_AS_IS_GNU) && CONFIG_AS_VERSION >= 24100
#define ASM_RDMSR_IMM			\
	" .insn VEX.128.F2.M7.W0 0xf6 /0, %[msr]%{:u32}, %[val]\n\t"
#define ASM_WRMSRNS_IMM			\
	" .insn VEX.128.F3.M7.W0 0xf6 /0, %[val], %[msr]%{:u32}\n\t"
#else
/*
 * Note, clang also doesn't support the .insn directive.
 *
 * The register operand is encoded as %rax because all uses of the immediate
 * form MSR access instructions reference %rax as the register operand.
 */
#define ASM_RDMSR_IMM			\
	" .byte 0xc4,0xe7,0x7b,0xf6,0xc0; .long %c[msr]"
#define ASM_WRMSRNS_IMM			\
	" .byte 0xc4,0xe7,0x7a,0xf6,0xc0; .long %c[msr]"
#endif

#define RDMSR_AND_SAVE_RESULT		\
	"rdmsr\n\t"			\
	"shl $0x20, %%rdx\n\t"		\
	"or %%rdx, %%rax\n\t"

#define PREPARE_RDX_FOR_WRMSR		\
	"mov %%rax, %%rdx\n\t"		\
	"shr $0x20, %%rdx\n\t"

#define PREPARE_RCX_RDX_FOR_WRMSR	\
	"mov %[msr], %%ecx\n\t"		\
	PREPARE_RDX_FOR_WRMSR

/*
 * __rdmsr() and __wrmsr() are the two primitives which are the bare minimum MSR
 * accessors and should not have any tracing or other functionality piggybacking
 * on them - those are *purely* for accessing MSRs and nothing more. So don't even
 * think of extending them - you will be slapped with a stinking trout or a frozen
 * shark will reach you, wherever you are! You've been warned.
 */
static __always_inline bool __rdmsrq_variable(u32 msr, u64 *val, int type)
 {
#ifdef CONFIG_X86_64
	BUILD_BUG_ON(__builtin_constant_p(msr));

	asm_inline volatile goto(
		"1:\n"
		RDMSR_AND_SAVE_RESULT
		_ASM_EXTABLE_TYPE(1b, %l[badmsr], %c[type])	/* For RDMSR */

		: [val] "=a" (*val)
		: "c" (msr), [type] "i" (type)
		: "rdx"
		: badmsr);
#else
	asm_inline volatile goto(
		"1: rdmsr\n\t"
		_ASM_EXTABLE_TYPE(1b, %l[badmsr], %c[type])	/* For RDMSR */

		: "=A" (*val)
		: "c" (msr), [type] "i" (type)
		:
		: badmsr);
#endif

	return false;

badmsr:
	*val = 0;

	return true;
}

#ifdef CONFIG_X86_64
static __always_inline bool __rdmsrq_constant(u32 msr, u64 *val, int type)
{
	BUILD_BUG_ON(!__builtin_constant_p(msr));

	asm_inline volatile goto(
		"1:\n"
		ALTERNATIVE("mov %[msr], %%ecx\n\t"
			    "2:\n"
			    RDMSR_AND_SAVE_RESULT,
			    ASM_RDMSR_IMM,
			    X86_FEATURE_MSR_IMM)
		_ASM_EXTABLE_TYPE(1b, %l[badmsr], %c[type])	/* For RDMSR immediate */
		_ASM_EXTABLE_TYPE(2b, %l[badmsr], %c[type])	/* For RDMSR */

		: [val] "=a" (*val)
		: [msr] "i" (msr), [type] "i" (type)
		: "ecx", "rdx"
		: badmsr);

	return false;

badmsr:
	*val = 0;

	return true;
}
#endif

static __always_inline bool __rdmsr(u32 msr, u64 *val, int type)
{
#ifdef CONFIG_X86_64
	if (__builtin_constant_p(msr))
		return __rdmsrq_constant(msr, val, type);
#endif

	return __rdmsrq_variable(msr, val, type);
}

static __always_inline bool __wrmsrq_variable(u32 msr, u64 val, int type)
{
#ifdef CONFIG_X86_64
	BUILD_BUG_ON(__builtin_constant_p(msr));
#endif

	/*
	 * WRMSR is 2 bytes.  WRMSRNS is 3 bytes.  Pad WRMSR with a redundant
	 * DS prefix to avoid a trailing NOP.
	 */
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
 *
 * Otherwise, it falls back to a serializing WRMSR.
 */
static __always_inline bool __wrmsrq_constant(u32 msr, u64 val, int type)
{
	BUILD_BUG_ON(!__builtin_constant_p(msr));

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

static __always_inline bool __wrmsrq(u32 msr, u64 val, int type)
{
#ifdef CONFIG_X86_64
	if (__builtin_constant_p(msr))
		return __wrmsrq_constant(msr, val, type);
#endif

	return __wrmsrq_variable(msr, val, type);
}

static __always_inline u64 native_rdmsrq(u32 msr)
{
	u64 val;

	__rdmsr(msr, &val, EX_TYPE_RDMSR);

	return val;
}

#define native_rdmsr(msr, val1, val2)			\
do {							\
	u64 __val = native_rdmsrq((msr));		\
	(void)((val1) = (u32)__val);			\
	(void)((val2) = (u32)(__val >> 32));		\
} while (0)

static __always_inline void native_wrmsrq(u32 msr, u64 val)
{
	__wrmsrq(msr, val, EX_TYPE_WRMSR);
}

static __always_inline void native_wrmsr(u32 msr, u32 low, u32 high)
{
	native_wrmsrq(msr, (u64)high << 32 | low);
}

static inline u64 native_read_msr(u32 msr)
{
	return native_rdmsrq(msr);
}

static inline int native_read_msr_safe(u32 msr, u64 *val)
{
	return __rdmsr(msr, val, EX_TYPE_RDMSR_SAFE) ? -EIO : 0;
}

/* Can be uninlined because referenced by paravirt */
static inline void notrace native_write_msr(u32 msr, u64 val)
{
	native_wrmsrq(msr, val);
}

/* Can be uninlined because referenced by paravirt */
static inline int notrace native_write_msr_safe(u32 msr, u64 val)
{
	return __wrmsrq(msr, val, EX_TYPE_WRMSR_SAFE) ? -EIO : 0;
}

extern int rdmsr_safe_regs(u32 regs[8]);
extern int wrmsr_safe_regs(u32 regs[8]);

static inline u64 native_read_pmc(int counter)
{
	EAX_EDX_DECLARE_ARGS(val, low, high);

	asm volatile("rdpmc" : EAX_EDX_RET(val, low, high) : "c" (counter));
	if (tracepoint_enabled(rdpmc))
		do_trace_rdpmc(counter, EAX_EDX_VAL(val, low, high), 0);
	return EAX_EDX_VAL(val, low, high);
}

#ifdef CONFIG_PARAVIRT_XXL
#include <asm/paravirt-msr.h>
#else
static __always_inline u64 read_msr(u32 msr)
{
	return native_read_msr(msr);
}

static __always_inline int read_msr_safe(u32 msr, u64 *p)
{
	return native_read_msr_safe(msr, p);
}

static __always_inline void write_msr(u32 msr, u64 val)
{
	native_write_msr(msr, val);
}

static __always_inline int write_msr_safe(u32 msr, u64 val)
{
	return native_write_msr_safe(msr, val);
}

static __always_inline u64 rdpmc(int counter)
{
	return native_read_pmc(counter);
}

#endif	/* !CONFIG_PARAVIRT_XXL */

/*
 * Access to machine-specific registers (available on 586 and better only)
 * Note: the rd* operations modify the parameters directly (without using
 * pointer indirection), this allows gcc to optimize better
 */

#define rdmsrq(msr, val)			\
do {						\
	(val) = read_msr(msr);			\
	if (tracepoint_enabled(read_msr))	\
		do_trace_read_msr(msr, val, 0);	\
} while (0)

#define rdmsr(msr, low, high)					\
do {								\
	u64 __val;						\
	rdmsrq(msr, __val);					\
	(void)((low) = (u32)__val);				\
	(void)((high) = (u32)(__val >> 32));			\
} while (0)

/* rdmsr with exception handling */
static inline int rdmsrq_safe(u32 msr, u64 *p)
{
	int err;

	err = read_msr_safe(msr, p);

	if (tracepoint_enabled(read_msr))
		do_trace_read_msr(msr, *p, err);

	return err;
}

#define rdmsr_safe(msr, low, high)				\
({								\
	u64 __val;						\
	int __err = rdmsrq_safe((msr), &__val);			\
	(*low) = (u32)__val;					\
	(*high) = (u32)(__val >> 32);				\
	__err;							\
})

static inline void wrmsrq(u32 msr, u64 val)
{
	write_msr(msr, val);

	if (tracepoint_enabled(write_msr))
		do_trace_write_msr(msr, val, 0);
}

/* wrmsr with exception handling */
static inline int wrmsrq_safe(u32 msr, u64 val)
{
	int err;

	err = write_msr_safe(msr, val);

	if (tracepoint_enabled(write_msr))
		do_trace_write_msr(msr, val, err);

	return err;
}

static inline void wrmsr(u32 msr, u32 low, u32 high)
{
	wrmsrq(msr, (u64)high << 32 | low);
}

/*
 * Dual u32 version of wrmsrq_safe():
 */
static inline int wrmsr_safe(u32 msr, u32 low, u32 high)
{
	return wrmsrq_safe(msr, (u64)high << 32 | low);
}

struct msr __percpu *msrs_alloc(void);
void msrs_free(struct msr __percpu *msrs);
int msr_set_bit(u32 msr, u8 bit);
int msr_clear_bit(u32 msr, u8 bit);

#ifdef CONFIG_SMP
int rdmsr_on_cpu(unsigned int cpu, u32 msr_no, u32 *l, u32 *h);
int wrmsr_on_cpu(unsigned int cpu, u32 msr_no, u32 l, u32 h);
int rdmsrq_on_cpu(unsigned int cpu, u32 msr_no, u64 *q);
int wrmsrq_on_cpu(unsigned int cpu, u32 msr_no, u64 q);
void rdmsr_on_cpus(const struct cpumask *mask, u32 msr_no, struct msr __percpu *msrs);
void wrmsr_on_cpus(const struct cpumask *mask, u32 msr_no, struct msr __percpu *msrs);
int rdmsr_safe_on_cpu(unsigned int cpu, u32 msr_no, u32 *l, u32 *h);
int wrmsr_safe_on_cpu(unsigned int cpu, u32 msr_no, u32 l, u32 h);
int rdmsrq_safe_on_cpu(unsigned int cpu, u32 msr_no, u64 *q);
int wrmsrq_safe_on_cpu(unsigned int cpu, u32 msr_no, u64 q);
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
static inline int rdmsrq_on_cpu(unsigned int cpu, u32 msr_no, u64 *q)
{
	rdmsrq(msr_no, *q);
	return 0;
}
static inline int wrmsrq_on_cpu(unsigned int cpu, u32 msr_no, u64 q)
{
	wrmsrq(msr_no, q);
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
	return wrmsr_safe(msr_no, l, h);
}
static inline int rdmsrq_safe_on_cpu(unsigned int cpu, u32 msr_no, u64 *q)
{
	return rdmsrq_safe(msr_no, q);
}
static inline int wrmsrq_safe_on_cpu(unsigned int cpu, u32 msr_no, u64 q)
{
	return wrmsrq_safe(msr_no, q);
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

/* Compatibility wrappers: */
#define rdmsrl(msr, val) rdmsrq(msr, val)
#define wrmsrl(msr, val) wrmsrq(msr, val)
#define rdmsrl_on_cpu(cpu, msr, q) rdmsrq_on_cpu(cpu, msr, q)

#endif /* __ASSEMBLER__ */
#endif /* _ASM_X86_MSR_H */
