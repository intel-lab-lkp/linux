/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __OFPART_AIROHA_H
#define __OFPART_AIROHA_H

#ifdef CONFIG_MTD_OF_PARTS_AIROHA
int airoha_partitions_post_parse(struct mtd_info *mtd,
				 struct mtd_partition *parts,
				 int nr_parts);
#else
static inline int airoha_partitions_post_parse(struct mtd_info *mtd,
					       struct mtd_partition *parts,
					       int nr_parts)
{
	return -EOPNOTSUPP;
}
#endif

#endif
