/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2020-2022 Linaro Limited, All rights reserved.
 * Author: Mike Leach <mike.leach@linaro.org>
 */

#ifndef _CORESIGHT_CORESIGHT_CONFIG_TABLE_H
#define _CORESIGHT_CORESIGHT_CONFIG_TABLE_H

#include <linux/sizes.h>

#ifdef __KERNEL__
#include "coresight-config.h"
#else
#include "coresight-config-uapi.h"
#endif


/*
 * Configurations and features can be dynamically loaded at runtime
 * using a table format that is converted on read into the internal
 * structures used by the cscfg infrastructure.
 *
 * Table structure - for loading configuration(s) + feature(s)
 * from configfs or other external sources.
 *
 * [cscfg_table_header]	- mandatory
 * [CONFIG_ELEM] * [cscfg_table_header.nr_configs] - optional.
 * [FEATURE_ELEM] * [cscfg_table_header.nr_features] - optional.
 *
 * Table valid if it has both config(s) and feature(s), only config(s)
 * or only feature(s).
 *
 * Invalid table if no config or features.
 *
 * Structure for [CONFIG_ELEM]:
 *
 * [cscfg_table_elem_header] - header length value to end of feature strings.
 * [cscfg_table_elem_str]    - name of the configuration
 * [cscfg_table_elem_str]    - description of configuration
 * [u16 value - nr_presets] - number of sets of presets supplied
 * [u32 value - nr_total_params] - total of all params in referenced features
 * [u16 value - nr_feat_refs] - number of features selected by this configuration
 * [u64 values] * (nr_presets * nr_total_params)
 * [cscfg_table_elem_str] * nr_feat_refs - names of features selected by configuration.
 *
 *  A configuration must reference at least one feature.
 *  Referenced features may be in this table, or have been loaded previously.
 *
 * Structure for a [FEATURE_ELEM]
 *
 * [cscfg_table_elem_header] - header length is total bytes to end of param structures.
 * [cscfg_table_elem_str]    - feature name.
 * [cscfg_table_elem_str]    - feature description.
 * [u32 value: match_flags]
 * [u16 value: nr_regs]	    - number of registers.
 * [u16 value: nr_params]   - number of parameters.
 * [cscfg_regval_desc struct] * nr_regs
 * [PARAM_ELEM] * nr_params
 *
 * Structure for [PARAM_ELEM]
 *
 * [cscfg_table_elem_str]    - parameter name.
 * [u64 value: param_value] - initial value.
 */

/* major element types - configurations and features */

#define CSCFG_TABLE_ELEM_TYPE_FEAT	0x1
#define CSCFG_TABLE_ELEM_TYPE_CFG	0x2

#define CSCFG_TABLE_MAGIC_VERSION	0xC5CF0001

#define CSCFG_TABLE_U32_TO_REG_DESC_INFO(val32, p_desc) \
	{ \
	p_desc->type = (val32 >> 24) & 0xFF; \
	p_desc->offset = (val32 >> 12) & 0xFFF; \
	p_desc->hw_info = val32 & 0xFFF; \
	}

#define CSCFG_TABLE_REG_DESC_INFO_TO_U32(val32, p_desc) \
	{ \
	val32 = p_desc->hw_info & 0xFFF; \
	val32 |= ((p_desc->offset & 0xFFF) << 12); \
	val32 |= ((p_desc->type & 0xFF) << 24); \
	}

/*
 * Define a maximum size for any configuration table.
 *
 * Use a value that will reasonably cover all the usable & programmable
 * registers in an ETM, the most complex device we have.
 *
 * This may also be used for the binary attributes in configfs which need a max
 * size, as an internal buffer is declared, and will not be exceeded to prevent
 * kernel OOM errors / attacks.
 *
 */
#define CSCFG_TABLE_MAXSIZE	SZ_16K

/* limit string sizes - used for descriptions and names. */
#define CSCFG_TABLE_STR_MAXSIZE	SZ_1K

/**
 * Table header.
 *
 * @magic_version: magic number / version for table format.
 * @length       : total length of all data in the table.
 * @nr_configs	 : total number of configs in the table.
 * @nr_features  : total number of features in the table.
 */
struct cscfg_table_header {
	u32 magic_version;
	u16 length;
	u16 nr_configs;
	u16 nr_features;
};

/**
 * element header
 *
 * @elem_length: total length of this element
 * @elem_type  : type of this element - one of CSCFG_TABLE_ELEM_TYPE.. defines.
 */
struct cscfg_table_elem_header {
	u16 elem_length;
	u8 elem_type;
};

/**
 * string table element.
 *
 * @str_len: length of string buffer including 0 terminator
 * @str    : string buffer - 0 terminated.
 */
struct cscfg_table_elem_str {
	u16 str_len;
	char *str;
};

/*
 * Read a configuration programming table from the buffer and create the
 * structures needed to load into the cscfg system
 */
int cscfg_table_read_buffer(const u8 *buffer, const int buflen,
			    struct cscfg_table_load_descs *desc_arrays);

/* on unload we need to free up memory allocated on read */
void cscfg_table_free_load_descs(struct cscfg_table_load_descs *desc_arrays);

#endif /* _CORESIGHT_CORESIGHT_CONFIG_TABLE_H */
