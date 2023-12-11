// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MTD parser for GPT partition tables.
 *
 * This parser supports GPT partition tables located at fixed standard sectors.
 * Partitioning a raw flash device in this manner prevents wear-leveling and bad
 * block handling at the whole-device level. Note that bad blocks on critical
 * GPT sectors will completely break your partition table! Because of this, use
 * of this parser is restricted to NOR flash devices, which are less susceptible
 * to bad blocks than NAND flash devices.
 *
 * http://www.uefi.org/specs/
 *
 * Acronyms:
 * PTE: Partition Table Entry
 * LBA: Logical Block Address
 *
 * Copyright © 2023 Bootlin
 *
 * Author: Romain Gantois <romain.gantois@bootlin.com>
 *
 */

#include <linux/mtd/partitions.h>
#include <linux/mtd/mtd.h>
#include <linux/minmax.h>
#include <linux/kernel.h>
#include <linux/gpt.h>

/*
 * We assume that the GPT partition was written through an mtdblock device. This
 * would make the LBA size 512.
 */
#define MTD_GPT_LBA_SIZE 512

/*
 * This value is pretty much arbitrary, it's in the range of typical MTD parser
 * caps. Creating too many partitions on a raw Flash device is a bad idea
 * anyway.
 */
#define MTD_GPT_MAX_PARTS 32

#define mtd_gpt_lba_to_offset(x) ((x) * MTD_GPT_LBA_SIZE)
#define mtd_gpt_lba_to_size(x)   ((size_t)(x) * MTD_GPT_LBA_SIZE)

#define MTD_GPT_PARTNAME_SIZE sizeof(((gpt_entry *)0)->partition_name)

static int mtd_gpt_read_header(struct mtd_info *mtd, gpt_header *gpt, int lba, u64 last_lba)
{
	loff_t read_offset = mtd_gpt_lba_to_offset(lba);
	size_t retlen;
	int ret;

	dev_dbg(&mtd->dev, "reading GPT header at offset %lli\n", read_offset);

	ret = mtd_read(mtd, read_offset, MTD_GPT_LBA_SIZE, &retlen, (u_char *)gpt);
	if (ret) {
		dev_err(&mtd->dev, "failed to read GPT header\n");
		return -EIO;
	}

	if (retlen < MTD_GPT_LBA_SIZE) {
		dev_err(&mtd->dev, "truncated read 0x%zx expected 0x%x\n",
			retlen, MTD_GPT_LBA_SIZE);
		return -EIO;
	}

	return gpt_validate_header(gpt, lba, MTD_GPT_LBA_SIZE, last_lba);
}

static int mtd_gpt_parse_partitions(struct mtd_info *mtd,
				    const struct mtd_partition **pparts,
				    struct mtd_part_parser_data *data)
{
	int ret = 0, valid_parts = 0, part_no;
	u64 last_lba = div_u64(mtd->size, MTD_GPT_LBA_SIZE) - 1ULL;
	struct mtd_partition *parts, *part;
	size_t pte_read_size, retlen;
	char *name_buf, *cur_name;
	loff_t pte_read_offset;
	gpt_header *gpt, *agpt;
	gpt_entry *ptes, *pte;
	unsigned int nr_parts;

	if (mtd_type_is_nand(mtd)) {
		/* NAND flash devices are too susceptible to bad blocks */
		dev_err(&mtd->dev, "GPT parsing is forbidden on NAND devices!");
		return -EPERM;
	}

	gpt = kzalloc(MTD_GPT_LBA_SIZE, GFP_KERNEL);
	if (!gpt)
		return -ENOMEM;

	agpt = kzalloc(MTD_GPT_LBA_SIZE, GFP_KERNEL);
	if (!agpt)
		return -ENOMEM;

	ret = mtd_gpt_read_header(mtd, gpt, GPT_PRIMARY_PARTITION_TABLE_LBA, last_lba);
	if (ret) {
		dev_warn(&mtd->dev,
			 "invalid primary GPT header: error %d, trying alternate", ret);
		ret = mtd_gpt_read_header(mtd, gpt, le64_to_cpu(gpt->alternate_lba), last_lba);
		if (ret) {
			dev_err(&mtd->dev, "invalid alternate GPT header: error %d\n", ret);
			goto free_gpt;
		}
	} else {
		/* Check alternate header and warn if it doesn't match primary header */
		gpt_compare_alt(gpt, agpt, last_lba);
	}

	/* This should contain the PTE array */
	pte_read_offset = mtd_gpt_lba_to_offset(le64_to_cpu(gpt->partition_entry_lba));
	pte_read_size = get_pt_size(gpt);
	ptes = kzalloc(pte_read_size, GFP_KERNEL);
	if (!ptes) {
		ret = -ENOMEM;
		goto free_gpt;
	}

	dev_dbg(&mtd->dev, "reading PTE array offset %lli size 0x%zx\n",
		pte_read_offset, pte_read_size);

	ret = mtd_read(mtd, pte_read_offset, pte_read_size, &retlen, (u_char *)ptes);
	if (ret)
		goto free_ptes;

	if (retlen < pte_read_size) {
		ret = -EIO;
		goto free_ptes;
	}

	ret = gpt_check_pte_array_crc(gpt, ptes);
	if (ret) {
		dev_err(&mtd->dev,
			"CRC check failed for GPT Partition Table Entry array! error %d\n", ret);
		goto free_ptes;
	}

	nr_parts = le32_to_cpu(gpt->num_partition_entries);
	parts = kcalloc(min(nr_parts, MTD_GPT_MAX_PARTS),
			sizeof(*parts),
			GFP_KERNEL);
	if (!parts) {
		ret = -ENOMEM;
		goto free_ptes;
	}

	name_buf = kcalloc(min(nr_parts, MTD_GPT_MAX_PARTS), (MTD_GPT_PARTNAME_SIZE + 1),
			   GFP_KERNEL);
	if (!name_buf) {
		ret = -ENOMEM;
		goto free_parts;
	}

	for (part_no = 0; part_no < nr_parts && valid_parts < MTD_GPT_MAX_PARTS; part_no++) {
		pte = &ptes[part_no];
		part = &parts[valid_parts];
		cur_name = &name_buf[valid_parts * (MTD_GPT_PARTNAME_SIZE + 1)];

		if (!gpt_is_pte_valid(pte, last_lba)) {
			dev_warn(&mtd->dev, "skipping invalid partition entry %d!\n", part_no);
			continue;
		}

		part->offset = mtd_gpt_lba_to_offset(le64_to_cpu(pte->starting_lba));
		part->size = mtd_gpt_lba_to_size(le64_to_cpu(pte->ending_lba) -
						 le64_to_cpu(pte->starting_lba) + 1ULL);
		part->name = cur_name;

		/* part->name is const so we can't pass it directly */
		utf16_le_to_7bit(pte->partition_name,
				 MTD_GPT_PARTNAME_SIZE / sizeof(__le16),
				 cur_name);
		valid_parts++;
	}

	if (valid_parts == MTD_GPT_MAX_PARTS)
		dev_warn(&mtd->dev,
			 "reached maximum allowed number of MTD partitions %d\n",
			 MTD_GPT_MAX_PARTS);

	*pparts = parts;
	kfree(ptes);
	kfree(gpt);
	return valid_parts;

free_parts:
	kfree(parts);
free_ptes:
	kfree(ptes);
free_gpt:
	kfree(agpt);
	kfree(gpt);

	return ret;
}

static void mtd_gpt_cleanup_partitions(const struct mtd_partition *pparts, int nr_parts)
{
	kfree(pparts->name);
	kfree(pparts);
}

static const struct of_device_id mtd_gpt_of_match_table[] = {
	{ .compatible = "gpt" },
	{},
};
MODULE_DEVICE_TABLE(of, mtd_gpt_of_match_table);

static struct mtd_part_parser mtd_gpt_parser = {
	.parse_fn = mtd_gpt_parse_partitions,
	.cleanup = mtd_gpt_cleanup_partitions,
	.name = "GPT",
	.of_match_table = mtd_gpt_of_match_table,
};
module_mtd_part_parser(mtd_gpt_parser);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Romain Gantois <romain.gantois@bootlin.com>");
MODULE_DESCRIPTION("MTD parser for GPT partition tables");
