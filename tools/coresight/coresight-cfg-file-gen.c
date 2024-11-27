// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020-2022 Linaro Limited, All rights reserved.
 * Author: Mike Leach <mike.leach@linaro.org>
 */

#include <linux/types.h>
#include <linux/unistd.h>
#include <stdio.h>
#include <unistd.h>

#include "coresight-cfg-bufw.h"
#include "coresight-cfg-examples.h"
#include "coresight-cfg-file-gen.h"

/* array of example table files to generate */
struct cscfg_file_eg_info *info_ptrs[] = {
	&file_info_eg1,
	&file_info_eg2,
	NULL,
};

int main(int argc, char **argv)
{
	struct cscfg_config_desc **config_descs;
	struct cscfg_feature_desc **feat_descs;
	u8 buffer[CSCFG_TABLE_MAXSIZE];
	int used, idx = 0;
	FILE *fp;
	const char *filename;

	printf("Coresight Configuration table file Generator\n\n");

	while (info_ptrs[idx]) {
		printf("Generating %s example\n", info_ptrs[idx]->example_name);
		config_descs = info_ptrs[idx]->config_descs;
		feat_descs = info_ptrs[idx]->feat_descs;
		filename = info_ptrs[idx]->filename;
		used = cscfg_table_write_buffer(buffer, CSCFG_TABLE_MAXSIZE,
					       config_descs, feat_descs);

		if (used < 0) {
			printf("Error %d writing configuration %s into buffer\n",
			       used, info_ptrs[idx]->example_name);
			return used;
		}

		fp = fopen(filename, "wb");
		if (fp == NULL) {
			printf("Error opening file %s\n", filename);
			return -1;
		}
		fwrite(buffer, used, sizeof(u8), fp);
		fclose(fp);
		idx++;
	}
	return 0;
}
