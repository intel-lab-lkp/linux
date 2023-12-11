// SPDX-License-Identifier: GPL-2.0-or-later
/* EFI GUID Partition Table handling
 *
 * http://www.uefi.org/specs/
 * http://www.intel.com/technology/efi/
 *
 * efi.[ch] by Matt Domsch <Matt_Domsch@dell.com>
 *   Copyright 2000,2001,2002,2004 Dell Inc.
 *
 * This code was previously in block/partitions/efi.c
 * and was moved in /lib so that other kernel subsystems
 * could use it as a common GPT parsing library.
 *
 * This library should be stateless and not make any
 * assumptions about the type of device the GPT data
 * came from.
 *
 */

#include <linux/gpt.h>
#include <linux/efi.h>

static inline int pmbr_part_valid(gpt_mbr_record *part)
{
	if (part->os_type != EFI_PMBR_OSTYPE_EFI_GPT)
		goto invalid;

	/* set to 0x00000001 (i.e., the LBA of the GPT Partition Header) */
	if (le32_to_cpu(part->starting_lba) != GPT_PRIMARY_PARTITION_TABLE_LBA)
		goto invalid;

	return GPT_MBR_PROTECTIVE;
invalid:
	return 0;
}

/**
 * gpt_is_pmbr_valid(): test Protective MBR for validity
 * @mbr: pointer to a legacy mbr structure
 * @total_sectors: amount of sectors in the device
 *
 * Description: Checks for a valid protective or hybrid
 * master boot record (MBR). The validity of a pMBR depends
 * on all of the following properties:
 *  1) MSDOS signature is in the last two bytes of the MBR
 *  2) One partition of type 0xEE is found
 *
 * In addition, a hybrid MBR will have up to three additional
 * primary partitions, which point to the same space that's
 * marked out by up to three GPT partitions.
 *
 * Returns 0 upon invalid MBR, or GPT_MBR_PROTECTIVE or
 * GPT_MBR_HYBRID depending on the device layout.
 */
int gpt_is_pmbr_valid(legacy_mbr *mbr, sector_t total_sectors)
{
	int i, part = 0, ret = 0; /* invalid by default */
	uint32_t sz = 0;

	if (!mbr || le16_to_cpu(mbr->signature) != MSDOS_MBR_SIGNATURE)
		goto done;

	for (i = 0; i < 4; i++) {
		ret = pmbr_part_valid(&mbr->partition_record[i]);
		if (ret == GPT_MBR_PROTECTIVE) {
			part = i;
			/*
			 * Ok, we at least know that there's a protective MBR,
			 * now check if there are other partition types for
			 * hybrid MBR.
			 */
			goto check_hybrid;
		}
	}

	if (ret != GPT_MBR_PROTECTIVE)
		goto done;
check_hybrid:
	for (i = 0; i < 4; i++)
		if (mbr->partition_record[i].os_type != EFI_PMBR_OSTYPE_EFI_GPT &&
		    mbr->partition_record[i].os_type != 0x00)
			ret = GPT_MBR_HYBRID;

	/*
	 * Protective MBRs take up the lesser of the whole disk
	 * or 2 TiB (32bit LBA), ignoring the rest of the disk.
	 * Some partitioning programs, nonetheless, choose to set
	 * the size to the maximum 32-bit limitation, disregarding
	 * the disk size.
	 *
	 * Hybrid MBRs do not necessarily comply with this.
	 *
	 * Consider a bad value here to be a warning to support dd'ing
	 * an image from a smaller disk to a larger disk.
	 */
	if (ret == GPT_MBR_PROTECTIVE) {
		sz = le32_to_cpu(mbr->partition_record[part].size_in_lba);
		if (sz != (uint32_t)total_sectors - 1 && sz != 0xFFFFFFFF)
			pr_debug("GPT: mbr size in lba (%u) different than whole disk (%u).\n",
				 sz, min_t(uint32_t,
					   total_sectors - 1, 0xFFFFFFFF));
	}
done:
	return ret;
}
EXPORT_SYMBOL_GPL(gpt_is_pmbr_valid);

/**
 * gpt_validate_header() - tests one GPT header for validity
 * @gpt:      header to check
 * @lba:      logical block address of the GPT header to test
 * @lba_size: logical block size of the partitioned device
 * @lastlba:  last logical block on the partitioned device
 *
 * Returns 0 if validation was successful.
 */
int gpt_validate_header(gpt_header *gpt, u64 lba, unsigned int lba_size,
			u64 lastlba)
{
	u32 crc, origcrc;
	u64 pt_size;

	/* Check the GUID Partition Table signature */
	if (le64_to_cpu(gpt->signature) != GPT_HEADER_SIGNATURE) {
		pr_debug("GUID Partition Table Header signature is wrong: %lld != %lld\n",
			 (unsigned long long)le64_to_cpu(gpt->signature),
			 (unsigned long long)GPT_HEADER_SIGNATURE);
		return -EINVAL;
	}

	/* Check the GUID Partition Table header size is too big */
	if (le32_to_cpu(gpt->header_size) > lba_size) {
		pr_debug("GUID Partition Table Header size is too large: %u > %u\n",
			 le32_to_cpu(gpt->header_size), lba_size);
		return -EINVAL;
	}

	/* Check the GUID Partition Table header size is too small */
	if (le32_to_cpu(gpt->header_size) < sizeof(gpt_header)) {
		pr_debug("GUID Partition Table Header size is too small: %u < %zu\n",
			 le32_to_cpu(gpt->header_size),
			 sizeof(gpt_header));
		return -EINVAL;
	}

	/* Check the GUID Partition Table CRC */
	origcrc = le32_to_cpu(gpt->header_crc32);
	gpt->header_crc32 = 0;
	crc = efi_crc32((const unsigned char *)gpt, le32_to_cpu(gpt->header_size));

	if (crc != origcrc) {
		pr_debug("GUID Partition Table Header CRC is wrong: %x != %x\n",
			 crc, origcrc);
		return -EINVAL;
	}
	gpt->header_crc32 = cpu_to_le32(origcrc);

	/* Check that the my_lba entry points to the LBA that contains
	 * the GUID Partition Table
	 */
	if (le64_to_cpu(gpt->my_lba) != lba) {
		pr_debug("GPT my_lba incorrect: %lld != %lld\n",
			 (unsigned long long)le64_to_cpu(gpt->my_lba),
			 (unsigned long long)lba);
		return -EINVAL;
	}

	/* Check the first_usable_lba and last_usable_lba are
	 * within the disk.
	 */
	if (le64_to_cpu(gpt->first_usable_lba) > lastlba) {
		pr_debug("GPT: first_usable_lba incorrect: %lld > %lld\n",
			 (unsigned long long)le64_to_cpu(gpt->first_usable_lba),
			 (unsigned long long)lastlba);
		return -EINVAL;
	}
	if (le64_to_cpu(gpt->last_usable_lba) > lastlba) {
		pr_debug("GPT: last_usable_lba incorrect: %lld > %lld\n",
			 (unsigned long long)le64_to_cpu(gpt->last_usable_lba),
			 (unsigned long long)lastlba);
		return -EINVAL;
	}
	if (le64_to_cpu(gpt->last_usable_lba) < le64_to_cpu(gpt->first_usable_lba)) {
		pr_debug("GPT: last_usable_lba incorrect: %lld > %lld\n",
			 (unsigned long long)le64_to_cpu(gpt->last_usable_lba),
			 (unsigned long long)le64_to_cpu(gpt->first_usable_lba));
		return -EINVAL;
	}

	/* Check that sizeof_partition_entry has the correct value */
	if (le32_to_cpu(gpt->sizeof_partition_entry) != sizeof(gpt_entry)) {
		pr_debug("GUID Partition Entry Size check failed.\n");
		return -EINVAL;
	}

	/* Sanity check partition table size */
	pt_size = (u64)get_pt_size(gpt);
	if (pt_size > KMALLOC_MAX_SIZE) {
		pr_debug("GUID Partition Table is too large: %llu > %lu bytes\n",
			 (unsigned long long)pt_size, KMALLOC_MAX_SIZE);
		return -EINVAL;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(gpt_validate_header);

/* Check the GUID Partition Entry Array CRC */
int gpt_check_pte_array_crc(gpt_header *gpt, gpt_entry *ptes)
{
	u32 crc;

	crc = efi_crc32((const unsigned char *)ptes, get_pt_size(gpt));
	if (crc != le32_to_cpu(gpt->partition_entry_array_crc32)) {
		pr_debug("GUID Partition Entry Array CRC check failed.\n");
		return -EINVAL;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(gpt_check_pte_array_crc);

/**
 * gpt_compare_alt() - Compares the Primary and Alternate GPT headers
 * @pgpt: primary GPT header
 * @agpt: alternate GPT header
 * @lastlba: last LBA number
 *
 * Description: Sanity checks pgpt and agpt fields and prints warnings
 * on discrepancies. Returns error count. GPT parsers can choose to
 * ignore this or not.
 *
 */
int gpt_compare_alt(gpt_header *pgpt, gpt_header *agpt, u64 lastlba)
{
	int error_found = 0;

	if (!pgpt || !agpt)
		return -EINVAL;

	if (le64_to_cpu(pgpt->my_lba) != le64_to_cpu(agpt->alternate_lba)) {
		pr_warn("GPT:Primary header LBA != Alt. header alternate_lba\n");
		pr_warn("GPT:%lld != %lld\n",
			(unsigned long long)le64_to_cpu(pgpt->my_lba),
			(unsigned long long)le64_to_cpu(agpt->alternate_lba));
		error_found++;
	}
	if (le64_to_cpu(pgpt->alternate_lba) != le64_to_cpu(agpt->my_lba)) {
		pr_warn("GPT:Primary header alternate_lba != Alt. header my_lba\n");
		pr_warn("GPT:%lld != %lld\n",
			(unsigned long long)le64_to_cpu(pgpt->alternate_lba),
			(unsigned long long)le64_to_cpu(agpt->my_lba));
		error_found++;
	}
	if (le64_to_cpu(pgpt->first_usable_lba) !=
	    le64_to_cpu(agpt->first_usable_lba)) {
		pr_warn("GPT:first_usable_lbas don't match.\n");
		pr_warn("GPT:%lld != %lld\n",
			(unsigned long long)le64_to_cpu(pgpt->first_usable_lba),
			(unsigned long long)le64_to_cpu(agpt->first_usable_lba));
		error_found++;
	}
	if (le64_to_cpu(pgpt->last_usable_lba) !=
	    le64_to_cpu(agpt->last_usable_lba)) {
		pr_warn("GPT:last_usable_lbas don't match.\n");
		pr_warn("GPT:%lld != %lld\n",
			(unsigned long long)le64_to_cpu(pgpt->last_usable_lba),
			(unsigned long long)le64_to_cpu(agpt->last_usable_lba));
		error_found++;
	}
	if (efi_guidcmp(pgpt->disk_guid, agpt->disk_guid)) {
		pr_warn("GPT:disk_guids don't match.\n");
		error_found++;
	}
	if (le32_to_cpu(pgpt->num_partition_entries) !=
	    le32_to_cpu(agpt->num_partition_entries)) {
		pr_warn("GPT:num_partition_entries don't match: 0x%x != 0x%x\n",
			le32_to_cpu(pgpt->num_partition_entries),
			le32_to_cpu(agpt->num_partition_entries));
		error_found++;
	}
	if (le32_to_cpu(pgpt->sizeof_partition_entry) !=
	    le32_to_cpu(agpt->sizeof_partition_entry)) {
		pr_warn("GPT:sizeof_partition_entry values don't match: 0x%x != 0x%x\n",
			le32_to_cpu(pgpt->sizeof_partition_entry),
			le32_to_cpu(agpt->sizeof_partition_entry));
		error_found++;
	}
	if (le32_to_cpu(pgpt->partition_entry_array_crc32) !=
	    le32_to_cpu(agpt->partition_entry_array_crc32)) {
		pr_warn("GPT:partition_entry_array_crc32 values don't match: 0x%x != 0x%x\n",
			le32_to_cpu(pgpt->partition_entry_array_crc32),
			le32_to_cpu(agpt->partition_entry_array_crc32));
		error_found++;
	}
	if (le64_to_cpu(pgpt->alternate_lba) != lastlba) {
		pr_warn("GPT:Primary header thinks Alt. header is not at the end of the disk.\n");
		pr_warn("GPT:%lld != %lld\n",
			(unsigned long long)le64_to_cpu(pgpt->alternate_lba),
			(unsigned long long)lastlba);
		error_found++;
	}

	if (le64_to_cpu(agpt->my_lba) != lastlba) {
		pr_warn("GPT:Alternate GPT header not at the end of the disk.\n");
		pr_warn("GPT:%lld != %lld\n",
			(unsigned long long)le64_to_cpu(agpt->my_lba),
			(unsigned long long)lastlba);
		error_found++;
	}

	if (error_found)
		pr_warn("GPT: Use GNU Parted to correct GPT errors.\n");
	return error_found;
}
EXPORT_SYMBOL_GPL(gpt_compare_alt);

/**
 * utf16_le_to_7bit(): Naively converts a UTF-16LE string to 7-bit ASCII characters
 * @in: input UTF-16LE string
 * @size: size of the input string
 * @out: output string ptr, should be capable to store @size+1 characters
 *
 * Description: Converts @size UTF16-LE symbols from @in string to 7-bit
 * ASCII characters and stores them to @out. Adds trailing zero to @out array.
 */
void utf16_le_to_7bit(const __le16 *in, unsigned int size, u8 *out)
{
	unsigned int i = 0;

	out[size] = 0;

	while (i < size) {
		u8 c = le16_to_cpu(in[i]) & 0xff;

		if (c && !isprint(c))
			c = '!';
		out[i] = c;
		i++;
	}
}
EXPORT_SYMBOL_GPL(utf16_le_to_7bit);
