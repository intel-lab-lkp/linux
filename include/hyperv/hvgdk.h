/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Type definitions for the Microsoft Hypervisor.
 */
#ifndef _HV_HVGDK_H
#define _HV_HVGDK_H

#include "hvgdk_mini.h"
#include "hvgdk_ext.h"

/*
 * The guest OS needs to register the guest ID with the hypervisor.
 * The guest ID is a 64 bit entity and the structure of this ID is
 * specified in the Hyper-V TLFS specification.
 *
 * While the current guideline does not specify how Linux guest ID(s)
 * need to be generated, our plan is to publish the guidelines for
 * Linux and other guest operating systems that currently are hosted
 * on Hyper-V. The implementation here conforms to this yet
 * unpublished guidelines.
 *
 * Bit(s)
 * 63 - Indicates if the OS is Open Source or not; 1 is Open Source
 * 62:56 - Os Type; Linux is 0x100
 * 55:48 - Distro specific identification
 * 47:16 - Linux kernel version number
 * 15:0  - Distro specific identification
 */

#define HV_LINUX_VENDOR_ID              0x8100

/*
 * Hyper-V uses the software reserved 32 bytes in VMCB control area to expose
 * SVM enlightenments to guests. This is documented in the TLFS doc.
 * HV_VMX_ENLIGHTENED_VMCS or SVM_NESTED_ENLIGHTENED_VMCB_FIELDS
 */
struct hv_vmcb_enlightenments {
	struct __packed hv_enlightenments_control {
		u32 nested_flush_hypercall : 1;
		u32 msr_bitmap : 1;
		u32 enlightened_npt_tlb: 1;
		u32 reserved : 29;
	} __packed hv_enlightenments_control;
	u32 hv_vp_id;
	u64 hv_vm_id;
	u64 partition_assist_page;
	u64 reserved;
} __packed;

/* Define connection identifier type. */
union hv_connection_id {
	u32 asu32;
	struct {
		u32 id : 24;
		u32 reserved : 8;
	} __packed u;
};

struct hv_input_unmap_gpa_pages {
	u64 target_partition_id;
	u64 target_gpa_base;
	u32 unmap_flags;
	u32 padding;
} __packed;

#endif /* #ifndef _HV_HVGDK_H */
