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
 * create a configuration only example using the strobing feature
 */

/* we will provide 4 sets of preset parameter values */
#define AFDO3_NR_PRESETS	4
/* the total number of parameters in used features - strobing has 2 */
#define AFDO3_NR_PARAM_SUM	2

static const char *afdo3_ref_names[] = {
	"strobing",
};

/*
 * set of presets leaves strobing window constant while varying period to allow
 * experimentation with mark / space ratios for various workloads
 */
static u64 afdo3_presets[AFDO3_NR_PRESETS][AFDO3_NR_PARAM_SUM] = {
	{ 2000, 100 },
	{ 2000, 1000 },
	{ 2000, 5000 },
	{ 2000, 10000 },
};

struct cscfg_config_desc afdo3 = {
	.name = "autofdo3",
	.description = "Setup ETMs with strobing for autofdo\n"
	"Supplied presets allow experimentation with mark-space ratio for various loads\n",
	.nr_feat_refs = ARRAY_SIZE(afdo3_ref_names),
	.feat_ref_names = afdo3_ref_names,
	.nr_presets = AFDO3_NR_PRESETS,
	.nr_total_params = AFDO3_NR_PARAM_SUM,
	.presets = &afdo3_presets[0][0],
};

static struct cscfg_feature_desc *sample_feats[] = {
	NULL
};

static struct cscfg_config_desc *sample_cfgs[] = {
	&afdo3,
	NULL
};

struct cscfg_file_eg_info file_info_eg1 = {
	.example_name = "example1",
	.filename = "example1.cscfg",
	.config_descs = sample_cfgs,
	.feat_descs = sample_feats,
};
