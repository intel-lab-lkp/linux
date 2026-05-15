/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Intel Trusted eXecution Technology (TXT) Definitions
 *
 * Copyright (c) 2026 Apertus Solutions, LLC
 * Copyright (c) 2026, Oracle and/or its affiliates.
 */

#ifndef _ASM_X86_TXT_H
#define _ASM_X86_TXT_H

/*
 * Intel Safer Mode Extensions (SMX)
 *
 * Intel SMX provides a programming interface to establish a Measured Launched
 * Environment (MLE). The measurement and protection mechanisms are supported by the
 * capabilities of an Intel Trusted Execution Technology (TXT) platform. SMX is
 * the processor's programming interface in an Intel TXT platform.
 *
 * See:
 *   Intel SDM Volume 2 - 6.1 "Safer Mode Extensions Reference"
 *   Intel Trusted Execution Technology - Measured Launch Environment Developer's Guide
 */

/*
 * SMX GETSEC Leaf Functions
 */
#define SMX_X86_GETSEC_SEXIT	5
#define SMX_X86_GETSEC_SMCTRL	7
#define SMX_X86_GETSEC_WAKEUP	8

/*
 * Intel Trusted Execution Technology MMIO Registers Banks
 */
#define TXT_PUB_CONFIG_REGS_BASE	0xfed30000
#define TXT_PRIV_CONFIG_REGS_BASE	0xfed20000
#define TXT_NR_CONFIG_PAGES     ((TXT_PUB_CONFIG_REGS_BASE - \
				  TXT_PRIV_CONFIG_REGS_BASE) >> PAGE_SHIFT)

/*
 * Intel Trusted Execution Technology (TXT) Registers
 */
#define TXT_CR_STS			0x0000
#define TXT_CR_ESTS			0x0008
#define TXT_CR_ERRORCODE		0x0030
#define TXT_CR_CMD_RESET		0x0038
#define TXT_CR_CMD_CLOSE_PRIVATE	0x0048
#define TXT_CR_DIDVID			0x0110
#define TXT_CR_VER_EMIF			0x0200
#define TXT_CR_CMD_UNLOCK_MEM_CONFIG	0x0218
#define TXT_CR_SINIT_BASE		0x0270
#define TXT_CR_SINIT_SIZE		0x0278
#define TXT_CR_MLE_JOIN			0x0290
#define TXT_CR_HEAP_BASE		0x0300
#define TXT_CR_HEAP_SIZE		0x0308
#define TXT_CR_SCRATCHPAD		0x0378
#define TXT_CR_CMD_OPEN_LOCALITY1	0x0380
#define TXT_CR_CMD_CLOSE_LOCALITY1	0x0388
#define TXT_CR_CMD_OPEN_LOCALITY2	0x0390
#define TXT_CR_CMD_CLOSE_LOCALITY2	0x0398
#define TXT_CR_CMD_SECRETS		0x08e0
#define TXT_CR_CMD_NO_SECRETS		0x08e8
#define TXT_CR_E2STS			0x08f0

/* TXT default register value */
#define TXT_REGVALUE_ONE		0x1ULL

/* TXTCR_STS status bits */
#define TXT_SENTER_DONE_STS		BIT(0)
#define TXT_SEXIT_DONE_STS		BIT(1)

/*
 * SINIT/MLE Capabilities Field Bit Definitions
 */
#define TXT_SINIT_MLE_CAP_RLP_WAKE_GETSEC	0
#define TXT_SINIT_MLE_CAP_RLP_WAKE_MONITOR	1

/*
 * OS/MLE Secure Launch Specific Definitions
 */
#define TXT_OS_MLE_STRUCT_VERSION	1
#define TXT_OS_MLE_MAX_VARIABLE_MTRRS	32

#ifndef __ASSEMBLER__

/*
 * TXT Heap extended data elements.
 */
struct txt_heap_ext_data_element {
	u32 type;
	u32 size;
	/* Data */
} __packed;

#define TXT_HEAP_EXTDATA_TYPE_END			0

struct txt_heap_end_element {
	u32 type;
	u32 size;
} __packed;

#define TXT_HEAP_EXTDATA_TYPE_TPM_EVENT_LOG_PTR		5

struct txt_heap_event_log_element {
	u64 event_log_phys_addr;
} __packed;

#define TXT_HEAP_EXTDATA_TYPE_EVENT_LOG_POINTER2_1	8

struct txt_heap_event_log_pointer2_1_element {
	u64 phys_addr;
	u32 allocated_event_container_size;
	u32 first_record_offset;
	u32 next_record_offset;
} __packed;

/*
 * TXT specification defined BIOS data TXT Heap table
 */
struct txt_bios_data {
	u32 version; /* Currently 5 for TPM 1.2 and 6 for TPM 2.0 */
	u32 bios_sinit_size;
	u64 reserved1;
	u64 reserved2;
	u32 num_logical_procs;
	u32 sinit_flags;
	u32 mle_flags;
	/* Versions >= 5 with updates in version 6 */
	/* Ext Data Elements */
} __packed;

/*
 * TXT specification defined OS/SINIT TXT Heap table
 */
struct txt_os_sinit_data {
	u32 version; /* Currently 6 for TPM 1.2 and 7 for TPM 2.0 */
	u32 flags;
	u64 mle_ptab;
	u64 mle_size;
	u64 mle_hdr_base;
	u64 vtd_pmr_lo_base;
	u64 vtd_pmr_lo_size;
	u64 vtd_pmr_hi_base;
	u64 vtd_pmr_hi_size;
	u64 lcp_po_base;
	u64 lcp_po_size;
	u32 capabilities;
	/* Version = 5 */
	u64 efi_rsdt_ptr;
	/* Versions >= 6 */
	/* Ext Data Elements */
} __packed;

/*
 * TXT specification defined SINIT/MLE TXT Heap table
 */
struct txt_sinit_mle_data {
	u32 version;             /* Current values are 6 through 9 */
	/* Versions <= 8 */
	u8 bios_acm_id[20];
	u32 edx_senter_flags;
	u64 mseg_valid;
	u8 sinit_hash[20];
	u8 mle_hash[20];
	u8 stm_hash[20];
	u8 lcp_policy_hash[20];
	u32 lcp_policy_control;
	/* Versions >= 7 */
	u32 rlp_wakeup_addr;
	u32 reserved;
	u32 num_of_sinit_mdrs;
	u32 sinit_mdrs_table_offset;
	u32 sinit_vtd_dmar_table_size;
	u32 sinit_vtd_dmar_table_offset;
	/* Versions >= 8 */
	u32 processor_scrtm_status;
	/* Versions >= 9 */
	/* Ext Data Elements */
} __packed;

/*
 * TXT data reporting structure for memory types
 */
struct txt_sinit_memory_descriptor_record {
	u64 address;
	u64 length;
	u8 type;
	u8 reserved[7];
} __packed;

/*
 * TXT data structure used by a responsive local processor (RLP) to start
 * execution in response to a GETSEC[WAKEUP].
 */
struct smx_rlp_mle_join {
	u32 rlp_gdt_limit;
	u32 rlp_gdt_base;
	u32 rlp_seg_sel;     /* cs (ds, es, ss are seg_sel+8) */
	u32 rlp_entry_point; /* phys addr */
} __packed;

/*
 * TPM event log structures defined in both the TXT specification and
 * the TCG documentation.
 */
#define TPM_EVTLOG_SIGNATURE "TXT Event Container"

struct tpm_event_log_header {
	char signature[20];
	char reserved[12];
	u8 container_ver_major;
	u8 container_ver_minor;
	u8 pcr_event_ver_major;
	u8 pcr_event_ver_minor;
	u32 container_size;
	u32 pcr_events_offset;
	u32 next_event_offset;
	/* PCREvents[] */
} __packed;

/*
 * Functions to extract data from the Intel TXT Heap Memory. The layout
 * of the heap is as follows:
 *  +----------------------------+
 *  | Size Bios Data table (u64) |
 *  +----------------------------+
 *  | Bios Data table            |
 *  +----------------------------+
 *  | Size OS MLE table (u64)    |
 *  +----------------------------+
 *  | OS MLE table               |
 *  +--------------------------- +
 *  | Size OS SINIT table (u64)  |
 *  +----------------------------+
 *  | OS SINIT table             |
 *  +----------------------------+
 *  | Size SINIT MLE table (u64) |
 *  +----------------------------+
 *  | SINIT MLE table            |
 *  +----------------------------+
 *
 *  NOTE: the table size fields include the 8 byte size field itself.
 */
enum {
	TXT_BIOS_DATA_TABLE,
	TXT_OS_MLE_DATA_TABLE,
	TXT_OS_SINIT_DATA_TABLE,
	TXT_SINIT_MLE_DATA_TABLE,
	TXT_SINIT_TABLE_MAX,
};

/*
 * Find the TPM v2 event log element in the TXT heap. This element contains
 * the information about the size and location of the DRTM event log. Note
 * this is a TXT specific structure.
 *
 * See:
 *   Intel Trusted Execution Technology -
 *     Measured Launch Environment Developer's Guide - Appendix C.
 */
static inline struct txt_heap_event_log_pointer2_1_element*
txt_find_log2_1_element(struct txt_os_sinit_data *os_sinit_data)
{
#define ptr_after(p)     ((void *)p + sizeof(*p))
#define next_ext_elem(e) ((void *)e + e->size)
	/* The extended element array is at the end of this table */
	struct txt_heap_ext_data_element *ext_elem = ptr_after(os_sinit_data);

	while (ext_elem->type != TXT_HEAP_EXTDATA_TYPE_END) {
		if (ext_elem->type == TXT_HEAP_EXTDATA_TYPE_EVENT_LOG_POINTER2_1)
			return ptr_after(ext_elem);

		ext_elem = next_ext_elem(ext_elem);
	}

	return NULL;
}

#endif /* !__ASSEMBLER__ */

#endif /* _ASM_X86_TXT_H */
