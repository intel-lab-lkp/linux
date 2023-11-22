/* SPDX-License-Identifier: GPL-2.0 or MIT */
#ifndef _ASM_X86_VMWARE_H
#define _ASM_X86_VMWARE_H

#include <asm/cpufeatures.h>
#include <asm/alternative.h>
#include <linux/stringify.h>

/*
 * The hypercall definitions differ in the low word of the %edx argument
 * in the following way: the old I/O port based interface uses the port
 * number to distinguish between high- and low bandwidth versions, and
 * uses IN/OUT instructions to define transfer direction.
 *
 * The new vmcall interface instead uses a set of flags to select
 * bandwidth mode and transfer direction. The flags should be loaded
 * into %dx by any user and are automatically replaced by the port
 * number if the I/O port method is used.
 *
 * In short, new driver code should strictly use the new definition of
 * %dx content.
 */

#define VMWARE_HYPERVISOR_HB		BIT(0)
#define VMWARE_HYPERVISOR_OUT		BIT(1)

#define VMWARE_HYPERVISOR_PORT		0x5658
#define VMWARE_HYPERVISOR_PORT_HB	(VMWARE_HYPERVISOR_PORT | \
					 VMWARE_HYPERVISOR_HB)

#define VMWARE_HYPERVISOR_MAGIC		0x564D5868U

#define VMWARE_CMD_GETVERSION		10
#define VMWARE_CMD_GETHZ		45
#define VMWARE_CMD_GETVCPU_INFO		68
#define VMWARE_CMD_STEALCLOCK		91

#define CPUID_VMWARE_FEATURES_ECX_VMMCALL	BIT(0)
#define CPUID_VMWARE_FEATURES_ECX_VMCALL	BIT(1)

extern u8 vmware_hypercall_mode;

/*
 * The low bandwidth call. The low word of edx is presumed to have OUT bit
 * set. The high word of edx may contain input data from the caller.
 */
#define VMWARE_HYPERCALL						\
	ALTERNATIVE_3("cmpb $"						\
			__stringify(CPUID_VMWARE_FEATURES_ECX_VMMCALL)	\
			", %[mode]\n\t"					\
		      "jg 2f\n\t"					\
		      "je 1f\n\t"					\
		      "movw %[port], %%dx\n\t"				\
		      "inl (%%dx), %%eax\n\t"				\
		      "jmp 3f\n\t"					\
		      "1: vmmcall\n\t"					\
		      "jmp 3f\n\t"					\
		      "2: vmcall\n\t"					\
		      "3:\n\t",						\
		      "movw %[port], %%dx\n\t"				\
		      "inl (%%dx), %%eax", X86_FEATURE_HYPERVISOR,	\
		      "vmcall", X86_FEATURE_VMCALL,			\
		      "vmmcall", X86_FEATURE_VMW_VMMCALL)

static inline
unsigned long vmware_hypercall1(unsigned long cmd, unsigned long in1)
{
	unsigned long out0;

	asm_inline volatile (VMWARE_HYPERCALL
		: "=a" (out0)
		: [port] "i" (VMWARE_HYPERVISOR_PORT),
		  [mode] "m" (vmware_hypercall_mode),
		  "a" (VMWARE_HYPERVISOR_MAGIC),
		  "b" (in1),
		  "c" (cmd),
		  "d" (0)
		: "cc", "memory");
	return out0;
}

static inline
unsigned long vmware_hypercall3(unsigned long cmd, unsigned long in1,
				uint32_t *out1, uint32_t *out2)
{
	unsigned long out0;

	asm_inline volatile (VMWARE_HYPERCALL
		: "=a" (out0), "=b" (*out1), "=c" (*out2)
		: [port] "i" (VMWARE_HYPERVISOR_PORT),
		  [mode] "m" (vmware_hypercall_mode),
		  "a" (VMWARE_HYPERVISOR_MAGIC),
		  "b" (in1),
		  "c" (cmd),
		  "d" (0)
		: "cc", "memory");
	return out0;
}

static inline
unsigned long vmware_hypercall4(unsigned long cmd, unsigned long in1,
				uint32_t *out1, uint32_t *out2,
				uint32_t *out3)
{
	unsigned long out0;

	asm_inline volatile (VMWARE_HYPERCALL
		: "=a" (out0), "=b" (*out1), "=c" (*out2), "=d" (*out3)
		: [port] "i" (VMWARE_HYPERVISOR_PORT),
		  [mode] "m" (vmware_hypercall_mode),
		  "a" (VMWARE_HYPERVISOR_MAGIC),
		  "b" (in1),
		  "c" (cmd),
		  "d" (0)
		: "cc", "memory");
	return out0;
}

static inline
unsigned long vmware_hypercall5(unsigned long cmd, unsigned long in1,
				unsigned long in3, unsigned long in4,
				unsigned long in5, uint32_t *out2)
{
	unsigned long out0;

	asm_inline volatile (VMWARE_HYPERCALL
		: "=a" (out0), "=c" (*out2)
		: [port] "i" (VMWARE_HYPERVISOR_PORT),
		  [mode] "m" (vmware_hypercall_mode),
		  "a" (VMWARE_HYPERVISOR_MAGIC),
		  "b" (in1),
		  "c" (cmd),
		  "d" (in3),
		  "S" (in4),
		  "D" (in5)
		: "cc", "memory");
	return out0;
}

static inline
unsigned long vmware_hypercall6(unsigned long cmd, unsigned long in1,
				unsigned long in3, uint32_t *out2,
				uint32_t *out3, uint32_t *out4,
				uint32_t *out5)
{
	unsigned long out0;

	asm_inline volatile (VMWARE_HYPERCALL
		: "=a" (out0), "=c" (*out2), "=d" (*out3), "=S" (*out4),
		  "=D" (*out5)
		: [port] "i" (VMWARE_HYPERVISOR_PORT),
		  [mode] "m" (vmware_hypercall_mode),
		  "a" (VMWARE_HYPERVISOR_MAGIC),
		  "b" (in1),
		  "c" (cmd),
		  "d" (in3)
		: "cc", "memory");
	return out0;
}

static inline
unsigned long vmware_hypercall7(unsigned long cmd, unsigned long in1,
				unsigned long in3, unsigned long in4,
				unsigned long in5, uint32_t *out1,
				uint32_t *out2, uint32_t *out3)
{
	unsigned long out0;

	asm_inline volatile (VMWARE_HYPERCALL
		: "=a" (out0), "=b" (*out1), "=c" (*out2), "=d" (*out3)
		: [port] "i" (VMWARE_HYPERVISOR_PORT),
		  [mode] "m" (vmware_hypercall_mode),
		  "a" (VMWARE_HYPERVISOR_MAGIC),
		  "b" (in1),
		  "c" (cmd),
		  "d" (in3),
		  "S" (in4),
		  "D" (in5)
		: "cc", "memory");
	return out0;
}


#ifdef CONFIG_X86_64
#define VMW_BP_REG "%%rbp"
#define VMW_BP_CONSTRAINT "r"
#else
#define VMW_BP_REG "%%ebp"
#define VMW_BP_CONSTRAINT "m"
#endif

/*
 * High bandwidth calls are not supported on encrypted memory guests.
 * The caller should check cc_platform_has(CC_ATTR_MEM_ENCRYPT) and use
 * low bandwidth hypercall it memory encryption is set.
 * This assumption simplifies HB hypercall impementation to just I/O port
 * based approach without alternative patching.
 */
static inline
unsigned long vmware_hypercall_hb_out(unsigned long cmd, unsigned long in2,
				      unsigned long in3, unsigned long in4,
				      unsigned long in5, unsigned long in6,
				      uint32_t *out1)
{
	unsigned long out0;

	asm_inline volatile (
		UNWIND_HINT_SAVE
		"push " VMW_BP_REG "\n\t"
		UNWIND_HINT_UNDEFINED
		"mov %[in6], " VMW_BP_REG "\n\t"
		"rep outsb\n\t"
		"pop " VMW_BP_REG "\n\t"
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
				     uint32_t *out1)
{
	unsigned long out0;

	asm_inline volatile (
		UNWIND_HINT_SAVE
		"push " VMW_BP_REG "\n\t"
		UNWIND_HINT_UNDEFINED
		"mov %[in6], " VMW_BP_REG "\n\t"
		"rep insb\n\t"
		"pop " VMW_BP_REG "\n\t"
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
#undef VMW_BP_REG
#undef VMW_BP_CONSTRAINT
#undef VMWARE_HYPERCALL

#endif
