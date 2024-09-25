// SPDX-License-Identifier: GPL-2.0-only
/*
 * Alt-mode implementation for Thunderbolt on ChromeOS EC.
 *
 * Copyright 2024 Google LLC
 * Author: Abhishek Pandit-Subedi <abhishekpandit@chromium.org>
 */
#include "cros_ec_typec.h"

#include <linux/usb/typec_tbt.h>
#include <linux/usb/pd_vdo.h>

#include "cros_typec_altmode.h"

struct typec_tbt_data {
	struct work_struct work;

	struct cros_typec_port *port;
	struct typec_altmode *alt;

	u32 header;
	u32 *vdo_data;
	u8 vdo_size;
};

static void cros_typec_thunderbolt_work(struct work_struct *work)
{
	struct typec_tbt_data *data =
		container_of(work, struct typec_tbt_data, work);

	if (typec_altmode_vdm(data->alt, data->header, data->vdo_data,
			      data->vdo_size))
		dev_err(&data->alt->dev, "VDM 0x%x failed", data->header);

	data->header = 0;
	data->vdo_data = NULL;
	data->vdo_size = 0;
}

static int cros_typec_thunderbolt_enter(struct typec_altmode *alt, u32 *vdo)
{
	struct typec_tbt_data *data = typec_altmode_get_drvdata(alt);
	struct ec_params_typec_control req = {
		.port = data->port->port_num,
		.command = TYPEC_CONTROL_COMMAND_ENTER_MODE,
		.mode_to_enter = CROS_EC_ALTMODE_TBT,
	};
	int svdm_version;
	int ret;

	ret = cros_ec_cmd(data->port->typec_data->ec, 0, EC_CMD_TYPEC_CONTROL,
			  &req, sizeof(req), NULL, 0);
	if (ret < 0)
		return ret;

	svdm_version = typec_altmode_get_svdm_version(alt);
	if (svdm_version < 0)
		return svdm_version;

	data->header = VDO(USB_TYPEC_TBT_SID, 1, svdm_version, CMD_ENTER_MODE);
	data->header |= VDO_OPOS(TYPEC_TBT_MODE);
	data->header |= VDO_CMDT(CMDT_RSP_ACK);

	data->vdo_data = NULL;
	data->vdo_size = 1;

	schedule_work(&data->work);

	return ret;
}

static int cros_typec_thunderbolt_exit(struct typec_altmode *alt)
{
	struct typec_tbt_data *data = typec_altmode_get_drvdata(alt);
	struct ec_params_typec_control req = {
		.port = data->port->port_num,
		.command = TYPEC_CONTROL_COMMAND_EXIT_MODES,
	};
	int svdm_version;
	int ret;

	ret = cros_ec_cmd(data->port->typec_data->ec, 0, EC_CMD_TYPEC_CONTROL,
			  &req, sizeof(req), NULL, 0);

	if (ret < 0)
		return ret;

	svdm_version = typec_altmode_get_svdm_version(alt);
	if (svdm_version < 0)
		return svdm_version;

	data->header = VDO(USB_TYPEC_TBT_SID, 1, svdm_version, CMD_EXIT_MODE);
	data->header |= VDO_OPOS(TYPEC_TBT_MODE);
	data->header |= VDO_CMDT(CMDT_RSP_ACK);

	data->vdo_data = NULL;
	data->vdo_size = 1;

	schedule_work(&data->work);

	return ret;
}

static int cros_typec_thunderbolt_vdm(struct typec_altmode *alt, u32 header,
				      const u32 *data, int count)
{
	struct typec_tbt_data *tbt_data = typec_altmode_get_drvdata(alt);

	int cmd_type = PD_VDO_CMDT(header);
	int cmd = PD_VDO_CMD(header);
	int svdm_version;

	svdm_version = typec_altmode_get_svdm_version(alt);
	if (svdm_version < 0)
		return svdm_version;

	switch (cmd_type) {
	case CMDT_INIT:
		if (PD_VDO_SVDM_VER(header) < svdm_version) {
			typec_partner_set_svdm_version(tbt_data->port->partner,
						       PD_VDO_SVDM_VER(header));
			svdm_version = PD_VDO_SVDM_VER(header);
		}

		tbt_data->header = VDO(USB_TYPEC_TBT_SID, 1, svdm_version, cmd);
		tbt_data->header |= VDO_OPOS(TYPEC_TBT_MODE);

		/*
		 * TODO - Just always reply to the VDMs that we are done.
		 */
		switch (cmd) {
		case CMD_ENTER_MODE:
			/* Don't respond to the enter mode vdm because it
			 * triggers mux configuration. This is handled directly
			 * by the cros_ec_typec driver so the Thunderbolt driver
			 * doesn't need to be involved.
			 */
			break;
		default:
			tbt_data->header |= VDO_CMDT(CMDT_RSP_ACK);
			schedule_work(&tbt_data->work);
			break;
		}

		break;
	default:
		break;
	}

	return 0;
}

static const struct typec_altmode_ops cros_typec_thunderbolt_ops = {
	.enter = cros_typec_thunderbolt_enter,
	.exit = cros_typec_thunderbolt_exit,
	.vdm = cros_typec_thunderbolt_vdm,
};

struct typec_altmode *
cros_typec_register_thunderbolt(struct cros_typec_port *port,
				struct typec_altmode_desc *desc)
{
	struct typec_altmode *alt;
	struct typec_tbt_data *data;

	alt = typec_port_register_altmode(port->port, desc);
	if (IS_ERR(alt))
		return alt;

	data = devm_kzalloc(&alt->dev, sizeof(*data), GFP_KERNEL);
	if (!data) {
		typec_unregister_altmode(alt);
		return ERR_PTR(-ENOMEM);
	}

	INIT_WORK(&data->work, cros_typec_thunderbolt_work);
	data->alt = alt;
	data->port = port;

	typec_altmode_set_ops(alt, &cros_typec_thunderbolt_ops);
	typec_altmode_set_drvdata(alt, data);

	return alt;
}
