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

/* The low bandwidth call. The low word of edx is presumed clear. */
#define VMWARE_HYPERCALL						\
	ALTERNATIVE_2("movw $" __stringify(VMWARE_HYPERVISOR_PORT) ", %%dx; " \
		      "inl (%%dx), %%eax",				\
		      "vmcall", X86_FEATURE_VMCALL,			\
		      "vmmcall", X86_FEATURE_VMW_VMMCALL)

/*
 * The high bandwidth out call. The low word of edx is presumed to have the
 * HB and OUT bits set.
 */
#define VMWARE_HYPERCALL_HB_OUT						\
	ALTERNATIVE_2("movw $" __stringify(VMWARE_HYPERVISOR_PORT_HB) ", %%dx; " \
		      "rep outsb",					\
		      "vmcall", X86_FEATURE_VMCALL,			\
		      "vmmcall", X86_FEATURE_VMW_VMMCALL)

/*
 * The high bandwidth in call. The low word of edx is presumed to have the
 * HB bit set.
 */
#define VMWARE_HYPERCALL_HB_IN						\
	ALTERNATIVE_2("movw $" __stringify(VMWARE_HYPERVISOR_PORT_HB) ", %%dx; " \
		      "rep insb",					\
		      "vmcall", X86_FEATURE_VMCALL,			\
		      "vmmcall", X86_FEATURE_VMW_VMMCALL)

#define VMWARE_PORT(cmd, eax, ebx, ecx, edx)				\
	__asm__("inl (%%dx), %%eax" :					\
		"=a"(eax), "=c"(ecx), "=d"(edx), "=b"(ebx) :		\
		"a"(VMWARE_HYPERVISOR_MAGIC),				\
		"c"(VMWARE_CMD_##cmd),					\
		"d"(VMWARE_HYPERVISOR_PORT), "b"(UINT_MAX) :		\
		"memory")

#define VMWARE_VMCALL(cmd, eax, ebx, ecx, edx)				\
	__asm__("vmcall" :						\
		"=a"(eax), "=c"(ecx), "=d"(edx), "=b"(ebx) :		\
		"a"(VMWARE_HYPERVISOR_MAGIC),				\
		"c"(VMWARE_CMD_##cmd),					\
		"d"(0), "b"(UINT_MAX) :					\
		"memory")

#define VMWARE_VMMCALL(cmd, eax, ebx, ecx, edx)				\
	__asm__("vmmcall" :						\
		"=a"(eax), "=c"(ecx), "=d"(edx), "=b"(ebx) :		\
		"a"(VMWARE_HYPERVISOR_MAGIC),				\
		"c"(VMWARE_CMD_##cmd),					\
		"d"(0), "b"(UINT_MAX) :					\
		"memory")

#define VMWARE_CMD(cmd, eax, ebx, ecx, edx) do {		\
	switch (vmware_hypercall_mode) {			\
	case CPUID_VMWARE_FEATURES_ECX_VMCALL:			\
		VMWARE_VMCALL(cmd, eax, ebx, ecx, edx);		\
		break;						\
	case CPUID_VMWARE_FEATURES_ECX_VMMCALL:			\
		VMWARE_VMMCALL(cmd, eax, ebx, ecx, edx);	\
		break;						\
	default:						\
		VMWARE_PORT(cmd, eax, ebx, ecx, edx);		\
		break;						\
	}							\
	} while (0)

#endif
