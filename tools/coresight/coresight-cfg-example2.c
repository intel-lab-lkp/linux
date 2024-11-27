// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020-2022 Linaro Limited, All rights reserved.
 * Author: Mike Leach <mike.leach@linaro.org>
 */
#include <linux/types.h>
#include <linux/unistd.h>
#include <stdio.h>
#include <unistd.h>

#include "coresight-cfg-examples.h"

/*
 * create a dual configuration only example using the strobing feature
 */

/* we will provide 10 sets of preset parameter values */
#define AFDO_NR_PRESETS		10
/* the total number of parameters in used features - strobing has 2 */
#define AFDO_NR_PARAM_SUM	2

static const char *afdo_ref_names[] = {
	"strobing",
};

/*
 * sets of presets leaves strobing window constant while varying period to allow
 * experimentation with mark / space ratios for various workloads
 */
static u64 afdo_set_a_presets[AFDO_NR_PRESETS][AFDO_NR_PARAM_SUM] = {
	{ 2000, 100 },
	{ 2000, 1000 },
	{ 2000, 5000 },
	{ 2000, 10000 },
	{ 4000, 100 },
	{ 4000, 1000 },
	{ 4000, 5000 },
	{ 4000, 10000 },
	{ 6000, 100 },
	{ 6000, 1000 },
};


static u64 afdo_set_b_presets[AFDO_NR_PRESETS][AFDO_NR_PARAM_SUM] = {
	{ 6000, 5000 },
	{ 6000, 10000 },
	{ 8000, 100 },
	{ 8000, 1000 },
	{ 8000, 5000 },
	{ 8000, 10000 },
	{ 12000, 100 },
	{ 12000, 1000 },
	{ 12000, 5000 },
	{ 12000, 10000 },
};
/* two configurations with differing preset tables */
struct cscfg_config_desc afdo_seta = {
	.name = "autofdo_set_a",
	.description = "Setup ETMs with strobing for autofdo\n"
	"Supplied presets allow experimentation with mark-space ratio for various loads\n",
	.nr_feat_refs = ARRAY_SIZE(afdo_ref_names),
	.feat_ref_names = afdo_ref_names,
	.nr_presets = AFDO_NR_PRESETS,
	.nr_total_params = AFDO_NR_PARAM_SUM,
	.presets = &afdo_set_a_presets[0][0],
};

struct cscfg_config_desc afdo_setb = {
	.name = "autofdo_set_b",
	.description = "Setup ETMs with strobing for autofdo\n"
	"Supplied presets allow experimentation with mark-space ratio for various loads\n",
	.nr_feat_refs = ARRAY_SIZE(afdo_ref_names),
	.feat_ref_names = afdo_ref_names,
	.nr_presets = AFDO_NR_PRESETS,
	.nr_total_params = AFDO_NR_PARAM_SUM,
	.presets = &afdo_set_b_presets[0][0],
};


static struct cscfg_feature_desc *sample_feats[] = {
	NULL
};

static struct cscfg_config_desc *sample_cfgs[] = {
	&afdo_seta,
	&afdo_setb,
	NULL
};

struct cscfg_file_eg_info file_info_eg2 = {
	.example_name = "example2",
	.filename = "example2.cscfg",
	.config_descs = sample_cfgs,
	.feat_descs = sample_feats,
};
