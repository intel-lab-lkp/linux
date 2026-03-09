/* SPDX-License-Identifier: GPL-2.0 or MIT */
#ifndef _ASM_X86_VMWARE_H
#define _ASM_X86_VMWARE_H

#include <asm/cpufeatures.h>
#include <linux/stringify.h>
#include <linux/static_call.h>

/*
 * VMware hypercall ABI.
 *
 * - Low bandwidth (LB) hypercalls: I/O port based (aka backdoor), vmcall and
 * vmmcall have up to 6 input and 6 output on registers arguments, with the
 * register mapping:
 *  +------+----------------------------------------+-----------------+
 *  | Reg  | Input argument                         | Output argument |
 *  +======+========================================+=================+
 *  | %eax | VMWARE_HYPERVISOR_MAGIC                | out0            |
 *  +------+----------------------------------------+-----------------+
 *  | %ebx | (in1)                                  | out1            |
 *  +------+----------------------------------------+-----------------+
 *  | %ecx | (cmd) - Hypercall command              | out2            |
 *  +------+----------------------------------------+-----------------+
 *  | %edx | Bits [15:0] - Port number for backdoor | out3            |
 *  |      |               Zero for vmcall/vmmcall  |                 |
 *  |      | Bits [31:16] - (in3)                   |                 |
 *  +------+----------------------------------------+-----------------+
 *  | %esi | (in4)                                  | out4            |
 *  +------+----------------------------------------+-----------------+
 *  | %edi | (in5)                                  | out5            |
 *  +------+----------------------------------------+-----------------+
 *
 * - Low bandwidth TDX hypercalls (x86_64 only) are similar to LB hypercalls.
 * They also have up to 6 input and 6 output on registers arguments, with
 * different argument to register mapping:
 *  +------+----------------------------------------+-----------------+
 *  | Reg  | Input argument                         | Output argument |
 *  +======+========================================+=================+
 *  | %r12 | VMWARE_HYPERVISOR_MAGIC                | out0            |
 *  +------+----------------------------------------+-----------------+
 *  | %ebx | (in1)                                  | out1            |
 *  +------+----------------------------------------+-----------------+
 *  | %r13 | (cmd) - Hypercall command              | out2            |
 *  +------+----------------------------------------+-----------------+
 *  | %edx | Bits [15:0] - Must be zero             | out3            |
 *  |      | Bits [31:16] - (in3)                   |                 |
 *  +------+----------------------------------------+-----------------+
 *  | %esi | (in4)                                  | out4            |
 *  +------+----------------------------------------+-----------------+
 *  | %edi | (in5)                                  | out5            |
 *  +------+----------------------------------------+-----------------+
 *
 * - High bandwidth (HB) hypercalls are I/O port based only. They have up to 7
 * input and 7 output on reegister arguments with the following mapping:
 *  +------+----------------------------------------+-----------------+
 *  | Reg  | Input argument                         | Output argument |
 *  +======+========================================+=================+
 *  | %eax | VMWARE_HYPERVISOR_MAGIC                | out0            |
 *  +------+----------------------------------------+-----------------+
 *  | %ebx | (cmd) - Hypercall command              | out1            |
 *  +------+----------------------------------------+-----------------+
 *  | %ebx | (in2)                                  | out2            |
 *  +------+----------------------------------------+-----------------+
 *  | %edx | Bits [15:0] - Port number and HB flag  | out3            |
 *  |      | Bits [31:16] - (in3)                   |                 |
 *  +------+----------------------------------------+-----------------+
 *  | %esi | (in4)                                  | out4            |
 *  +------+----------------------------------------+-----------------+
 *  | %edi | (in5)                                  | out5            |
 *  +------+----------------------------------------+-----------------+
 *  | %ebp | (in6)                                  | out6            |
 *  +------+----------------------------------------+-----------------+
 *
 * For compatibility purposes, x86_64 systems use only lower 32 bits for input
 * and output arguments.
 *
 * The hypercall definitions differ in the low word of the %edx (arg3) in the
 * following way: the old I/O port based interface uses the port number, the
 * bandwidth mode flag, and uses IN/OUT instructions to define transfer
 * direction.
 *
 * The new vmcall interface instead uses a set of flags to select bandwidth
 * mode and transfer direction.
 */

#define VMWARE_HYPERVISOR_HB		BIT(0)
#define VMWARE_HYPERVISOR_OUT		BIT(1)

#define VMWARE_HYPERVISOR_PORT		0x5658
#define VMWARE_HYPERVISOR_PORT_HB	(VMWARE_HYPERVISOR_PORT | \
					 VMWARE_HYPERVISOR_HB)

#define VMWARE_HYPERVISOR_MAGIC		0x564d5868U

#define VMWARE_CMD_GETVERSION		10
#define VMWARE_CMD_MESSAGE		30
#define VMWARE_CMD_GETHZ		45
#define VMWARE_CMD_GETVCPU_INFO		68
#define VMWARE_CMD_STEALCLOCK		91
#define VMWARE_CMD_REPORTGUESTCRASH	102
/*
 * Hypercall command mask:
 *   bits [6:0] command, range [0, 127]
 *   bits [19:16] sub-command, range [0, 15]
 */
#define VMWARE_CMD_MASK			0xf007fU

#define CPUID_VMWARE_FEATURES_ECX_VMMCALL	BIT(0)
#define CPUID_VMWARE_FEATURES_ECX_VMCALL	BIT(1)

#define VMWARE_TDX_VENDOR_LEAF 0x1af7e4909ULL
#define VMWARE_TDX_HCALL_FUNC  1

unsigned long dummy_vmware_hypercall(unsigned long cmd,
				     unsigned long in1, unsigned long in3,
				     unsigned long in4, unsigned long in5,
				     u32 *out1, u32 *out2, u32 *out3,
				     u32 *out4, u32 *out5);

/*
 * Low bandwidth (LB) VMware hypercall.
 *
 * It is backed by the backdoor, vmcall, vmmcall or tdx call implementation.
 *
 * Use inX/outX arguments naming as the register mappings vary between
 * different implementations. See VMware hypercall ABI above.
 * These 10 arguments could be nicely wrapped in in/out structures, but it
 * will introduce unnecessary structs copy in vmware_tdx_hypercall().
 *
 * NOTE:
 * Do not merge vmware_{backdoor,vmcall,vmmcall}_hypercall implementations
 * using alternative instructions. Such patching mechanism can not be used
 * in vmware_hypercall path, as the first hypercall will be called much
 * before the apply_alternatives(). See vmware_platform_setup().
 */
DECLARE_STATIC_CALL(vmware_hypercall, dummy_vmware_hypercall);

/*
 * Set of commonly used vmware_hypercallX functions - wrappers on top of the
 * vmware_hypercall.
 */
static inline
unsigned long vmware_hypercall1(unsigned long cmd, unsigned long in1)
{
	u32 out1, out2, out3, out4, out5;

	return static_call_mod(vmware_hypercall)(cmd, in1, 0, 0, 0,
			       &out1, &out2, &out3, &out4, &out5);
}

static inline
unsigned long vmware_hypercall3(unsigned long cmd, unsigned long in1,
				u32 *out1, u32 *out2)
{
	u32 out3, out4, out5;

	return static_call_mod(vmware_hypercall)(cmd, in1, 0, 0, 0,
			       out1, out2, &out3, &out4, &out5);
}

static inline
unsigned long vmware_hypercall4(unsigned long cmd, unsigned long in1,
				u32 *out1, u32 *out2, u32 *out3)
{
	u32 out4, out5;

	return static_call_mod(vmware_hypercall)(cmd, in1, 0, 0, 0,
			       out1, out2, out3, &out4, &out5);
}

static inline
unsigned long vmware_hypercall5(unsigned long cmd, unsigned long in1,
				unsigned long in3, unsigned long in4,
				unsigned long in5, u32 *out2)
{
	u32 out1, out3, out4, out5;

	return static_call_mod(vmware_hypercall)(cmd, in1, in3, in4, in5,
			       &out1, out2, &out3, &out4, &out5);
}

static inline
unsigned long vmware_hypercall6(unsigned long cmd, unsigned long in1,
				unsigned long in3, u32 *out2,
				u32 *out3, u32 *out4, u32 *out5)
{
	u32 out1;

	return static_call_mod(vmware_hypercall)(cmd, in1, in3, 0, 0,
			       &out1, out2, out3, out4, out5);
}

static inline
unsigned long vmware_hypercall7(unsigned long cmd, unsigned long in1,
				unsigned long in3, unsigned long in4,
				unsigned long in5, u32 *out1,
				u32 *out2, u32 *out3)
{
	u32 out4, out5;

	return static_call_mod(vmware_hypercall)(cmd, in1, in3, in4, in5,
			       out1, out2, out3, &out4, &out5);
}

#ifdef CONFIG_X86_64
#define VMW_BP_CONSTRAINT "r"
#else
#define VMW_BP_CONSTRAINT "m"
#endif

/*
 * High bandwidth calls are not supported on encrypted memory guests.
 * The caller should check cc_platform_has(CC_ATTR_MEM_ENCRYPT) and use
 * low bandwidth hypercall if memory encryption is set.
 * This assumption simplifies HB hypercall implementation to just I/O port
 * based approach without alternative patching.
 */
static inline
unsigned long vmware_hypercall_hb_out(unsigned long cmd, unsigned long in2,
				      unsigned long in3, unsigned long in4,
				      unsigned long in5, unsigned long in6,
				      u32 *out1)
{
	unsigned long out0;

	asm_inline volatile (
		UNWIND_HINT_SAVE
		"push %%" _ASM_BP "\n\t"
		UNWIND_HINT_UNDEFINED
		"mov %[in6], %%" _ASM_BP "\n\t"
		"rep outsb\n\t"
		"pop %%" _ASM_BP "\n\t"
		UNWIND_HINT_RESTORE
		: "=a" (out0), "=b" (*out1)
		: "a" (VMWARE_HYPERVISOR_MAGIC),
		  "b" (cmd),
		  "c" (in2),
		  "d" (in3 | VMWARE_HYPERVISOR_PORT_HB),
		  "S" (in4),
		  "D" (in5),
		  [in6] VMW_BP_CONSTRAINT (in6)
		: "cc", "memory");
	return out0;
}

static inline
unsigned long vmware_hypercall_hb_in(unsigned long cmd, unsigned long in2,
				     unsigned long in3, unsigned long in4,
				     unsigned long in5, unsigned long in6,
				     u32 *out1)
{
	unsigned long out0;

	asm_inline volatile (
		UNWIND_HINT_SAVE
		"push %%" _ASM_BP "\n\t"
		UNWIND_HINT_UNDEFINED
		"mov %[in6], %%" _ASM_BP "\n\t"
		"rep insb\n\t"
		"pop %%" _ASM_BP "\n\t"
		UNWIND_HINT_RESTORE
		: "=a" (out0), "=b" (*out1)
		: "a" (VMWARE_HYPERVISOR_MAGIC),
		  "b" (cmd),
		  "c" (in2),
		  "d" (in3 | VMWARE_HYPERVISOR_PORT_HB),
		  "S" (in4),
		  "D" (in5),
		  [in6] VMW_BP_CONSTRAINT (in6)
		: "cc", "memory");
	return out0;
}
#undef VMW_BP_CONSTRAINT

#endif
