// SPDX-License-Identifier: GPL-2.0-only
/*
 * Alt-mode implementation for Displayport on ChromeOS EC.
 *
 * Copyright 2024 Google LLC
 * Author: Abhishek Pandit-Subedi <abhishekpandit@chromium.org>
 */
#include "cros_ec_typec.h"

#include <linux/usb/typec_dp.h>
#include <linux/usb/pd_vdo.h>

#include "cros_typec_altmode.h"

struct typec_dp_data {
	struct typec_displayport_data data;
	struct work_struct work;

	struct cros_typec_port *port;
	struct typec_altmode *alt;
	bool ap_mode_entry;
	bool configured;
	bool pending_status_update;

	u32 header;
	u32 *vdo_data;
	u8 vdo_size;
};

static void cros_typec_displayport_work(struct work_struct *work)
{
	struct typec_dp_data *data =
		container_of(work, struct typec_dp_data, work);

	if (typec_altmode_vdm(data->alt, data->header, data->vdo_data,
			      data->vdo_size))
		dev_err(&data->alt->dev, "VDM 0x%x failed", data->header);

	data->header = 0;
	data->vdo_data = NULL;
	data->vdo_size = 0;
}

static int cros_typec_displayport_enter(struct typec_altmode *alt, u32 *vdo)
{
	struct typec_dp_data *data = typec_altmode_get_drvdata(alt);
	struct ec_params_typec_control req = {
		.port = data->port->port_num,
		.command = TYPEC_CONTROL_COMMAND_ENTER_MODE,
		.mode_to_enter = CROS_EC_ALTMODE_DP,
	};
	int svdm_version;
	int ret;

	if (!data->ap_mode_entry) {
		const struct typec_altmode *partner =
			typec_altmode_get_partner(alt);
		dev_warn(&partner->dev,
			 "EC does not support ap driven mode entry\n");
		return -EOPNOTSUPP;
	}

	ret = cros_ec_cmd(data->port->typec_data->ec, 0, EC_CMD_TYPEC_CONTROL,
			  &req, sizeof(req), NULL, 0);
	if (ret < 0)
		return ret;

	svdm_version = typec_altmode_get_svdm_version(alt);
	if (svdm_version < 0)
		return svdm_version;

	data->header = VDO(USB_TYPEC_DP_SID, 1, svdm_version, CMD_ENTER_MODE);
	data->header |= VDO_OPOS(USB_TYPEC_DP_MODE);
	data->header |= VDO_CMDT(CMDT_RSP_ACK);

	data->vdo_data = NULL;
	data->vdo_size = 1;

	schedule_work(&data->work);

	return ret;
}

static int cros_typec_displayport_exit(struct typec_altmode *alt)
{
	struct typec_dp_data *data = typec_altmode_get_drvdata(alt);
	struct ec_params_typec_control req = {
		.port = data->port->port_num,
		.command = TYPEC_CONTROL_COMMAND_EXIT_MODES,
	};
	int svdm_version;
	int ret;

	if (!data->ap_mode_entry) {
		const struct typec_altmode *partner =
			typec_altmode_get_partner(alt);
		dev_warn(&partner->dev,
			 "EC does not support ap driven mode entry\n");
		return -EOPNOTSUPP;
	}

	ret = cros_ec_cmd(data->port->typec_data->ec, 0, EC_CMD_TYPEC_CONTROL,
			  &req, sizeof(req), NULL, 0);

	if (ret < 0)
		return ret;

	svdm_version = typec_altmode_get_svdm_version(alt);
	if (svdm_version < 0)
		return svdm_version;

	data->header = VDO(USB_TYPEC_DP_SID, 1, svdm_version, CMD_EXIT_MODE);
	data->header |= VDO_OPOS(USB_TYPEC_DP_MODE);
	data->header |= VDO_CMDT(CMDT_RSP_ACK);

	data->vdo_data = NULL;
	data->vdo_size = 1;

	schedule_work(&data->work);

	return ret;
}

int cros_typec_displayport_status_update(struct typec_altmode *altmode,
					 struct typec_displayport_data *data)
{
	struct typec_dp_data *dp_data = typec_altmode_get_drvdata(altmode);

	if (!dp_data->pending_status_update) {
		dev_dbg(&altmode->dev,
			"Got DPStatus without a pending request");
		return 0;
	}

	if (dp_data->configured && dp_data->data.conf != data->conf)
		dev_dbg(&altmode->dev,
			"DP Conf doesn't match. Requested 0x%04x, Actual 0x%04x",
			dp_data->data.conf, data->conf);

	dp_data->header |= VDO_CMDT(CMDT_RSP_ACK);
	dp_data->data = *data;
	dp_data->vdo_data = &dp_data->data.status;
	dp_data->vdo_size = 2;
	dp_data->pending_status_update = false;

	schedule_work(&dp_data->work);
	return 0;
}

static int cros_typec_displayport_vdm(struct typec_altmode *alt, u32 header,
				      const u32 *data, int count)
{
	struct typec_dp_data *dp_data = typec_altmode_get_drvdata(alt);

	int cmd_type = PD_VDO_CMDT(header);
	int cmd = PD_VDO_CMD(header);
	int svdm_version;

	if (!dp_data->ap_mode_entry) {
		const struct typec_altmode *partner =
			typec_altmode_get_partner(alt);
		dev_warn(&partner->dev,
			 "EC does not support ap driven mode entry\n");
		return -EOPNOTSUPP;
	}

	svdm_version = typec_altmode_get_svdm_version(alt);
	if (svdm_version < 0)
		return svdm_version;

	switch (cmd_type) {
	case CMDT_INIT:
		if (PD_VDO_SVDM_VER(header) < svdm_version) {
			typec_partner_set_svdm_version(dp_data->port->partner,
						       PD_VDO_SVDM_VER(header));
			svdm_version = PD_VDO_SVDM_VER(header);
		}

		dp_data->header = VDO(USB_TYPEC_DP_SID, 1, svdm_version, cmd);
		dp_data->header |= VDO_OPOS(USB_TYPEC_DP_MODE);

		/*
		 * DP_CMD_CONFIGURE: We can't actually do anything with the
		 * provided VDO yet so just send back an ACK.
		 *
		 * DP_CMD_STATUS_UPDATE: We wait for Mux changes to send
		 * DPStatus Acks.
		 */
		switch (cmd) {
		case DP_CMD_CONFIGURE:
			dp_data->data.conf = *data;
			dp_data->header |= VDO_CMDT(CMDT_RSP_ACK);
			dp_data->configured = true;
			schedule_work(&dp_data->work);
			break;
		case DP_CMD_STATUS_UPDATE:
			dp_data->pending_status_update = true;
			break;
		default:
			dp_data->header |= VDO_CMDT(CMDT_RSP_ACK);
			schedule_work(&dp_data->work);
			break;
		}

		break;
	default:
		break;
	}

	return 0;
}

static const struct typec_altmode_ops cros_typec_displayport_ops = {
	.enter = cros_typec_displayport_enter,
	.exit = cros_typec_displayport_exit,
	.vdm = cros_typec_displayport_vdm,
};

struct typec_altmode *
cros_typec_register_displayport(struct cros_typec_port *port,
				struct typec_altmode_desc *desc,
				bool ap_mode_entry)
{
	struct typec_altmode *alt;
	struct typec_dp_data *data;

	alt = typec_port_register_altmode(port->port, desc);
	if (IS_ERR(alt))
		return alt;

	data = devm_kzalloc(&alt->dev, sizeof(*data), GFP_KERNEL);
	if (!data) {
		typec_unregister_altmode(alt);
		return ERR_PTR(-ENOMEM);
	}

	INIT_WORK(&data->work, cros_typec_displayport_work);
	data->alt = alt;
	data->port = port;
	data->ap_mode_entry = ap_mode_entry;
	data->configured = false;

	typec_altmode_set_ops(alt, &cros_typec_displayport_ops);
	typec_altmode_set_drvdata(alt, data);

	return alt;
}
