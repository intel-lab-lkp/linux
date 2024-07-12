/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AW_SAR_H_
#define _AW_SAR_H_
#include "aw_sar_type.h"

enum aw_sar_driver_list_t {
	AW_SAR_AW9610X,
	AW_SAR_AW963XX,

	AW_SAR_DRIVER_MAX,
};

int aw9610x_check_chipid(void *data);
int aw9610x_init(struct aw_sar *p_sar);
void aw9610x_deinit(struct aw_sar *p_sar);

int aw963xx_check_chipid(void *data);
int aw963xx_init(struct aw_sar *p_sar);
void aw963xx_deinit(struct aw_sar *p_sar);



#endif
