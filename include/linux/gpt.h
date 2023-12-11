/* SPDX-License-Identifier: GPL-2.0-or-later */
/************************************************************
 * EFI GUID Partition Table
 * Per Intel EFI Specification v1.02
 * http://developer.intel.com/technology/efi/efi.htm
 *
 * By Matt Domsch <Matt_Domsch@dell.com>  Fri Sep 22 22:15:56 CDT 2000
 *   Copyright 2000,2001 Dell Inc.
 ************************************************************/

#ifndef _GPT_H
#define _GPT_H

#include <linux/types.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/major.h>
#include <linux/string.h>
#include <linux/efi.h>
#include <linux/compiler.h>

#define MSDOS_MBR_SIGNATURE 0xaa55
#define EFI_PMBR_OSTYPE_EFI 0xEF
#define EFI_PMBR_OSTYPE_EFI_GPT 0xEE

#define GPT_MBR_PROTECTIVE  1
#define GPT_MBR_HYBRID      2

#define GPT_HEADER_SIGNATURE 0x5452415020494645ULL
#define GPT_HEADER_REVISION_V1 0x00010000
#define GPT_PRIMARY_PARTITION_TABLE_LBA 1

#define PARTITION_SYSTEM_GUID \
	EFI_GUID(0xC12A7328, 0xF81F, 0x11d2, \
		 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B)
#define LEGACY_MBR_PARTITION_GUID \
	EFI_GUID(0x024DEE41, 0x33E7, 0x11d3, \
		 0x9D, 0x69, 0x00, 0x08, 0xC7, 0x81, 0xF3, 0x9F)
#define PARTITION_MSFT_RESERVED_GUID \
	EFI_GUID(0xE3C9E316, 0x0B5C, 0x4DB8, \
		 0x81, 0x7D, 0xF9, 0x2D, 0xF0, 0x02, 0x15, 0xAE)
#define PARTITION_BASIC_DATA_GUID \
	EFI_GUID(0xEBD0A0A2, 0xB9E5, 0x4433, \
		 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7)
#define PARTITION_LINUX_RAID_GUID \
	EFI_GUID(0xa19d880f, 0x05fc, 0x4d3b, \
		 0xa0, 0x06, 0x74, 0x3f, 0x0f, 0x84, 0x91, 0x1e)
#define PARTITION_LINUX_SWAP_GUID \
	EFI_GUID(0x0657fd6d, 0xa4ab, 0x43c4, \
		 0x84, 0xe5, 0x09, 0x33, 0xc8, 0x4b, 0x4f, 0x4f)
#define PARTITION_LINUX_LVM_GUID \
	EFI_GUID(0xe6d6d379, 0xf507, 0x44c2, \
		 0xa2, 0x3c, 0x23, 0x8f, 0x2a, 0x3d, 0xf9, 0x28)

typedef struct _gpt_header {
	__le64 signature;
	__le32 revision;
	__le32 header_size;
	__le32 header_crc32;
	__le32 reserved1;
	__le64 my_lba;
	__le64 alternate_lba;
	__le64 first_usable_lba;
	__le64 last_usable_lba;
	efi_guid_t disk_guid;
	__le64 partition_entry_lba;
	__le32 num_partition_entries;
	__le32 sizeof_partition_entry;
	__le32 partition_entry_array_crc32;

	/* The rest of the logical block is reserved by UEFI and must be zero.
	 * EFI standard handles this by:
	 *
	 * uint8_t		reserved2[ BlockSize - 92 ];
	 */
} __packed gpt_header;

typedef struct _gpt_entry_attributes {
	u64 required_to_function:1;
	u64 reserved:47;
	u64 type_guid_specific:16;
} __packed gpt_entry_attributes;

typedef struct _gpt_entry {
	efi_guid_t partition_type_guid;
	efi_guid_t unique_partition_guid;
	__le64 starting_lba;
	__le64 ending_lba;
	gpt_entry_attributes attributes;
	__le16 partition_name[72 / sizeof(__le16)];
} __packed gpt_entry;

typedef struct _gpt_mbr_record {
	u8	boot_indicator; /* unused by EFI, set to 0x80 for bootable */
	u8	start_head;     /* unused by EFI, pt start in CHS */
	u8	start_sector;   /* unused by EFI, pt start in CHS */
	u8	start_track;
	u8	os_type;        /* EFI and legacy non-EFI OS types */
	u8	end_head;       /* unused by EFI, pt end in CHS */
	u8	end_sector;     /* unused by EFI, pt end in CHS */
	u8	end_track;      /* unused by EFI, pt end in CHS */
	__le32	starting_lba;   /* used by EFI - start addr of the on disk pt */
	__le32	size_in_lba;    /* used by EFI - size of pt in LBA */
} __packed gpt_mbr_record;

typedef struct _legacy_mbr {
	u8 boot_code[440];
	__le32 unique_mbr_signature;
	__le16 unknown;
	gpt_mbr_record partition_record[4];
	__le16 signature;
} __packed legacy_mbr;

// Helpers for validating GPT metadata
int gpt_is_pmbr_valid(legacy_mbr *mbr, sector_t total_sectors);
int gpt_validate_header(gpt_header *gpt, u64 lba, unsigned int lba_size,
			u64 lastlba);
int gpt_check_pte_array_crc(gpt_header *gpt, gpt_entry *ptes);
int gpt_compare_alt(gpt_header *pgpt, gpt_header *agpt, u64 lastlba);

/**
 * is_pte_valid() - tests one PTE for validity
 * @pte:pte to check
 * @lastlba: last lba of the disk
 *
 * returns 1 if valid,  0 on error.
 */
	static inline bool
gpt_is_pte_valid(const gpt_entry *pte, const u64 lastlba)
{
	if ((!efi_guidcmp(pte->partition_type_guid, NULL_GUID)) ||
	    le64_to_cpu(pte->starting_lba) > lastlba         ||
	    le64_to_cpu(pte->ending_lba)   > lastlba)
		return 0;
	return 1;
}

// Returns size in bytes of PTE array
static inline int get_pt_size(gpt_header *gpt)
{
	return le32_to_cpu(gpt->num_partition_entries)
		* le32_to_cpu(gpt->sizeof_partition_entry);
}

void utf16_le_to_7bit(const __le16 *in, unsigned int size, u8 *out);

#endif
