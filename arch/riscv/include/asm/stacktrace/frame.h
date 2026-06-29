/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ASM_RISCV_STACKTRACE_FRAME_H
#define __ASM_RISCV_STACKTRACE_FRAME_H

/*
 * See: arch/arm64/include/asm/stacktrace/frame.h for the reference
 * implementation.
 */

/*
 * - FRAME_META_TYPE_NONE
 *
 *   This value is reserved.
 *
 * - FRAME_META_TYPE_FINAL
 *
 *   The record is the last entry on the stack.
 *   Unwinding should terminate successfully.
 *
 * - FRAME_META_TYPE_PT_REGS
 *
 *   The record is embedded within a struct pt_regs, recording the registers at
 *   an arbitrary point in time.
 *   Unwinding should consume pt_regs::epc, followed by pt_regs::ra.
 *
 * Note: all other values are reserved and should result in unwinding
 * terminating with an error.
 */
#define FRAME_META_TYPE_NONE		0
#define FRAME_META_TYPE_FINAL		1
#define FRAME_META_TYPE_PT_REGS		2

#ifndef __ASSEMBLER__
/*
 * A standard RISC-V frame record.
 */
struct frame_record {
	unsigned long fp;
	unsigned long ra;
};

/*
 * A metadata frame record indicating a special unwind.
 * The record::{fp,ra} fields must be zero to indicate the presence of
 * metadata.
 */
struct frame_record_meta {
	struct frame_record record;
	unsigned long type;
};
#endif /* __ASSEMBLER__ */

#endif /* __ASM_RISCV_STACKTRACE_FRAME_H */
