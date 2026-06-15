// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2025-2026, ITE. All Rights Reserved
 */
#include <linux/unaligned.h>
#include <linux/auxiliary_bus.h>
#include <linux/bitfield.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/workqueue.h>
#include <linux/usb/typec.h>
#include <linux/usb/typec_dp.h>
#include <linux/slab.h>

#include "itepd.h"
#include "ucsi.h"

struct ucsi_itepd {
	struct device *dev;
	struct ucsi *ucsi;
	struct completion complete;
	struct mutex received_lock; /* Protects command response data:*/
	struct workqueue_struct *ordered_wq;
	bool connected[ITEPD_MAX_PORTS];
	bool dp_en[ITEPD_MAX_PORTS];
	bool con_change_processed[ITEPD_MAX_PORTS];
	u8 cmd_port;
	u8 resp_received;
	u8 msg_in[0x28];
	u8 dp_idx[ITEPD_MAX_PORTS];
	u8 orientation[ITEPD_MAX_PORTS];
	u8 mux[ITEPD_MAX_PORTS];
	u32 cci;
	u64 cmd;
};

struct ucsi_itepd_work {
	struct ucsi_itepd *ucsi_itepd;
	u8 port;
	u32 cci;
	struct work_struct work;
};

static struct ucsi_itepd *__ucsi_itepd;

static void ucsi_itepd_handle_dp_altmode(struct ucsi_itepd *ucsi_itepd,
					 u8 port, u8 *mux, u32 *config, u32 *status)
{
	u8 orientation = ucsi_itepd->orientation[port];
	u8 data[10];
	u64 cmd;
	int ret;

	cmd = UCSI_COMMAND(UCSI_GET_CURRENT_CAM) | UCSI_CONNECTOR_NUMBER(port + 1);
	ret = ucsi_send_command(ucsi_itepd->ucsi, cmd, data, 1);
	if (ret < 0)
		return;

	if (data[0] != ucsi_itepd->dp_idx[port])
		return;

	cmd = UCSI_COMMAND(UCSI_GET_CAM_CS) |
	      UCSI_CONNECTOR_NUMBER(port + 1) |
	      ((u64)(ucsi_itepd->dp_idx[port]) << 24);
	ret = ucsi_send_command(ucsi_itepd->ucsi, cmd, data, 10);
	if (ret < 0)
		return;

	*config = get_unaligned_le32(data + 6);
	*status = get_unaligned_le32(data + 1);

	if (DP_CONF_GET_PIN_ASSIGN(*config) == BIT(DP_PIN_ASSIGN_C)) {
		*mux = (orientation == 1) ? ITEPD_USBPD_MUX_DP_1 : ITEPD_USBPD_MUX_DP_0;
		ucsi_itepd->dp_en[port] = true;
	} else if (DP_CONF_GET_PIN_ASSIGN(*config) == BIT(DP_PIN_ASSIGN_D)) {
		*mux = (orientation == 1) ? ITEPD_USBPD_MUX_USB_DP_1 : ITEPD_USBPD_MUX_USB_DP_0;
		ucsi_itepd->dp_en[port] = true;
	}
}

static void ucsi_itepd_connector_partner_change(struct ucsi_itepd *ucsi_itepd,
						u8 port, struct ucsi_connector *con)
{
	u8 orientation = ucsi_itepd->orientation[port];
	u8 mux = ucsi_itepd->mux[port];
	u32 config = 0;
	u32 status = 0;

	if (!(UCSI_CONSTAT(con, CHANGE) &
	      (UCSI_CONSTAT_PARTNER_CHANGE | UCSI_CONSTAT_CONNECT_CHANGE)))
		return;

	if (!UCSI_CONSTAT(con, CONNECTED)) {
		mux = ITEPD_USBPD_MUX_OFF;
		ucsi_itepd->dp_en[port] = false;
	} else if (UCSI_CONSTAT(con, PARTNER_FLAG_ALT_MODE)) {
		ucsi_itepd_handle_dp_altmode(ucsi_itepd, port, &mux, &config, &status);
	} else {
		mux = (orientation == 1) ? ITEPD_USBPD_MUX_USB_1 : ITEPD_USBPD_MUX_USB_0;
		ucsi_itepd->dp_en[port] = false;
	}

	if (mux != ucsi_itepd->mux[port]) {
		itepd_mode(ucsi_itepd->dev, port, mux, config, status);
		ucsi_itepd->mux[port] = mux;
	}
}

static void ucsi_itepd_connector_change_work(struct work_struct *work)
{
	struct ucsi_itepd_work *worker = container_of(work, struct ucsi_itepd_work, work);
	struct ucsi_itepd *ucsi_itepd = worker->ucsi_itepd;
	u8 data[11];
	u8 num_vdos;
	u32 status;
	u64 cmd;
	int ret;

	if (ucsi_itepd->con_change_processed[worker->port])
		goto out;

	ucsi_itepd->con_change_processed[worker->port] = true;
	if (ucsi_itepd->dp_en[worker->port]) {
		/* UCSI_GET_ATTENTION_VDO (0x16) */
		cmd = UCSI_COMMAND(UCSI_GET_ATTENTION_VDO) |
		      UCSI_CONNECTOR_NUMBER(worker->port + 1);
		ret = ucsi_send_command(ucsi_itepd->ucsi, cmd, data, 11);
	}
	if (ret < 0)
		goto out;
	num_vdos = data[2] & 0x07;
	status = get_unaligned_le32(data + 7);

	if (num_vdos)
		itepd_hpd(ucsi_itepd->dev, worker->port, status);

	ucsi_connector_change(ucsi_itepd->ucsi, UCSI_CCI_CONNECTOR(worker->cci));
out:
	kfree(worker);
}

static void ucsi_itepd_command_hook(struct ucsi_itepd *ucsi_itepd, u64 *cmd)
{
	/* Translate UCSI 1.2 commands/fields to ITE PD controller (v2.1) */
	switch (UCSI_COMMAND(*cmd)) {
	case UCSI_SET_NOTIFICATION_ENABLE:
		if (*cmd & UCSI_ENABLE_NTFY_CMD_COMPLETE)
			/* Enable Attention Notification for alt. mode */
			*cmd |= FIELD_PREP(GENMASK_ULL(32, 16), BIT(3));
		break;
	case UCSI_GET_PDOS:
		*cmd &= ~GENMASK_ULL(38, 37);
		break;
	case UCSI_GET_ERROR_STATUS:
		*cmd &= ~GENMASK_ULL(22, 16);
		*cmd |= UCSI_CONNECTOR_NUMBER(ucsi_itepd->cmd_port + 1);
		break;
	default:
		break;
	}

	/* Track the connector number associated with this command */
	switch (UCSI_COMMAND(*cmd)) {
	case UCSI_PPM_RESET:
	case UCSI_CANCEL:
	case UCSI_SET_NOTIFICATION_ENABLE:
	case UCSI_GET_CAPABILITY:
		ucsi_itepd->cmd_port = 0;
		break;
	case UCSI_CONNECTOR_RESET:
	case UCSI_GET_CONNECTOR_CAPABILITY:
	case UCSI_SET_CCOM:		/* 0x08 - SET_UOM in older specs */
	case UCSI_SET_UOR:
	case UCSI_SET_PDR:
	case UCSI_GET_CAM_SUPPORTED:
	case UCSI_GET_CURRENT_CAM:
	case UCSI_SET_NEW_CAM:
	case UCSI_GET_PDOS:
	case UCSI_GET_CABLE_PROPERTY:
	case UCSI_GET_CONNECTOR_STATUS:
	case UCSI_SET_POWER_LEVEL:	/* 0x14 */
	case UCSI_GET_PD_MESSAGE:	/* 0x15 */
	case UCSI_GET_ATTENTION_VDO:	/* 0x16 */
	case UCSI_GET_CAM_CS:		/* 0x18 */
	case 0x19:
	case 0x1A:
	case 0x1B:
	case UCSI_SET_SINK_PATH:	/* 0x1C */
	case 0x1D:
	case UCSI_READ_POWER_LEVEL:	/* 0x1E */
	case 0x1F:
		ucsi_itepd->cmd_port =
			FIELD_GET(GENMASK(22, 16), *cmd) - 1;
		break;
	case UCSI_GET_ALTERNATE_MODES:
		ucsi_itepd->cmd_port =
			FIELD_GET(GENMASK(30, 24), *cmd) - 1;
		break;
	}

	ucsi_itepd->cmd = *cmd;
}

static void ucsi_itepd_response_hook(struct ucsi_itepd *ucsi_itepd,
				     u32 *cci, u8 *msg_in)
{
	u8 recipient;
	u8 offset;
	struct ucsi_itepd_work *worker;

	if (((*cci & UCSI_CCI_COMMAND_COMPLETE) == 0) &&
	    UCSI_CCI_CONNECTOR(*cci)) {
		worker = kmalloc_obj(*worker, GFP_KERNEL);
		if (!worker) {
			dev_err(ucsi_itepd->dev,
				"out of memory, skip attention check\n");
			ucsi_connector_change(ucsi_itepd->ucsi,
					      UCSI_CCI_CONNECTOR(*cci));
		} else {
			worker->port = UCSI_CCI_CONNECTOR(*cci) - 1;
			worker->ucsi_itepd = ucsi_itepd;
			worker->cci = *cci;

			INIT_WORK(&worker->work,
				  ucsi_itepd_connector_change_work);
			queue_work(ucsi_itepd->ordered_wq, &worker->work);
		}
	}

	if ((*cci & UCSI_CCI_COMMAND_COMPLETE) &&
	    ((*cci & UCSI_CCI_ERROR) == 0)) {
		switch (UCSI_COMMAND(ucsi_itepd->cmd)) {
		case UCSI_GET_CONNECTOR_STATUS:
			ucsi_itepd->connected[ucsi_itepd->cmd_port] =
				!!(msg_in[2] & BIT(3));
			ucsi_itepd->orientation[ucsi_itepd->cmd_port] =
				FIELD_GET(BIT(6), msg_in[10]);
			break;

		case UCSI_GET_ALTERNATE_MODES:
			recipient = FIELD_GET(GENMASK_ULL(18, 16),
					      ucsi_itepd->cmd);
			if (recipient == UCSI_RECIPIENT_CON) {
				offset = FIELD_GET(GENMASK_ULL(39, 32),
						   ucsi_itepd->cmd);
				if (((struct ucsi_altmode *)msg_in)->svid ==
				    USB_TYPEC_DP_SID) {
					ucsi_itepd->dp_idx[ucsi_itepd->cmd_port] =
						offset;
				} else if (((struct ucsi_altmode *)
					    (msg_in + 6))->svid ==
					   USB_TYPEC_DP_SID) {
					ucsi_itepd->dp_idx[ucsi_itepd->cmd_port] =
						offset + 1;
				}
			}
			break;
		default:
			break;
		}
	}
}

/*
 * ITE PD notify callback
 */
static u8 ucsi_itepd_get_len(void *priv, u32 cci)
{
	if (cci & UCSI_CCI_COMMAND_COMPLETE)
		return UCSI_CCI_LENGTH(cci);
	return 0;
}

static void ucsi_itepd_notify(void *priv, u32 cci, u8 *data)
{
	struct ucsi_itepd *ucsi_itepd = (struct ucsi_itepd *)priv;
	bool comp = false;
	u8 msg_in[0x28];
	u8 len = UCSI_CCI_LENGTH(cci);

	memcpy(msg_in, data, min_t(u8, len, ARRAY_SIZE(msg_in)));
	ucsi_itepd_response_hook(ucsi_itepd, &cci, msg_in);

	mutex_lock(&ucsi_itepd->received_lock);
	if (cci & UCSI_CCI_COMMAND_COMPLETE) {
		ucsi_itepd->resp_received = 1;
		ucsi_itepd->cci = cci;
		memset(ucsi_itepd->msg_in, 0, ARRAY_SIZE(ucsi_itepd->msg_in));
		if (len)
			memcpy(ucsi_itepd->msg_in, msg_in,
			       min_t(u8, len, ARRAY_SIZE(ucsi_itepd->msg_in)));
		comp = true;
	}
	if (cci & UCSI_CCI_RESET_COMPLETE) {
		ucsi_itepd->resp_received = 1;
		ucsi_itepd->cci = cci;
		memset(ucsi_itepd->msg_in, 0, ARRAY_SIZE(ucsi_itepd->msg_in));
		comp = true;
	}
	mutex_unlock(&ucsi_itepd->received_lock);

	if (cci & UCSI_CCI_ACK_COMPLETE)
		comp = true;

	if (comp)
		complete(&ucsi_itepd->complete);
}

/*
 * New ucsi_operations implementation
 */

static int ucsi_itepd_read_version(struct ucsi *ucsi, u16 *version)
{
	struct ucsi_itepd *ucsi_itepd = ucsi_get_drvdata(ucsi);

	return itepd_cmd_receive(ucsi_itepd->dev,
				 ITEPD_RECEIVE_UCSI_VERSION,
				 version, sizeof(*version));
}

static int ucsi_itepd_read_cci(struct ucsi *ucsi, u32 *cci)
{
	struct ucsi_itepd *ucsi_itepd = ucsi_get_drvdata(ucsi);

	mutex_lock(&ucsi_itepd->received_lock);
	if (ucsi_itepd->resp_received) {
		ucsi_itepd->resp_received = 0;
		*cci = ucsi_itepd->cci;
	} else {
		*cci = 0;
	}
	mutex_unlock(&ucsi_itepd->received_lock);

	return 0;
}

/*
 * poll_cci: called when notifications are temporarily disabled (e.g. during
 * PPM reset).  For this hardware we can reuse read_cci — the firmware always
 * pushes CCI updates via the notify callback regardless.
 */
static int ucsi_itepd_poll_cci(struct ucsi *ucsi, u32 *cci)
{
	return ucsi_itepd_read_cci(ucsi, cci);
}

static int ucsi_itepd_read_message_in(struct ucsi *ucsi,
				      void *val, size_t val_len)
{
	struct ucsi_itepd *ucsi_itepd = ucsi_get_drvdata(ucsi);

	mutex_lock(&ucsi_itepd->received_lock);
	memcpy(val, ucsi_itepd->msg_in,
	       min(val_len, ARRAY_SIZE(ucsi_itepd->msg_in)));
	mutex_unlock(&ucsi_itepd->received_lock);

	return 0;
}

/*
 * async_control: fire a command to the PPM and return immediately.
 * The old async_write(UCSI_CONTROL, …) path, now receiving the raw
 * u64 command directly.
 */
static int ucsi_itepd_async_control(struct ucsi *ucsi, u64 command)
{
	struct ucsi_itepd *ucsi_itepd = ucsi_get_drvdata(ucsi);

	ucsi_itepd_command_hook(ucsi_itepd, &command);

	mutex_lock(&ucsi_itepd->received_lock);
	ucsi_itepd->resp_received = 0;
	mutex_unlock(&ucsi_itepd->received_lock);

	return itepd_cmd_send(ucsi_itepd->dev,
			      ITEPD_SEND_UCSI_CONTROL,
			      &command, sizeof(command));
}

/*
 * sync_control: blocking command — send and wait for completion.
 * On success the caller gets cci and data filled in.
 */
static int ucsi_itepd_sync_control(struct ucsi *ucsi, u64 command,
				   u32 *cci, void *data, size_t size)
{
	struct ucsi_itepd *ucsi_itepd = ucsi_get_drvdata(ucsi);
	int ret;

	reinit_completion(&ucsi_itepd->complete);

	ret = ucsi_itepd_async_control(ucsi, command);
	if (ret)
		return ret;

	if (!wait_for_completion_timeout(&ucsi_itepd->complete,
					 msecs_to_jiffies(5000)))
		return -ETIMEDOUT;

	/* Hand back CCI and (optionally) message data to the core */
	if (cci) {
		mutex_lock(&ucsi_itepd->received_lock);
		*cci = ucsi_itepd->cci;
		mutex_unlock(&ucsi_itepd->received_lock);
	}
	if (data && size)
		ucsi_itepd_read_message_in(ucsi, data, size);

	return 0;
}

static bool ucsi_itepd_update_altmodes(struct ucsi *ucsi, u8 recipient,
				       struct ucsi_altmode *orig, struct ucsi_altmode *updated)
{
	/* No altmode squashing needed for this hardware */
	return false;
}

static void ucsi_itepd_update_connector(struct ucsi_connector *con)
{
	if (con->num > ITEPD_MAX_PORTS || con->num < 1)
		return;

	con->typec_cap.orientation_aware = true;
}

static void ucsi_itepd_connector_status(struct ucsi_connector *con)
{
	struct ucsi_itepd *ucsi_itepd = ucsi_get_drvdata(con->ucsi);

	if (con->num > ITEPD_MAX_PORTS || con->num < 1)
		return;

	if (ucsi_itepd->connected[con->num - 1])
		typec_set_orientation(con->port,
				      ucsi_itepd->orientation[con->num - 1] ?
				      TYPEC_ORIENTATION_REVERSE :
				      TYPEC_ORIENTATION_NORMAL);
	else
		typec_set_orientation(con->port, TYPEC_ORIENTATION_NONE);

	/*
	 * Pass the ucsi_connector (which now holds the cached bitmap status)
	 * rather than the old struct ucsi_connector_status pointer.
	 */
	ucsi_itepd_connector_partner_change(ucsi_itepd, con->num - 1, con);

	ucsi_itepd->con_change_processed[con->num - 1] = false;
}

static const struct ucsi_operations ucsi_itepd_ops = {
	.read_version		= ucsi_itepd_read_version,
	.read_cci		    = ucsi_itepd_read_cci,
	.poll_cci		    = ucsi_itepd_poll_cci,
	.read_message_in	= ucsi_itepd_read_message_in,
	.sync_control		= ucsi_itepd_sync_control,
	.async_control		= ucsi_itepd_async_control,
	.update_altmodes	= ucsi_itepd_update_altmodes,
	.update_connector	= ucsi_itepd_update_connector,
	.connector_status	= ucsi_itepd_connector_status,
};

static int ucsi_itepd_probe(struct auxiliary_device *adev,
			    const struct auxiliary_device_id *id)
{
	struct device *dev = &adev->dev;
	struct ucsi_itepd *ucsi_itepd;
	struct itepd_ucsi_cb *ucsi_itepd_cb;
	int ret;

	ucsi_itepd = devm_kzalloc(dev, sizeof(*ucsi_itepd), GFP_KERNEL);
	if (!ucsi_itepd)
		return -ENOMEM;

	ucsi_itepd_cb = devm_kzalloc(dev, sizeof(*ucsi_itepd_cb), GFP_KERNEL);
	if (!ucsi_itepd_cb)
		return -ENOMEM;

	ucsi_itepd->dev = dev;
	init_completion(&ucsi_itepd->complete);
	mutex_init(&ucsi_itepd->received_lock);

	ucsi_itepd->ordered_wq = alloc_ordered_workqueue("fifo_wq", 0);
	if (!ucsi_itepd->ordered_wq)
		return -ENOMEM;

	dev_set_drvdata(dev, ucsi_itepd);
	__ucsi_itepd = ucsi_itepd;

	ucsi_itepd_cb->get_len = ucsi_itepd_get_len;
	ucsi_itepd_cb->notify  = ucsi_itepd_notify;
	ucsi_itepd_cb->priv    = ucsi_itepd;

	ret = itepd_register_cb(dev, ITEPD_CLIENT_UCSI, ucsi_itepd_cb);
	if (ret)
		goto out_destroy_wq;

	ucsi_itepd->ucsi = ucsi_create(dev, &ucsi_itepd_ops);
	if (IS_ERR(ucsi_itepd->ucsi)) {
		ret = PTR_ERR(ucsi_itepd->ucsi);
		goto out_unregister_cb;
	}

	ucsi_set_drvdata(ucsi_itepd->ucsi, ucsi_itepd);

	ret = ucsi_register(ucsi_itepd->ucsi);
	if (ret)
		goto out_ucsi_destroy;

	return 0;

out_ucsi_destroy:
	ucsi_destroy(ucsi_itepd->ucsi);
out_unregister_cb:
	itepd_register_cb(dev, ITEPD_CLIENT_UCSI, NULL);
out_destroy_wq:
	destroy_workqueue(ucsi_itepd->ordered_wq);

	return ret;
}

static void ucsi_itepd_remove(struct auxiliary_device *adev)
{
	struct ucsi_itepd *ucsi_itepd = dev_get_drvdata(&adev->dev);

	if (ucsi_itepd->ordered_wq) {
		flush_workqueue(ucsi_itepd->ordered_wq);
		destroy_workqueue(ucsi_itepd->ordered_wq);
	}

	ucsi_unregister(ucsi_itepd->ucsi);
	ucsi_destroy(ucsi_itepd->ucsi);
	usleep_range(2000, 2500);
	itepd_register_cb(&adev->dev, ITEPD_CLIENT_UCSI, NULL);
}

static const struct auxiliary_device_id ucsi_itepd_id_table[] = {
	{ .name = "itepd.ucsi", },
	{},
};
MODULE_DEVICE_TABLE(auxiliary, ucsi_itepd_id_table);

static struct auxiliary_driver ucsi_itepd_driver = {
	.name     = "ucsi_itepd",
	.probe    = ucsi_itepd_probe,
	.remove   = ucsi_itepd_remove,
	.id_table = ucsi_itepd_id_table,
};

module_auxiliary_driver(ucsi_itepd_driver);

MODULE_AUTHOR("Jeson Yang <jeson.yang@ite.com.tw>");
MODULE_DESCRIPTION("UCSI driver for ITE Type-C PD controller");
MODULE_LICENSE("GPL");
