/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2015 - 2024 Beijing WangXun Technology Co., Ltd. */

#ifndef _WX_DEVLINK_H_
#define _WX_DEVLINK_H_

struct wx_dl_priv *wx_create_devlink(struct device *dev);
int wx_devlink_create_pf_port(struct wx *wx);
void wx_devlink_destroy_pf_port(struct wx *wx);
int wx_devlink_create_vf_port(struct wx *wx, int vf_id);
void wx_devlink_destroy_vf_port(struct wx *wx);

#endif /* _WX_DEVLINK_H_ */
