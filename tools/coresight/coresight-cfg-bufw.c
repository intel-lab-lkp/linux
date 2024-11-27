// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020-2022 Linaro Limited, All rights reserved.
 * Author: Mike Leach <mike.leach@linaro.org>
 */

#include <string.h>

#include "coresight-cfg-bufw.h"
#include "coresight-config-uapi.h"

/*
 * Set of macros to make writing the buffer code easier.
 *.
 * Uses naming convention as 'buffer' for the buffer pointer and
 * 'used' as the current bytes used by the encosing function.
 */
#define cscfg_write_u64(val64) { \
	*(u64 *)(buffer + used) = val64; \
	used += sizeof(u64); \
	}

#define cscfg_write_u32(val32) { \
	*(u32 *)(buffer + used) = val32; \
	used += sizeof(u32); \
	}

#define cscfg_write_u16(val16) { \
	*(u16 *)(buffer + used) = val16; \
	used += sizeof(u16); \
	}

#define cscfg_write_u8(val8) { \
	*(buffer + used) = val8; \
	used++;	\
	}

/* write the header at the start of the buffer */
static int cscfg_table_write_fhdr(u8 *buffer, const int buflen,
				 const struct cscfg_table_header *fhdr)
{
	int used = 0;

	cscfg_write_u32(fhdr->magic_version);
	cscfg_write_u16(fhdr->length);
	cscfg_write_u16(fhdr->nr_configs);
	cscfg_write_u16(fhdr->nr_features);
	return used;
}

static int cscfg_table_write_string(u8 *buffer, const int buflen, const char *string)
{
	int len, used = 0;

	len = strlen(string);
	if (len > CSCFG_TABLE_STR_MAXSIZE)
		return -EINVAL;

	if (buflen < (len + 1 + sizeof(u16)))
		return -EINVAL;

	cscfg_write_u16((u16)(len + 1));
	strncpy((char *)(buffer + used), string, len + 1);
	used += (len + 1);

	return used;
}

static int cscfg_table_write_elem_hdr(u8 *buffer, const int buflen,
				     struct cscfg_table_elem_header *ehdr)
{
	int used = 0;

	if (buflen < (sizeof(u16) + sizeof(u8)))
		return -EINVAL;

	cscfg_write_u16(ehdr->elem_length);
	cscfg_write_u8(ehdr->elem_type);

	return used;
}

static int cscfg_table_write_config(u8 *buffer, const int buflen,
				   struct cscfg_config_desc *config_desc)
{
	int used = 0, bytes_w, space_req, preset_bytes, i;
	struct cscfg_table_elem_header ehdr;

	ehdr.elem_length = 0;
	ehdr.elem_type = CSCFG_TABLE_ELEM_TYPE_CFG;

	/* write element header at current buffer location */
	bytes_w = cscfg_table_write_elem_hdr(buffer, buflen, &ehdr);
	if (bytes_w < 0)
		return bytes_w;
	used += bytes_w;

	/* write out the configuration name */
	bytes_w = cscfg_table_write_string(buffer + used, buflen - used,
					  config_desc->name);
	if (bytes_w < 0)
		return bytes_w;
	used += bytes_w;

	/* write out the description string */
	bytes_w = cscfg_table_write_string(buffer + used, buflen - used,
					  config_desc->description);
	if (bytes_w < 0)
		return bytes_w;
	used += bytes_w;

	/*
	 * calculate the space needed for variables + presets
	 * [u16 value - nr_presets]
	 * [u32 value - nr_total_params]
	 * [u16 value - nr_feat_refs]
	 * [u64 values] * (nr_presets * nr_total_params)
	 */
	preset_bytes = sizeof(u64) * config_desc->nr_presets * config_desc->nr_total_params;
	space_req = (sizeof(u16) * 2) + sizeof(u32) + preset_bytes;

	if ((buflen - used) < space_req)
		return -EINVAL;

	cscfg_write_u16((u16)config_desc->nr_presets);
	cscfg_write_u32((u32)config_desc->nr_total_params);
	cscfg_write_u16((u16)config_desc->nr_feat_refs);
	if (preset_bytes) {
		memcpy(buffer + used, (u8 *)config_desc->presets, preset_bytes);
		used += preset_bytes;
	}

	/* now write the feature ref names */
	for (i = 0; i < config_desc->nr_feat_refs; i++) {
		bytes_w = cscfg_table_write_string(buffer + used, buflen - used,
						   config_desc->feat_ref_names[i]);
		if (bytes_w < 0)
			return bytes_w;
		used += bytes_w;
	}

	/* rewrite the element header with the correct length */
	ehdr.elem_length = used;
	bytes_w = cscfg_table_write_elem_hdr(buffer, buflen, &ehdr);
	/* used must not be updated here */
	if (bytes_w < 0)
		return bytes_w;

	return used;
}

/*
 * write a parameter structure into the buffer in following format:
 * [cscfg_table_elem_str]    - parameter name.
 * [u64 value: param_value] - initial value.
 */
static int cscfg_table_write_param(u8 *buffer, const int buflen,
				  struct cscfg_parameter_desc *param_desc)
{
	int used = 0, bytes_w;

	bytes_w = cscfg_table_write_string(buffer + used, buflen - used,
					  param_desc->name);
	if (bytes_w < 0)
		return bytes_w;
	used += bytes_w;

	if ((buflen - used) < sizeof(u64))
		return -EINVAL;

	cscfg_write_u64(param_desc->value);
	return used;
}

/*
 * Write a feature element from cscfg_feature_desc in following format:
 *
 * [cscfg_table_elem_header] - header length is total bytes to end of param structures.
 * [cscfg_table_elem_str]    - feature name.
 * [cscfg_table_elem_str]    - feature description.
 * [u32 value: match_flags]
 * [u16 value: nr_regs]	    - number of registers.
 * [u16 value: nr_params]   - number of parameters.
 * [cscfg_regval_desc struct] * nr_regs
 * [PARAM_ELEM] * nr_params
 */
static int cscfg_table_write_feat(u8 *buffer, const int buflen,
				 struct cscfg_feature_desc *feat_desc)
{
	struct cscfg_table_elem_header ehdr;
	struct cscfg_regval_desc *p_reg_desc;
	int used = 0, bytes_w, i, space_req;
	u32 val32;

	ehdr.elem_length = 0;
	ehdr.elem_type = CSCFG_TABLE_ELEM_TYPE_FEAT;

	/* write element header at current buffer location */
	bytes_w = cscfg_table_write_elem_hdr(buffer, buflen, &ehdr);
	if (bytes_w < 0)
		return bytes_w;
	used += bytes_w;

	/* write out the name string */
	bytes_w = cscfg_table_write_string(buffer + used, buflen - used,
					  feat_desc->name);
	if (bytes_w < 0)
		return bytes_w;
	used += bytes_w;

	/* write out the description string */
	bytes_w = cscfg_table_write_string(buffer + used, buflen - used,
					  feat_desc->description);
	if (bytes_w < 0)
		return bytes_w;
	used += bytes_w;

	/* check for space for variables and register structures */
	space_req = (sizeof(u16) * 2) + sizeof(u32) +
		(sizeof(struct cscfg_regval_desc) * feat_desc->nr_regs);
	if ((buflen - used) < space_req)
		return -EINVAL;

	/* write the variables */
	cscfg_write_u32((u32)feat_desc->match_flags);
	cscfg_write_u16((u16)feat_desc->nr_regs);
	cscfg_write_u16((u16)feat_desc->nr_params);

	/*write the registers */
	for (i = 0; i < feat_desc->nr_regs; i++) {
		p_reg_desc = (struct cscfg_regval_desc *)&feat_desc->regs_desc[i];
		CSCFG_TABLE_REG_DESC_INFO_TO_U32(val32, p_reg_desc);
		cscfg_write_u32(val32);
		cscfg_write_u64(feat_desc->regs_desc[i].val64);
	}

	/* write any parameters */
	for (i = 0; i < feat_desc->nr_params; i++) {
		bytes_w = cscfg_table_write_param(buffer + used, buflen - used,
						  &feat_desc->params_desc[i]);
		if (bytes_w < 0)
			return bytes_w;
		used += bytes_w;

	}

	/*
	 * rewrite the element header at the start of the buffer block
	 * with the correct length
	 */
	ehdr.elem_length = used;
	bytes_w = cscfg_table_write_elem_hdr(buffer, buflen, &ehdr);
	/* used must not be updated here */
	if (bytes_w < 0)
		return bytes_w;

	return used;
}

/*
 * write a buffer from the configuration and feature
 * descriptors to write into a file for configfs.
 *
 * Will only write one config, and/or a number of features,
 * per the file standard.
 */
int cscfg_table_write_buffer(u8 *buffer, const int buflen,
			    struct cscfg_config_desc **config_descs,
			    struct cscfg_feature_desc **feat_descs)
{
	struct cscfg_table_header fhdr;
	int used = 0,  bytes_w, i;

	/* init the file header */
	fhdr.magic_version = CSCFG_TABLE_MAGIC_VERSION;
	fhdr.length = 0;
	fhdr.nr_configs = 0;
	fhdr.nr_features = 0;

	/* count the configs */
	if (config_descs) {
		while (config_descs[fhdr.nr_configs])
			fhdr.nr_configs++;
	}

	/* count the features */
	if (feat_descs) {
		while (feat_descs[fhdr.nr_features])
			fhdr.nr_features++;
	}

	/* need a buffer and at least one config or feature */
	if ((!fhdr.nr_configs && !fhdr.nr_features) ||
	    !buffer || (buflen > CSCFG_TABLE_MAXSIZE))
		return -EINVAL;

	/* write a header at the start to get the length of the header */
	bytes_w = cscfg_table_write_fhdr(buffer, buflen, &fhdr);
	if (bytes_w < 0)
		return bytes_w;
	used += bytes_w;

	/* write configs */
	for (i = 0; i < fhdr.nr_configs; i++) {
		bytes_w = cscfg_table_write_config(buffer + used, buflen - used,
						   config_descs[i]);
		if (bytes_w < 0)
			return bytes_w;
		used += bytes_w;
	}

	/* write any features */
	for (i = 0; i < fhdr.nr_features; i++) {
		bytes_w = cscfg_table_write_feat(buffer + used, buflen - used,
						 feat_descs[i]);
		if (bytes_w < 0)
			return bytes_w;
		used += bytes_w;
	}

	/* finally re-write the header at the buffer start with the correct length */
	fhdr.length = (u16)used;
	bytes_w = cscfg_table_write_fhdr(buffer, buflen, &fhdr);
	/* used must not be updated here */
	if (bytes_w < 0)
		return bytes_w;
	return used;
}
