// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2017 - 2019 Pensando Systems, Inc */

#include <linux/module.h>
#include <linux/netdevice.h>

#include "ionic.h"
#include "ionic_bus.h"
#include "ionic_lif.h"
#include "ionic_devlink.h"
#include "ionic_aux.h"

static int ionic_dl_flash_update(struct devlink *dl,
				 struct devlink_flash_update_params *params,
				 struct netlink_ext_ack *extack)
{
	struct ionic *ionic = devlink_priv(dl);

	return ionic_firmware_update(ionic->lif, params->fw, extack);
}

static int ionic_dl_info_get(struct devlink *dl, struct devlink_info_req *req,
			     struct netlink_ext_ack *extack)
{
	struct ionic *ionic = devlink_priv(dl);
	struct ionic_dev *idev = &ionic->idev;
	char buf[16];
	int err = 0;

	err = devlink_info_version_running_put(req,
					       DEVLINK_INFO_VERSION_GENERIC_FW,
					       idev->dev_info.fw_version);
	if (err)
		return err;

	snprintf(buf, sizeof(buf), "0x%x", idev->dev_info.asic_type);
	err = devlink_info_version_fixed_put(req,
					     DEVLINK_INFO_VERSION_GENERIC_ASIC_ID,
					     buf);
	if (err)
		return err;

	snprintf(buf, sizeof(buf), "0x%x", idev->dev_info.asic_rev);
	err = devlink_info_version_fixed_put(req,
					     DEVLINK_INFO_VERSION_GENERIC_ASIC_REV,
					     buf);
	if (err)
		return err;

	err = devlink_info_serial_number_put(req, idev->dev_info.serial_num);

	return err;
}

static const struct devlink_ops ionic_dl_ops = {
	.info_get	= ionic_dl_info_get,
	.flash_update	= ionic_dl_flash_update,
};

static bool is_aux_enabled(struct ionic *ionic)
{
	return !!ionic->lif->ionic_adev;
}

static int ionic_devlink_enable_rdma_get(struct devlink *dl, u32 id,
					 struct devlink_param_gset_ctx *ctx,
					 struct netlink_ext_ack *extack)
{
	ctx->val.vbool = is_aux_enabled(devlink_priv(dl));
	return 0;
}

static int ionic_devlink_enable_rdma_set(struct devlink *dl, u32 id,
					 struct devlink_param_gset_ctx *ctx,
					 struct netlink_ext_ack *extack)
{
	struct ionic *ionic = devlink_priv(dl);
	int err = 0;

	if (ctx->val.vbool == is_aux_enabled(ionic))
		return err;

	if (ctx->val.vbool)
		err = ionic_auxbus_register(ionic->lif);
	else
		ionic_auxbus_unregister(ionic->lif);

	return err;
}

static int ionic_devlink_enable_rdma_validate(struct devlink *dl, u32 id,
					      union devlink_param_value val,
					      struct netlink_ext_ack *extack)
{
	struct ionic *ionic = devlink_priv(dl);
	bool new_state = val.vbool;

	if (new_state &&
	    !(le64_to_cpu(ionic->ident.lif.capabilities) & IONIC_LIF_CAP_RDMA))
		return -EOPNOTSUPP;

	return 0;
}

static const struct devlink_param ionic_dl_rdma_params[] = {
	DEVLINK_PARAM_GENERIC(ENABLE_RDMA, BIT(DEVLINK_PARAM_CMODE_RUNTIME),
			      ionic_devlink_enable_rdma_get, ionic_devlink_enable_rdma_set,
			      ionic_devlink_enable_rdma_validate),
};

static int ionic_dl_rdma_params_register(struct devlink *dl)
{
	if (!IS_ENABLED(CONFIG_INFINIBAND_IONIC))
		return 0;

	return devlink_params_register(dl, ionic_dl_rdma_params,
				       ARRAY_SIZE(ionic_dl_rdma_params));
}

static void ionic_devlink_rdma_params_unregister(struct devlink *dl)
{
	devlink_params_unregister(dl, ionic_dl_rdma_params,
				  ARRAY_SIZE(ionic_dl_rdma_params));
}

struct ionic *ionic_devlink_alloc(struct device *dev)
{
	struct devlink *dl;

	dl = devlink_alloc(&ionic_dl_ops, sizeof(struct ionic), dev);
	if (!dl)
		return NULL;

	return devlink_priv(dl);
}

void ionic_devlink_free(struct ionic *ionic)
{
	struct devlink *dl = priv_to_devlink(ionic);

	devlink_free(dl);
}

int ionic_devlink_register(struct ionic *ionic)
{
	struct devlink *dl = priv_to_devlink(ionic);
	struct devlink_port_attrs attrs = {};
	int err;

	attrs.flavour = DEVLINK_PORT_FLAVOUR_PHYSICAL;
	devlink_port_attrs_set(&ionic->dl_port, &attrs);

	err = ionic_dl_rdma_params_register(dl);
	if (err) {
		dev_err(ionic->dev, "ionic_dl_rdma_params_register failed: %d\n", err);
		return err;
	}

	err = devlink_port_register(dl, &ionic->dl_port, 0);
	if (err) {
		dev_err(ionic->dev, "devlink_port_register failed: %d\n", err);
		ionic_devlink_rdma_params_unregister(dl);
		return err;
	}

	SET_NETDEV_DEVLINK_PORT(ionic->lif->netdev, &ionic->dl_port);
	devlink_register(dl);
	return 0;
}

void ionic_devlink_unregister(struct ionic *ionic)
{
	struct devlink *dl = priv_to_devlink(ionic);

	devlink_unregister(dl);
	devlink_port_unregister(&ionic->dl_port);
}
