/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 Qualcomm Technologies, Inc.
 */

#ifndef __RISCV_HW_BREAKPOINT_H
#define __RISCV_HW_BREAKPOINT_H

struct task_struct;

#ifdef CONFIG_HAVE_HW_BREAKPOINT

#include <uapi/linux/hw_breakpoint.h>

/* Maximum number of hardware breakpoints supported */
#define RISCV_HW_BP_NUM_MAX 32

#if __riscv_xlen == 64
#define cpu_to_le cpu_to_le64
#define le_to_cpu le64_to_cpu
#elif __riscv_xlen == 32
#define cpu_to_le cpu_to_le32
#define le_to_cpu le32_to_cpu
#else
#error "Unexpected __riscv_xlen"
#endif

#define RISCV_DBTR_BIT(_prefix, _name)		\
	RISCV_DBTR_##_prefix##_##_name##_BIT

#define RISCV_DBTR_BIT_MASK(_prefix, _name)		\
	RISCV_DBTR_##_prefix##_name##_BIT_MASK

#define RISCV_DBTR_BIT_MASK_VAL(_prefix, _name, _width)	\
	(((1UL << (_width)) - 1) << RISCV_DBTR_BIT(_prefix, _name))

#define CLEAR_DBTR_BIT(_target, _prefix, _bit_name)	\
	__clear_bit(RISCV_DBTR_BIT(_prefix, _bit_name), &(_target))

#define SET_DBTR_BIT(_target, _prefix, _bit_name)	\
	__set_bit(RISCV_DBTR_BIT(_prefix, _bit_name), &(_target))

#define RISCV_DBTR_EXEC		(0x1UL << 0)
#define RISCV_DBTR_LOAD		(0x1UL << 1)
#define RISCV_DBTR_STORE	(0x1UL << 2)
#define RISCV_DBTR_LDST		(RISCV_DBTR_LOAD | RISCV_DBTR_STORE)

enum {
	RISCV_DBTR_TRIG_NONE = 0,
	RISCV_DBTR_TRIG_LEGACY,
	RISCV_DBTR_TRIG_MCONTROL,
	RISCV_DBTR_TRIG_ICOUNT,
	RISCV_DBTR_TRIG_ITRIGGER,
	RISCV_DBTR_TRIG_ETRIGGER,
	RISCV_DBTR_TRIG_MCONTROL6,
};

/* Trigger Data 1 */
enum {
	RISCV_DBTR_BIT(TDATA1, DATA)   = 0,
#if __riscv_xlen == 64
	RISCV_DBTR_BIT(TDATA1, DMODE)  = 59,
	RISCV_DBTR_BIT(TDATA1, TYPE)   = 60,
#elif __riscv_xlen == 32
	RISCV_DBTR_BIT(TDATA1, DMODE)  = 27,
	RISCV_DBTR_BIT(TDATA1, TYPE)   = 28,
#else
	#error "Unknown __riscv_xlen"
#endif
};

enum {
#if __riscv_xlen == 64
	RISCV_DBTR_BIT_MASK(TDATA1, DATA) = RISCV_DBTR_BIT_MASK_VAL(TDATA1, DATA, 59),
#elif __riscv_xlen == 32
	RISCV_DBTR_BIT_MASK(TDATA1, DATA) = RISCV_DBTR_BIT_MASK_VAL(TDATA1, DATA,  27),
#else
	#error "Unknown __riscv_xlen"
#endif
	RISCV_DBTR_BIT_MASK(TDAT1, DMODE) = RISCV_DBTR_BIT_MASK_VAL(TDATA1, DMODE, 1),
	RISCV_DBTR_BIT_MASK(TDATA1, TYPE) = RISCV_DBTR_BIT_MASK_VAL(TDATA1, TYPE,  4),
};

/* MC - Match Control Type Register */
enum {
	RISCV_DBTR_BIT(MC, LOAD)     = 0,
	RISCV_DBTR_BIT(MC, STORE)    = 1,
	RISCV_DBTR_BIT(MC, EXEC)     = 2,
	RISCV_DBTR_BIT(MC, U)        = 3,
	RISCV_DBTR_BIT(MC, S)        = 4,
	RISCV_DBTR_BIT(MC, RES2)     = 5,
	RISCV_DBTR_BIT(MC, M)        = 6,
	RISCV_DBTR_BIT(MC, MATCH)    = 7,
	RISCV_DBTR_BIT(MC, CHAIN)    = 11,
	RISCV_DBTR_BIT(MC, ACTION)   = 12,
	RISCV_DBTR_BIT(MC, SIZELO)   = 16,
	RISCV_DBTR_BIT(MC, TIMING)   = 18,
	RISCV_DBTR_BIT(MC, SELECT)   = 19,
	RISCV_DBTR_BIT(MC, HIT)      = 20,
#if __riscv_xlen >= 64
	RISCV_DBTR_BIT(MC, SIZEHI) = 21,
#endif
#if __riscv_xlen == 64
	RISCV_DBTR_BIT(MC, MASKMAX) = 53,
	RISCV_DBTR_BIT(MC, DMODE)   = 59,
	RISCV_DBTR_BIT(MC, TYPE)    = 60,
#elif __riscv_xlen == 32
	RISCV_DBTR_BIT(MC, MASKMAX) = 21,
	RISCV_DBTR_BIT(MC, DMODE)   = 27,
	RISCV_DBTR_BIT(MC, TYPE)    = 28,
#else
	#error "Unknown riscv xlen"
#endif
};

enum {
	RISCV_DBTR_BIT_MASK(MC, LOAD) = RISCV_DBTR_BIT_MASK_VAL(MC, LOAD, 1),
	RISCV_DBTR_BIT_MASK(MC, STORE) = RISCV_DBTR_BIT_MASK_VAL(MC, STORE, 1),
	RISCV_DBTR_BIT_MASK(MC, EXEC) = RISCV_DBTR_BIT_MASK_VAL(MC, EXEC, 1),
	RISCV_DBTR_BIT_MASK(MC, U) = RISCV_DBTR_BIT_MASK_VAL(MC, U, 1),
	RISCV_DBTR_BIT_MASK(MC, S) = RISCV_DBTR_BIT_MASK_VAL(MC, S, 1),
	RISCV_DBTR_BIT_MASK(MC, RES2) = RISCV_DBTR_BIT_MASK_VAL(MC, RES2, 1),
	RISCV_DBTR_BIT_MASK(MC, M) = RISCV_DBTR_BIT_MASK_VAL(MC, M, 1),
	RISCV_DBTR_BIT_MASK(MC, MATCH) = RISCV_DBTR_BIT_MASK_VAL(MC, MATCH, 4),
	RISCV_DBTR_BIT_MASK(MC, CHAIN) = RISCV_DBTR_BIT_MASK_VAL(MC, CHAIN, 1),
	RISCV_DBTR_BIT_MASK(MC, ACTION) = RISCV_DBTR_BIT_MASK_VAL(MC, ACTION, 4),
	RISCV_DBTR_BIT_MASK(MC, SIZELO) = RISCV_DBTR_BIT_MASK_VAL(MC, SIZELO, 2),
	RISCV_DBTR_BIT_MASK(MC, TIMING) = RISCV_DBTR_BIT_MASK_VAL(MC, TIMING, 1),
	RISCV_DBTR_BIT_MASK(MC, SELECT) = RISCV_DBTR_BIT_MASK_VAL(MC, SELECT, 1),
	RISCV_DBTR_BIT_MASK(MC, HIT) = RISCV_DBTR_BIT_MASK_VAL(MC, HIT, 1),
#if __riscv_xlen >= 64
	RISCV_DBTR_BIT_MASK(MC, SIZEHI) = RISCV_DBTR_BIT_MASK_VAL(MC, SIZEHI,  2),
#endif
	RISCV_DBTR_BIT_MASK(MC, MASKMAX) = RISCV_DBTR_BIT_MASK_VAL(MC, MASKMAX, 6),
	RISCV_DBTR_BIT_MASK(MC, DMODE) = RISCV_DBTR_BIT_MASK_VAL(MC, DMODE, 1),
	RISCV_DBTR_BIT_MASK(MC, TYPE) = RISCV_DBTR_BIT_MASK_VAL(MC, TYPE, 4),
};

/* MC6 - Match Control 6 Type Register */
enum {
	RISCV_DBTR_BIT(MC6, LOAD)    = 0,
	RISCV_DBTR_BIT(MC6, STORE)   = 1,
	RISCV_DBTR_BIT(MC6, EXEC)    = 2,
	RISCV_DBTR_BIT(MC6, U)       = 3,
	RISCV_DBTR_BIT(MC6, S)       = 4,
	RISCV_DBTR_BIT(MC6, RES2)    = 5,
	RISCV_DBTR_BIT(MC6, M)       = 6,
	RISCV_DBTR_BIT(MC6, MATCH)   = 7,
	RISCV_DBTR_BIT(MC6, CHAIN)   = 11,
	RISCV_DBTR_BIT(MC6, ACTION)  = 12,
	RISCV_DBTR_BIT(MC6, SIZE)    = 16,
	RISCV_DBTR_BIT(MC6, TIMING)  = 20,
	RISCV_DBTR_BIT(MC6, SELECT)  = 21,
	RISCV_DBTR_BIT(MC6, HIT)     = 22,
	RISCV_DBTR_BIT(MC6, VU)      = 23,
	RISCV_DBTR_BIT(MC6, VS)      = 24,
#if __riscv_xlen == 64
	RISCV_DBTR_BIT(MC6, DMODE)   = 59,
	RISCV_DBTR_BIT(MC6, TYPE)    = 60,
#elif __riscv_xlen == 32
	RISCV_DBTR_BIT(MC6, DMODE)   = 27,
	RISCV_DBTR_BIT(MC6, TYPE)    = 28,
#else
	#error "Unknown riscv xlen"
#endif
};

enum {
	RISCV_DBTR_BIT_MASK(MC6, LOAD) = RISCV_DBTR_BIT_MASK_VAL(MC6, LOAD, 1),
	RISCV_DBTR_BIT_MASK(MC6, STORE) = RISCV_DBTR_BIT_MASK_VAL(MC6, STORE,  1),
	RISCV_DBTR_BIT_MASK(MC6, EXEC) = RISCV_DBTR_BIT_MASK_VAL(MC6, EXEC, 1),
	RISCV_DBTR_BIT_MASK(MC6, U) = RISCV_DBTR_BIT_MASK_VAL(MC6, U, 1),
	RISCV_DBTR_BIT_MASK(MC6, S) = RISCV_DBTR_BIT_MASK_VAL(MC6, S, 1),
	RISCV_DBTR_BIT_MASK(MC6, RES2) = RISCV_DBTR_BIT_MASK_VAL(MC6, RES2, 1),
	RISCV_DBTR_BIT_MASK(MC6, M) = RISCV_DBTR_BIT_MASK_VAL(MC6, M, 1),
	RISCV_DBTR_BIT_MASK(MC6, MATCH) = RISCV_DBTR_BIT_MASK_VAL(MC6, MATCH, 4),
	RISCV_DBTR_BIT_MASK(MC6, CHAIN) = RISCV_DBTR_BIT_MASK_VAL(MC6, CHAIN, 1),
	RISCV_DBTR_BIT_MASK(MC6, ACTION) = RISCV_DBTR_BIT_MASK_VAL(MC6, ACTION, 4),
	RISCV_DBTR_BIT_MASK(MC6, SIZE) = RISCV_DBTR_BIT_MASK_VAL(MC6, SIZE, 4),
	RISCV_DBTR_BIT_MASK(MC6, TIMING) = RISCV_DBTR_BIT_MASK_VAL(MC6, TIMING, 1),
	RISCV_DBTR_BIT_MASK(MC6, SELECT) = RISCV_DBTR_BIT_MASK_VAL(MC6, SELECT, 1),
	RISCV_DBTR_BIT_MASK(MC6, HIT) = RISCV_DBTR_BIT_MASK_VAL(MC6, HIT, 1),
	RISCV_DBTR_BIT_MASK(MC6, VU) = RISCV_DBTR_BIT_MASK_VAL(MC6, VU, 1),
	RISCV_DBTR_BIT_MASK(MC6, VS) = RISCV_DBTR_BIT_MASK_VAL(MC6, VS, 1),
#if __riscv_xlen == 64
	RISCV_DBTR_BIT_MASK(MC6, DMODE) = RISCV_DBTR_BIT_MASK_VAL(MC6, DMODE, 1),
	RISCV_DBTR_BIT_MASK(MC6, TYPE) = RISCV_DBTR_BIT_MASK_VAL(MC6, TYPE, 4),
#elif __riscv_xlen == 32
	RISCV_DBTR_BIT_MASK(MC6, DMODE) = RISCV_DBTR_BIT_MASK_VAL(MC6, DMODE, 1),
	RISCV_DBTR_BIT_MASK(MC6, TYPE) = RISCV_DBTR_BIT_MASK_VAL(MC6, TYPE, 4),
#else
	#error "Unknown riscv xlen"
#endif
};

#define RISCV_DBTR_SET_TDATA1_TYPE(_t1, _type)				\
	({								\
		typeof(_t1) (td1t1) = (_t1);				\
		(td1t1) &= ~RISCV_DBTR_BIT_MASK(TDATA1, TYPE);		\
		(td1t1) |= (((unsigned long)(_type)			\
			     << RISCV_DBTR_BIT(TDATA1, TYPE))		\
			    & RISCV_DBTR_BIT_MASK(TDATA1, TYPE));	\
		(td1t1);						\
	})

#define RISCV_DBTR_SET_MC_TYPE(_t1, _type)				\
	({								\
		typeof(_t1) (mct1) = (_t1);				\
		(mct1) &= ~RISCV_DBTR_BIT_MASK(MC, TYPE);		\
		(mct1) |= (((unsigned long)(_type)			\
			    << RISCV_DBTR_BIT(MC, TYPE))		\
			   & RISCV_DBTR_BIT_MASK(MC, TYPE));		\
		(mct1);							\
	})

#define RISCV_DBTR_SET_MC6_TYPE(_t1, _type)				\
	({								\
		typeof(_t1) (mc6t1) = (_t1);				\
		(mc6t1) &= ~RISCV_DBTR_BIT_MASK(MC6, TYPE);		\
		(mc6t1) |= (((unsigned long)(_type)			\
			     << RISCV_DBTR_BIT(MC6, TYPE))		\
			    & RISCV_DBTR_BIT_MASK(MC6, TYPE));		\
		(mc6t1);						\
	})

#define RISCV_DBTR_SET_MC_EXEC_BIT(_t1)		\
	SET_DBTR_BIT(_t1, MC, EXEC)

#define RISCV_DBTR_SET_MC_LOAD_BIT(_t1)		\
	SET_DBTR_BIT(_t1, MC, LOAD)

#define RISCV_DBTR_SET_MC_STORE_BIT(_t1)		\
	SET_DBTR_BIT(_t1, MC, STORE)

#define RISCV_DBTR_SET_MC_SIZELO(_t1, _val)				\
	({								\
		typeof(_t1) (mcslt1) = (_t1);				\
		mcslt1 &= ~RISCV_DBTR_BIT_MASK(MC, SIZELO);		\
		mcslt1 |= (((_val) << RISCV_DBTR_BIT(MC, SIZELO))	\
			   & RISCV_DBTR_BIT_MASK(MC, SIZELO));		\
		(mcslt1);						\
	})

#if __riscv_xlen >= 64
#define RISCV_DBTR_SET_MC_SIZEHI(_t1, _val)				\
	({								\
		typeof(_t1) (mcsht1) = (_t1);				\
		mcsht1 &= ~RISCV_DBTR_BIT_MASK(MC, SIZEHI);		\
		mcsht1 |= (((_val) << RISCV_DBTR_BIT(MC, SIZEHI))	\
			   & RISCV_DBTR_BIT_MASK(MC, SIZEHI));		\
		(mcsht1);						\
	})
#else
/* SIZEHI does not exist in the rv32 mcontrol layout; nothing to set. */
#define RISCV_DBTR_SET_MC_SIZEHI(_t1, _val) ((void)(_val), (_t1))
#endif

#define RISCV_DBTR_SET_MC6_EXEC_BIT(_t1)		\
	SET_DBTR_BIT(_t1, MC6, EXEC)

#define RISCV_DBTR_SET_MC6_LOAD_BIT(_t1)		\
	SET_DBTR_BIT(_t1, MC6, LOAD)

#define RISCV_DBTR_SET_MC6_STORE_BIT(_t1)		\
	SET_DBTR_BIT(_t1, MC6, STORE)

#define RISCV_DBTR_SET_MC6_SIZE(_t1, _val)				\
	({								\
		typeof(_t1) (mc6szt1) = (_t1);				\
		(mc6szt1) &= ~RISCV_DBTR_BIT_MASK(MC6, SIZE);		\
		(mc6szt1) |= (((_val) << RISCV_DBTR_BIT(MC6, SIZE))	\
			      & RISCV_DBTR_BIT_MASK(MC6, SIZE));	\
		(mc6szt1);						\
	})

struct arch_hw_breakpoint {
	unsigned long address;
	unsigned long len;
	unsigned int type;

	/* Trigger configuration data */
	unsigned long tdata1;
	unsigned long tdata2;
	unsigned long tdata3;
};

struct perf_event_attr;
struct notifier_block;
struct perf_event;
struct pt_regs;

int hw_breakpoint_slots(int type);
int arch_check_bp_in_kernelspace(struct arch_hw_breakpoint *hw);
int hw_breakpoint_arch_parse(struct perf_event *bp,
			     const struct perf_event_attr *attr,
			     struct arch_hw_breakpoint *hw);
int hw_breakpoint_exceptions_notify(struct notifier_block *unused,
				    unsigned long val, void *data);

void arch_enable_hw_breakpoint(struct perf_event *bp);
void arch_update_hw_breakpoint(struct perf_event *bp);
void arch_disable_hw_breakpoint(struct perf_event *bp);
int arch_install_hw_breakpoint(struct perf_event *bp);
void arch_uninstall_hw_breakpoint(struct perf_event *bp);
void hw_breakpoint_pmu_read(struct perf_event *bp);
void clear_ptrace_hw_breakpoint(struct task_struct *tsk);
void flush_ptrace_hw_breakpoint(struct task_struct *tsk);

#else

int hw_breakpoint_slots(int type)
{
	return 0;
}

static inline void clear_ptrace_hw_breakpoint(struct task_struct *tsk)
{
}

static inline void flush_ptrace_hw_breakpoint(struct task_struct *tsk)
{
}

void arch_enable_hw_breakpoint(struct perf_event *bp)
{
}

void arch_update_hw_breakpoint(struct perf_event *bp)
{
}

void arch_disable_hw_breakpoint(struct perf_event *bp)
{
}

#endif /* CONFIG_HAVE_HW_BREAKPOINT */
#endif /* __RISCV_HW_BREAKPOINT_H */
