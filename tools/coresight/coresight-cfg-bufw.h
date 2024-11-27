/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2020-2022 Linaro Limited, All rights reserved.
 * Author: Mike Leach <mike.leach@linaro.org>
 */

#ifndef _CORESIGHT_CFG_BUFW_H
#define _CORESIGHT_CFG_BUFW_H

#include <linux/types.h>

#include "coresight-config-table.h"

/*
 * Function to take coresight configurations and features and
 * write them into a supplied memory buffer for serialisation
 * into a file.
 *
 * Resulting file can then be loaded into the coresight
 * infrastructure via configfs.
 */
int cscfg_table_write_buffer(u8 *buffer, const int buflen,
			     struct cscfg_config_desc **config_descs,
			     struct cscfg_feature_desc **feat_descs);

#endif /* _CORESIGHT_CFG_BUFW_H */
