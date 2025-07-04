// SPDX-License-Identifier: GPL-2.0
/*
 *  fs/partitions/luks.c
 *  LUKS on raw partition; this is important because a LUKS volume may detected
 *  as a valid Atari partition table, breaking other detection.
 *
 *  Copyright (C) 2025 Robin H. Johnson (robbat2@gentoo.org)
 *
 *  Reference: https://gitlab.com/cryptsetup/LUKS2-docs/blob/master/luks2_doc_wip.pdf
 *  Page 5, Figure 2: LUKS2 binary header on-disk structure
 *  This only looks for the Magic & version; and NOT a UUID that starts at
 *  offset 0xA8.
 */

#include <linux/ctype.h>
#include <linux/compiler.h>
#include "check.h"

#define LUKS_MAGIC_1ST_V1		"LUKS\xba\xbe\x00\x01"
#define LUKS_MAGIC_1ST_V2		"LUKS\xba\xbe\x00\x02"
#define LUKS_MAGIC_2ND_V1		"SKUL\xba\xbe\x00\x01"
#define LUKS_MAGIC_2ND_V2		"SKUL\xba\xbe\x00\x02"

int luks_partition(struct parsed_partitions *state)
{
	Sector sect;
	int ret = 0;
	unsigned char *data;

	data = read_part_sector(state, 0, &sect);

	if (!data)
		return -1;

	if (memcmp(data, LUKS_MAGIC_1ST_V1, 8) == 0
		|| memcmp(data, LUKS_MAGIC_2ND_V1, 8) == 0) {
		strlcat(state->pp_buf, "LUKSv1\n", PAGE_SIZE);
		ret = 1;
	} else if (memcmp(data, LUKS_MAGIC_1ST_V2, 8) == 0
		|| memcmp(data, LUKS_MAGIC_2ND_V2, 8) == 0) {
		strlcat(state->pp_buf, "LUKSv2\n", PAGE_SIZE);
		ret = 1;
	}
	put_dev_sector(sect);
	return ret;
}
