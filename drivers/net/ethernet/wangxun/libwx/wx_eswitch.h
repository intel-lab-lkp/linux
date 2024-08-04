/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2019-2021, Intel Corporation. */

#ifndef _WX_ESWITCH_H_
#define _WX_ESWITCH_H_

#include <net/devlink.h>

int wx_eswitch_mode_get(struct devlink *devlink, u16 *mode);
int wx_eswitch_mode_set(struct devlink *devlink, u16 mode,
			struct netlink_ext_ack *extack);

#endif /* _WX_ESWITCH_H_ */
