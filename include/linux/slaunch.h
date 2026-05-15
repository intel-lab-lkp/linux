/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Main Secure Launch header file.
 *
 * Copyright (c) 2026 Apertus Solutions, LLC
 * Copyright (c) 2026, Oracle and/or its affiliates.
 */

#ifndef _LINUX_SLAUNCH_H
#define _LINUX_SLAUNCH_H

#include <asm/txt.h>

/*
 * Secure Launch Defined State Flags
 */
#define SL_FLAG_ACTIVE		0x00000001
#define SL_FLAG_ARCH_TXT	0x00000002

/*
 * Secure Launch CPU Type
 */
#define SL_CPU_INTEL	1

#define __SL32_CS	0x0008
#define __SL32_DS	0x0010
#define __SL64_CS	0x0018

/*
 * Secure Launch Defined Error Codes used in MLE-initiated TXT resets.
 *
 * Intel Trusted Execution Technology (TXT) Software Development Guide
 * Appendix I - ACM Error Codes
 */
#define SL_ERROR_GENERIC		0xc0008001
#define SL_ERROR_TPM_INIT		0xc0008002
#define SL_ERROR_TPM_INVALID_LOG20	0xc0008003
#define SL_ERROR_TPM_LOGGING_FAILED	0xc0008004
#define SL_ERROR_REGION_STRADDLE_4GB	0xc0008005
#define SL_ERROR_TPM_EXTEND		0xc0008006
#define SL_ERROR_MTRR_INV_VCNT		0xc0008007
#define SL_ERROR_MTRR_INV_DEF_TYPE	0xc0008008
#define SL_ERROR_MTRR_INV_BASE		0xc0008009
#define SL_ERROR_MTRR_INV_MASK		0xc000800a
#define SL_ERROR_MSR_INV_MISC_EN	0xc000800b
#define SL_ERROR_INV_AP_INTERRUPT	0xc000800c
#define SL_ERROR_INTEGER_OVERFLOW	0xc000800d
#define SL_ERROR_HEAP_WALK		0xc000800e
#define SL_ERROR_HEAP_MAP		0xc000800f
#define SL_ERROR_REGION_ABOVE_4GB	0xc0008010
#define SL_ERROR_HEAP_INVALID_DMAR	0xc0008011
#define SL_ERROR_HEAP_DMAR_SIZE		0xc0008012
#define SL_ERROR_HEAP_DMAR_MAP		0xc0008013
#define SL_ERROR_HI_PMR_BASE		0xc0008014
#define SL_ERROR_HI_PMR_SIZE		0xc0008015
#define SL_ERROR_LO_PMR_BASE		0xc0008016
#define SL_ERROR_LO_PMR_MLE		0xc0008017
#define SL_ERROR_INITRD_TOO_BIG		0xc0008018
#define SL_ERROR_HEAP_ZERO_OFFSET	0xc0008019
#define SL_ERROR_WAKE_BLOCK_TOO_SMALL	0xc000801a
#define SL_ERROR_MLE_BUFFER_OVERLAP	0xc000801b
#define SL_ERROR_BUFFER_BEYOND_PMR	0xc000801c
#define SL_ERROR_OS_SINIT_BAD_VERSION	0xc000801d
#define SL_ERROR_EVENTLOG_MAP		0xc000801e
#define SL_ERROR_TPM_INVALID_ALGS	0xc000801f
#define SL_ERROR_TPM_EVENT_COUNT	0xc0008020
#define SL_ERROR_TPM_INVALID_EVENT	0xc0008021
#define SL_ERROR_INVALID_SLRT		0xc0008022
#define SL_ERROR_SLRT_MISSING_ENTRY	0xc0008023
#define SL_ERROR_SLRT_MAP		0xc0008024

/*
 * Secure Launch Defined Limits
 */
#define SL_MAX_CPUS		512
#define SL_BOOT_STACK_SIZE	128

/*
 * Secure Launch event log entry type. The TXT specification defines the
 * base event value as 0x400 for DRTM values.
 *
 * Intel Trusted Execution Technology (TXT) Software Development Guide
 * Appendix F - TPM Event Log
 */
#define SL_EVTYPE_BASE			0x400
#define SL_EVTYPE_SECURE_LAUNCH		(SL_EVTYPE_BASE + 0x102)

/*
 * MLE scratch area offsets within TXT OS-MLE SL defined portion of the heap.
 */
#define SL_SCRATCH_AP_EBX		0
#define SL_SCRATCH_AP_JMP_OFFSET	4
#define SL_SCRATCH_AP_STACKS_OFFSET	8

#ifndef __ASSEMBLER__

#include <linux/io.h>
#include <linux/tpm_eventlog.h>

/*
 * Secure Launch AP stack and monitor block
 */
struct sl_ap_stack_and_monitor {
	u32 monitor;
	u32 cache_pad[15];
	u32 stack_pad[15];
	u32 apicid;
} __packed;

/*
 * Secure Launch AP wakeup information fetched in SMP boot code.
 */
struct sl_ap_wake_info {
	u32 ap_wake_block;
	u32 ap_wake_block_size;
	u32 ap_jmp_offset;
	u32 ap_stacks_offset;
};

/*
 * TXT Heap Map structure
 */
struct sl_txt_heap_info {
	u64 size;
	u64 offset;
};

/*
 * Secure Launch defined OS/MLE TXT Heap table
 *
 * This table is defined at the top level by the TXT specification
 * but the format of this structure is implementation specific.
 *
 * Intel Trusted Execution Technology (TXT) Software Development Guide
 * Appendix C - Intel TXT Heap Memory
 */
struct txt_os_mle_data {
	u32 version;
	u32 reserved;
	u64 slrt;
	u64 txt_info;
	u32 ap_wake_block;
	u32 ap_wake_block_size;
	u8 mle_scratch[64];
} __packed;

#ifdef CONFIG_SECURE_LAUNCH

/*
 * TPM event logging functions.
 */

/*
 * Log a TPM v1 formatted event to the given DRTM event log.
 */
static inline int tpm_log_event(void *evtlog_base, u32 evtlog_size,
				u32 event_size, void *event)
{
	struct tpm_event_log_header *evtlog =
		(struct tpm_event_log_header *)evtlog_base;

	if (memcmp(evtlog->signature, TPM_EVTLOG_SIGNATURE,
		   sizeof(TPM_EVTLOG_SIGNATURE)))
		return -EINVAL;

	if (evtlog->container_size > evtlog_size)
		return -EINVAL;

	if (evtlog->next_event_offset + event_size > evtlog->container_size)
		return -E2BIG;

	memcpy(evtlog_base + evtlog->next_event_offset, event, event_size);
	evtlog->next_event_offset += event_size;

	return 0;
}

/*
 * Log a TPM v2 formatted event to the given DRTM event log.
 */
static inline int tpm2_log_event(struct txt_heap_event_log_pointer2_1_element *elem,
				 void *evtlog_base, u32 evtlog_size,
				 u32 event_size, void *event)
{
	struct tcg_pcr_event *header =
		(struct tcg_pcr_event *)evtlog_base;

	/* Has to be at least big enough for the signature */
	if (header->event_size < sizeof(TCG_SPECID_SIG))
		return -EINVAL;

	if (memcmp((u8 *)header + sizeof(struct tcg_pcr_event),
		   TCG_SPECID_SIG, sizeof(TCG_SPECID_SIG)))
		return -EINVAL;

	if (elem->allocated_event_container_size > evtlog_size)
		return -EINVAL;

	if (elem->next_record_offset + event_size >
	    elem->allocated_event_container_size)
		return -E2BIG;

	memcpy(evtlog_base + elem->next_record_offset, event, event_size);
	elem->next_record_offset += event_size;

	return 0;
}

/*
 * External functions available in mainline kernel.
 */
void slaunch_setup(void);
void slaunch_fixup_ap_wake_vector(void);
u32 slaunch_get_flags(void);
struct sl_txt_heap_info *slaunch_txt_get_heap_map(void);
void *slaunch_txt_get_heap_table(void *heap, u8 index);
struct sl_ap_wake_info *slaunch_get_ap_wake_info(void);
struct acpi_table_header *slaunch_get_dmar_table(struct acpi_table_header *dmar);
void __noreturn slaunch_reset(void *ctx, const char *msg, u64 error);
void slaunch_finalize(int do_sexit);

static inline bool slaunch_is_txt_launch(void)
{
	u32 mask = SL_FLAG_ACTIVE | SL_FLAG_ARCH_TXT;

	return (slaunch_get_flags() & mask) == mask;
}

#else

static inline void slaunch_setup(void)
{
}

static inline void slaunch_fixup_ap_wake_vector(void)
{
}

static inline u32 slaunch_get_flags(void)
{
	return 0;
}

static inline struct acpi_table_header *slaunch_get_dmar_table(struct acpi_table_header *dmar)
{
	return dmar;
}

static inline void slaunch_finalize(int do_sexit)
{
}

static inline bool slaunch_is_txt_launch(void)
{
	return false;
}

#endif /* !CONFIG_SECURE_LAUNCH */

#endif /* !__ASSEMBLER__ */

#endif /* _LINUX_SLAUNCH_H */
