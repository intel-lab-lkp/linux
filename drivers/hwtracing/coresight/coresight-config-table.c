// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 Linaro Limited, All rights reserved.
 * Author: Mike Leach <mike.leach@linaro.org>
 */

#include "coresight-config-table.h"

#define cscfg_extract_u64(val64) { \
	val64 = *(u64 *)(buffer + used); \
	used += sizeof(u64); \
	}

#define cscfg_extract_u32(val32) { \
	val32 = *(u32 *)(buffer + used); \
	used += sizeof(u32); \
	}

#define cscfg_extract_u16(val16) { \
	val16 = *(u16 *)(buffer + used); \
	used += sizeof(u16); \
	}

#define cscfg_extract_u8(val8) { \
	val8 = *(buffer + used); \
	used++; \
	}

static int cscfg_table_read_hdr(const u8 *buffer, const int buflen, int *buf_used,
				struct cscfg_table_header *hdr)
{
	/* table header always at the start of the buffer */
	int used = 0;

	if (buflen < sizeof(struct cscfg_table_header))
		return -EINVAL;

	cscfg_extract_u32(hdr->magic_version);
	if (hdr->magic_version != CSCFG_TABLE_MAGIC_VERSION)
		return -EINVAL;

	cscfg_extract_u16(hdr->length);
	if (hdr->length > buflen)
		return -EINVAL;

	cscfg_extract_u16(hdr->nr_configs);
	cscfg_extract_u16(hdr->nr_features);

	*buf_used = used;
	return 0;
}

static int cscfg_table_read_elem_hdr(const u8 *buffer, const int buflen, int *buf_used,
				     struct cscfg_table_elem_header *elem_hdr)
{
	int used = *buf_used;

	if ((buflen - used) < (sizeof(u16) + sizeof(u8)))
		return -EINVAL;

	/* read length and check enough buffer remains for this element */
	elem_hdr->elem_length = *(u16 *)(buffer + used);
	if ((buflen - used) < elem_hdr->elem_length)
		return -EINVAL;
	/* don't use extract fn as we update used _after_ the comparison */
	used += sizeof(u16);

	/* read type and validate */
	cscfg_extract_u8(elem_hdr->elem_type);
	if ((elem_hdr->elem_type < CSCFG_TABLE_ELEM_TYPE_FEAT) ||
	    (elem_hdr->elem_type > CSCFG_TABLE_ELEM_TYPE_CFG))
		return -EINVAL;

	*buf_used = used;
	return 0;
}

static int cscfg_table_read_elem_str(const u8 *buffer, const int buflen, int *buf_used,
				     struct cscfg_table_elem_str *elem_str)
{
	int used = *buf_used;

	if ((buflen - used) < sizeof(u16))
		return -EINVAL;

	cscfg_extract_u16(elem_str->str_len);

	if ((buflen - used) < elem_str->str_len)
		return -EINVAL;

	/* check for 0 termination */
	if (buffer[used + (elem_str->str_len - 1)] != 0)
		return -EINVAL;

	elem_str->str = kstrdup((char *)(buffer + used),  GFP_KERNEL);
	used += elem_str->str_len;

	*buf_used = used;
	return 0;
}

static int cscfg_table_alloc_desc_arrays(struct cscfg_table_load_descs *desc_arrays,
					 int nr_features, int nr_configs)
{
	/* arrays are 0 terminated - nr_configs & nr_features elements */
	desc_arrays->config_descs = kcalloc(nr_configs + 1,  sizeof(struct cscfg_config_desc *),
					    GFP_KERNEL);
	if (!desc_arrays->config_descs)
		return -ENOMEM;
	desc_arrays->feat_descs = kcalloc(nr_features + 1, sizeof(struct cscfg_feature_desc *),
					  GFP_KERNEL);
	if (!desc_arrays->feat_descs)
		return -ENOMEM;
	return 0;
}

/* free up the data allocated to a config desc */
static void cscfg_table_free_config_desc(struct cscfg_config_desc *config_desc)
{
	int i;

	if (!config_desc)
		return;

	/* free presets */
	kfree(config_desc->presets);

	/* free feat ref strings */
	if (config_desc->nr_feat_refs) {
		/* each string */
		for (i = 0; i < config_desc->nr_feat_refs; i++)
			kfree(config_desc->feat_ref_names[i]);

		/* and the char * array */
		kfree(config_desc->feat_ref_names);
	}

	/* next the strings */
	kfree(config_desc->name);
	kfree(config_desc->description);

	/* finally the struct itself */
	kfree(config_desc);
}

static int cscfg_table_read_elem_config(const u8 *buffer, const int buflen, int *buf_used,
					struct cscfg_table_load_descs *desc_arrays,
					const int cfg_index)
{
	struct cscfg_table_elem_header elem_hdr;
	struct cscfg_table_elem_str elem_str;
	struct cscfg_config_desc *config_desc;
	int used = *buf_used, nr_preset_vals, nr_preset_bytes, i;
	int err = 0;
	u64 *presets;

	/*
	 * read the header - if not config, then don't update buf_used
	 * pointer on return
	 */
	err = cscfg_table_read_elem_hdr(buffer, buflen, &used, &elem_hdr);
	if (err)
		return err;
	if (elem_hdr.elem_type != CSCFG_TABLE_ELEM_TYPE_CFG)
		return 0;

	/* we have a config - allocate the descriptor */
	config_desc = kzalloc(sizeof(struct cscfg_config_desc), GFP_KERNEL);
	if (!config_desc)
		return -ENOMEM;

	/* read the name string */
	err = cscfg_table_read_elem_str(buffer, buflen, &used, &elem_str);
	if (err)
		return err;
	config_desc->name = elem_str.str;

	/* allocate load name if not set */
	if (!desc_arrays->load_name)
		desc_arrays->load_name = kstrdup(config_desc->name, GFP_KERNEL);

	/* read the description string */
	err = cscfg_table_read_elem_str(buffer, buflen, &used, &elem_str);
	if (err)
		return err;
	config_desc->description = elem_str.str;

	/* read in some values */
	if ((buflen - used) < sizeof(u64))
		return -EINVAL;
	cscfg_extract_u16(config_desc->nr_presets);
	cscfg_extract_u32(config_desc->nr_total_params);
	cscfg_extract_u16(config_desc->nr_feat_refs);

	/* read the array of 64bit presets if present */
	nr_preset_vals = config_desc->nr_total_params * config_desc->nr_presets;
	if (nr_preset_vals) {
		presets = kcalloc(nr_preset_vals, sizeof(u64), GFP_KERNEL);
		if (!presets)
			return -ENOMEM;

		nr_preset_bytes = sizeof(u64) * nr_preset_vals;
		if ((buflen - used) < nr_preset_bytes)
			return -EINVAL;

		memcpy(presets, (buffer + used), nr_preset_bytes);
		config_desc->presets = presets;
		used += nr_preset_bytes;
	}

	/* read the array of feature names referenced by the config */
	if (config_desc->nr_feat_refs) {
		config_desc->feat_ref_names = kcalloc(config_desc->nr_feat_refs,
						      sizeof(char *), GFP_KERNEL);
		if (!config_desc->feat_ref_names)
			return -ENOMEM;

		for (i = 0; i < config_desc->nr_feat_refs; i++) {
			err = cscfg_table_read_elem_str(buffer, buflen, &used, &elem_str);
			if (err)
				return err;
			config_desc->feat_ref_names[i] = elem_str.str;
		}
	}

	desc_arrays->config_descs[cfg_index] = config_desc;
	*buf_used = used;
	return 0;
}

static int cscfg_table_read_elem_param(const u8 *buffer, const int buflen, int *buf_used,
				       struct cscfg_parameter_desc *param_desc)
{
	struct cscfg_table_elem_str elem_str;
	int err = 0, used = *buf_used;

	/* parameter name */
	err = cscfg_table_read_elem_str(buffer, buflen, &used, &elem_str);
	if (err)
		return err;
	param_desc->name = elem_str.str;

	/* parameter value */
	if ((buflen - used) < sizeof(u64))
		return -EINVAL;
	cscfg_extract_u64(param_desc->value);

	*buf_used = used;
	return err;
}

static void cscfg_table_free_feat_desc(struct cscfg_feature_desc *feat_desc)
{
	if (!feat_desc)
		return;

	/* free up the register descriptor array */
	kfree(feat_desc->regs_desc);

	/* free up the parameters array */
	kfree(feat_desc->params_desc);

	/* name and description strings */
	kfree(feat_desc->name);
	kfree(feat_desc->description);

	/* finally the struct itself */
	kfree(feat_desc);
}

static int cscfg_table_read_elem_feature(const u8 *buffer, const int buflen, int *buf_used,
					 struct cscfg_table_load_descs *desc_arrays,
					 const int feat_idx)
{
	struct cscfg_table_elem_header elem_hdr;
	struct cscfg_table_elem_str elem_str;
	struct cscfg_feature_desc *feat_desc;
	struct cscfg_regval_desc *p_reg_desc;
	int used = *buf_used, err, i, nr_regs_bytes;
	u32 val32;

	/* allocate the feature descriptor object */
	feat_desc = kzalloc(sizeof(struct cscfg_feature_desc), GFP_KERNEL);
	if (!feat_desc)
		return -ENOMEM;

	/* read and check the element header */
	err = cscfg_table_read_elem_hdr(buffer, buflen, &used, &elem_hdr);
	if (err)
		return err;

	if (elem_hdr.elem_type != CSCFG_TABLE_ELEM_TYPE_FEAT)
		return -EINVAL;

	/* read the feature name */
	err = cscfg_table_read_elem_str(buffer, buflen, &used, &elem_str);
	if (err)
		return err;
	feat_desc->name = elem_str.str;

	/* allocate load name if not set previously by config  */
	if (!desc_arrays->load_name)
		desc_arrays->load_name = kstrdup(feat_desc->name, GFP_KERNEL);

	/* read the description string */
	err = cscfg_table_read_elem_str(buffer, buflen, &used, &elem_str);
	if (err)
		return err;
	feat_desc->description = elem_str.str;

	/*
	 * read in some values
	 * [u32 value: match_flags]
	 * [u16 value: nr_regs]	    - number of registers.
	 * [u16 value: nr_params]   - number of parameters.
	 */
	cscfg_extract_u32(feat_desc->match_flags);
	cscfg_extract_u16(feat_desc->nr_regs);
	cscfg_extract_u16(feat_desc->nr_params);

	/* register descriptors  - 32 bit + 64 bit value */
	if (feat_desc->nr_regs) {
		nr_regs_bytes = ((sizeof(u32) + sizeof(u64)) * feat_desc->nr_regs);
		if ((buflen - used) < nr_regs_bytes)
			return -EINVAL;
		feat_desc->regs_desc = kcalloc(feat_desc->nr_regs,
					       sizeof(struct cscfg_regval_desc), GFP_KERNEL);
		if (!feat_desc->regs_desc)
			return -ENOMEM;

		for (i = 0; i < feat_desc->nr_regs; i++) {
			cscfg_extract_u32(val32);
			p_reg_desc = (struct cscfg_regval_desc *)&feat_desc->regs_desc[i];
			CSCFG_TABLE_U32_TO_REG_DESC_INFO(val32, p_reg_desc);
			cscfg_extract_u64(feat_desc->regs_desc[i].val64);
		}
	}

	/* parameter descriptors - string + 64 bit value */
	if (feat_desc->nr_params) {
		feat_desc->params_desc = kcalloc(feat_desc->nr_params,
						 sizeof(struct cscfg_parameter_desc), GFP_KERNEL);
		if (!feat_desc->params_desc)
			return -ENOMEM;
		for (i = 0; i < feat_desc->nr_params; i++) {
			err = cscfg_table_read_elem_param(buffer, buflen, &used,
							 &feat_desc->params_desc[i]);
			if (err)
				return err;
		}
	}

	desc_arrays->feat_descs[feat_idx] = feat_desc;
	*buf_used = used;
	return 0;
}

/*
 * Read the table from a buffer and create the internal configuration and
 * feature descriptors to load into the cscfg system
 */
int cscfg_table_read_buffer(const u8 *buffer, const int buflen,
			    struct cscfg_table_load_descs *desc_arrays)
{
	struct cscfg_table_header hdr;
	int used = 0, err, i;

	/* read in the table  header */
	err = cscfg_table_read_hdr(buffer, buflen, &used, &hdr);
	if (err)
		return err;

	/* allocate the memory for the descriptor pointer arrays */
	err = cscfg_table_alloc_desc_arrays(desc_arrays, hdr.nr_features, hdr.nr_configs);
	if (err)
		return err;

	/* read elements */

	/* first elements are configurations */
	for (i = 0; i < hdr.nr_configs; i++) {
		err = cscfg_table_read_elem_config(buffer, buflen, &used, desc_arrays, i);
		if (err)
			return err;
	}

	/* now read and populate all the feature descriptors */
	for (i = 0; i < hdr.nr_features; i++) {
		err = cscfg_table_read_elem_feature(buffer, buflen, &used, desc_arrays, i);
		if (err)
			return err;
	}
	return 0;
}

/*
 * Need to free up the dynamically allocated descriptor arrays on unload
 * as the memory used could be significant if many configurations are loaded
 * and unloaded while the machine is operational.
 *
 * This frees up all the memory allocated by config during the load process.
 */
void cscfg_table_free_load_descs(struct cscfg_table_load_descs *desc_arrays)
{
	int i = 0;

	if (!desc_arrays)
		return;

	/* free up each of the config descriptors */
	while (desc_arrays->config_descs[i]) {
		cscfg_table_free_config_desc(desc_arrays->config_descs[i]);
		i++;
	}

	/* free up each of the feature descriptors */
	i = 0;
	while (desc_arrays->feat_descs[i]) {
		cscfg_table_free_feat_desc(desc_arrays->feat_descs[i]);
		i++;
	}

	/* finally free up the load descs pointer arrays */
	kfree(desc_arrays->config_descs);
	kfree(desc_arrays->feat_descs);
	kfree(desc_arrays->load_name);
}
