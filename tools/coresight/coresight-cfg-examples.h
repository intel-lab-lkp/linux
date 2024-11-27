/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2020-2022 Linaro Limited, All rights reserved.
 * Author: Mike Leach <mike.leach@linaro.org>
 */

#ifndef _CORESIGHT_CFG_EXAMPLES_H
#define _CORESIGHT_CFG_EXAMPLES_H

#include <linux/kernel.h>

#include "coresight-config-uapi.h"

/*
 * structure to pass C configuration structure information to
 * configuration table file generator program
 */
struct cscfg_file_eg_info {
	const char *example_name;
	const char *filename;
	struct cscfg_config_desc **config_descs;
	struct cscfg_feature_desc **feat_descs;
};

#endif /* _CORESIGHT_CFG_EXAMPLES_H */
