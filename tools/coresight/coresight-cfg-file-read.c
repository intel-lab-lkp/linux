// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020-2022 Linaro Limited, All rights reserved.
 * Author: Mike Leach <mike.leach@linaro.org>
 */

#include <linux/types.h>
#include <linux/unistd.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "coresight-config-table.h"
#include "coresight-config-uapi.h"

/*
 * tool to read and print a generated configuration
 * re-uses the read code source from the driver.
 */

static void validate_config_name(const char *name)
{
	int i, len = strlen(name);

	for (i = 0; i < len; i++) {
		if (!isalnum(name[i]) && !(name[i] == '_')) {
			printf("\n************************************************\n");
			printf("ERROR: Configuration name %s invalid character(s)\n", name);
			printf("     : must contain only alphanumeric and _ only\n");
			printf("************************************************\n");
		}
	}
}

static void print_configs(struct cscfg_table_load_descs *load_descs, int *nr_configs)
{
	struct cscfg_config_desc *config_desc;
	int i, j, p, cfg_idx = 0;

	*nr_configs = 0;
	config_desc = load_descs->config_descs[cfg_idx];
	if (!config_desc) {
		printf("File contains no configurations.\n\n");
		return;
	}

	while (config_desc) {
		printf("Configuration %d\nName:- %s\n", cfg_idx + 1, config_desc->name);
		validate_config_name(config_desc->name);
		printf("Description:-\n%s\n", config_desc->description);
		printf("Uses %d features:-\n", config_desc->nr_feat_refs);
		for (i = 0; i < config_desc->nr_feat_refs; i++)
			printf("Feature-%d: %s\n", i + 1, config_desc->feat_ref_names[i]);

		printf("\nProvides %d sets of preset values, %d presets per set\n",
		       config_desc->nr_presets, config_desc->nr_total_params);
		if (config_desc->nr_presets) {
			for (i = 0; i < config_desc->nr_presets; i++) {
				printf("set[%d]: ", i);
				for (j = 0; j < config_desc->nr_total_params; j++) {
					p = (i * config_desc->nr_total_params) + j;
					printf("0x%lx, ",  config_desc->presets[p]);
				}
				printf("\n");
			}
		}
		printf("\n============================================\n");
		cfg_idx++;
		config_desc = load_descs->config_descs[cfg_idx];
		(*nr_configs)++;
	}
}

static void print_reg_type_info(u8 type)
{
	if (type & CS_CFG_REG_TYPE_STD)
		printf("std_reg ");
	if (type & CS_CFG_REG_TYPE_RESOURCE)
		printf("resource ");
	if (type & CS_CFG_REG_TYPE_VAL_PARAM)
		printf("param_index ");
	if (type & CS_CFG_REG_TYPE_VAL_64BIT)
		printf("64_bit ");
	else
		printf("32_bit ");
	if (type & CS_CFG_REG_TYPE_VAL_MASK)
		printf("masked ");
	if (type & CS_CFG_REG_TYPE_VAL_SAVE)
		printf("save_on_disable ");

}

static void print_regs(int nr, struct cscfg_regval_desc *regs_desc_array)
{
	int i;
	struct cscfg_regval_desc *reg_desc;
	u8 type;
	u16 offset;
	u16 info;

	for (i = 0; i < nr; i++) {
		reg_desc = &regs_desc_array[i];
		type = (u8)reg_desc->type;
		offset = (u16)reg_desc->offset;
		info = (u16)reg_desc->hw_info;

		printf("Reg(%d): Type 0x%x: ", i, type);
		print_reg_type_info(type);
		printf("\nOffset: 0x%03x; HW Info: 0x%03x\n", offset, info);
		printf("Value: ");
		if (type & CS_CFG_REG_TYPE_VAL_64BIT)
			printf("0x%lx\n", reg_desc->val64);
		else if (type & CS_CFG_REG_TYPE_VAL_PARAM) {
			printf("param(%d) ", reg_desc->param_idx);
			if (type & (CS_CFG_REG_TYPE_VAL_MASK))
				printf(" mask: 0x%x", reg_desc->mask32);
			printf("\n");
		} else {
			printf("0x%x ", reg_desc->val32);
			if (type & (CS_CFG_REG_TYPE_VAL_MASK))
				printf(" mask: 0x%x", reg_desc->mask32);
			printf("\n");
		}
	}
}

static void print_params(int nr, struct cscfg_parameter_desc *params_desc)
{
	int i;

	for (i = 0; i < nr; i++)
		printf("Param(%d) : %s; Init value 0x%lx\n", i,
		       params_desc[i].name, params_desc[i].value);
}

static void print_features(struct cscfg_table_load_descs *load_descs, int *nr_feats)
{
	struct cscfg_feature_desc *feat_desc = 0;
	int idx = 0;

	*nr_feats = 0;
	feat_desc = load_descs->feat_descs[idx];
	if (!feat_desc) {
		printf("File contains no features\n\n");
		return;
	}

	while (feat_desc) {
		printf("Feature %d\nName:- %s\n\n", idx + 1, feat_desc->name);
		printf("Description:- %s\n", feat_desc->description);
		printf("Match flags: 0x%x\n", feat_desc->match_flags);
		printf("\nNumber of Paraneters: %d\n", feat_desc->nr_params);
		if (feat_desc->nr_params)
			print_params(feat_desc->nr_params, feat_desc->params_desc);
		printf("\nNumber of Registers: %d\n", feat_desc->nr_regs);
		if (feat_desc->nr_regs)
			print_regs(feat_desc->nr_regs, feat_desc->regs_desc);
		printf("\n============================================\n");

		/* next feature */
		idx++;
		feat_desc = load_descs->feat_descs[idx];
		(*nr_feats)++;
	}
}

int main(int argc, char **argv)
{
	FILE *fp;
	struct cscfg_table_load_descs *load_descs;
	int err, fsize, nr_configs = 0, nr_feats = 0;
	u8 buffer[CSCFG_TABLE_MAXSIZE];

	printf("\n\n============================================\n");
	printf("CoreSight Configuration table file reader");
	printf("\n============================================\n\n");

	/* need a filename */
	if (argc <= 1) {
		printf("Please provide filename on command line\n");
		return -EINVAL;
	}

	/* open file and read into the buffer. */
	fp = fopen(argv[1], "rb");
	if (fp == NULL) {
		printf("Error opening file %s\n", argv[1]);
		return -EINVAL;
	}

	fseek(fp, 0, SEEK_END);
	fsize = ftell(fp);
	rewind(fp);
	if (fsize > CSCFG_TABLE_MAXSIZE) {
		printf("Error: Input file too large.");
		fclose(fp);
		return -EINVAL;
	}
	err = fread(buffer, sizeof(u8), fsize, fp);
	fclose(fp);

	if (err < fsize) {
		printf("Error reading file %s\n", argv[1]);
		return -EINVAL;
	}

	/* allocate the descriptor structures to be populated by read operation */
	load_descs = calloc(1, sizeof(struct cscfg_table_load_descs));
	if (!load_descs) {
		printf("Error allocating load descs structure.\n");
		return -ENOMEM;
	}

	/* read the buffer and create the configuration and feature structures */
	err = cscfg_table_read_buffer(buffer, fsize, load_descs);
	if (err) {
		printf("Error reading configuration file\n");
		goto exit_free_mem;
	}

	/* print the contents of the structures */
	print_configs(load_descs, &nr_configs);
	print_features(load_descs, &nr_feats);

	printf("\n\n======= Summary ============================\n");
	printf(" Config File Name:       : %s\n", argv[1]);
	printf(" Number of Configurations: %d\n", nr_configs);
	printf(" Number of Features      : %d\n", nr_feats);
	printf(" Load name               : %s\n", load_descs->load_name);
	printf("============================================\n\n");


exit_free_mem:
	cscfg_table_free_load_descs(load_descs);
	free(load_descs);
	return err;
}
