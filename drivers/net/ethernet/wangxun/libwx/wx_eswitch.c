// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2019-2021, Intel Corporation. */

#include <linux/pci.h>

#include "wx_type.h"
#include "wx_eswitch.h"
#include "wx_devlink.h"

int wx_eswitch_mode_set(struct devlink *devlink, u16 mode,
			struct netlink_ext_ack *extack)
{
	struct wx_dl_priv *dl_priv = devlink_priv(devlink);
	struct wx *wx = dl_priv->priv_wx;

	if (wx->eswitch_mode == mode)
		return 0;

	if (wx->num_vfs) {
		dev_info(&(wx)->pdev->dev,
			 "Change eswitch mode is allowed if there is no VFs.");
		return -EOPNOTSUPP;
	}

	switch (mode) {
	case DEVLINK_ESWITCH_MODE_LEGACY:
		dev_info(&(wx)->pdev->dev,
			 "PF%d changed eswitch mode to legacy",
			 wx->bus.func);
		NL_SET_ERR_MSG_MOD(extack, "Changed eswitch mode to legacy");
		break;
	case DEVLINK_ESWITCH_MODE_SWITCHDEV:
		dev_info(&(wx)->pdev->dev,
			 "Do not support switchdev in eswitch mode.");
		NL_SET_ERR_MSG_MOD(extack, "Do not support switchdev mode.");
		return -EINVAL;
	default:
		NL_SET_ERR_MSG_MOD(extack, "Unknown eswitch mode");
		return -EINVAL;
	}

	wx->eswitch_mode = mode;
	return 0;
}

int wx_eswitch_mode_get(struct devlink *devlink, u16 *mode)
{
	struct wx_dl_priv *dl_priv = devlink_priv(devlink);
	struct wx *wx = dl_priv->priv_wx;

	*mode = wx->eswitch_mode;
	return 0;
}
