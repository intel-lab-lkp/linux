// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * China NERC (National Engineering Research Center of Digital Television)
 * DTMB (Cypress CY7C68013A + HDIC HD2312A) USB2.0 receiver.
 *
 * Copyright (c) 2026 David Yang
 */

#include "dvb_usb.h"

#include "nerc.h"

DVB_DEFINE_MOD_OPT_ADAPTER_NR(adapter_nr);

static const char *nerc_variant_name(enum nerc_variant variant)
{
	switch (variant) {
	case NERC_VARIANT_LETV:
		return "Letv";
	case NERC_VARIANT_AIWA:
		return "Aiwa";
	case NERC_VARIANT_CVB:
		return "CVB";
	default:
		return "unknown";
	}
}

static int
nerc_control_msg(struct dvb_usb_device *d, u8 request, bool read,
		 void *data, u16 size)
{
	struct nerc_priv *priv = d_to_priv(d);
	unsigned int pipe;
	u8 requesttype;
	int res;

	if (WARN_ON(size > sizeof(priv->buf)))
		return -EINVAL;

	lockdep_assert_held_once(&d->usb_mutex);

	if (read) {
		requesttype = USB_TYPE_VENDOR | USB_DIR_IN;
		pipe = usb_rcvctrlpipe(d->udev, 0);
	} else {
		requesttype = USB_TYPE_VENDOR | USB_DIR_OUT;
		pipe = usb_sndctrlpipe(d->udev, 0);
		if (size)
			memcpy(priv->buf, data, size);
	}

	/* value seems to be ignored, but just play safe */
	res = usb_control_msg(d->udev, pipe, request, requesttype, 0xfe,
			      0, priv->buf, size, NERC_USB_TIMEOUT);
	dvb_usb_dbg_usb_control_msg(d->udev, request, requesttype, 0xfe,
				    0, priv->buf, size);

	if (res < 0)
		return res;
	if (res != size)
		return -EIO;
	if (size && read)
		memcpy(data, priv->buf, res);
	return 0;
}

static int
nerc_get_tune_settings(struct dvb_frontend *fe,
		       struct dvb_frontend_tune_settings *s)
{
	s->min_delay_ms = 800;
	s->step_size = 0;
	s->max_drift = 0;

	return 0;
}

static int nerc_read_status(struct dvb_frontend *fe, enum fe_status *status)
{
	struct dvb_usb_device *d = fe_to_d(fe);
	bool has_signal;
	bool has_lock;
	int res;

	mutex_lock(&d->usb_mutex);
	res = nerc_control_msg(d, NERC_HAS_SIGNAL, true,
			       &has_signal, sizeof(has_signal));
	if (!res && has_signal) {
		res = nerc_control_msg(d, NERC_WAIT_LOCK, true,
				       &has_lock, sizeof(has_lock));
		if (res == -ETIMEDOUT) {
			res = 0;
			has_lock = false;
		}
	}
	mutex_unlock(&d->usb_mutex);
	if (res)
		return res;

	if (!has_signal)
		*status = 0;
	else if (!has_lock)
		*status = FE_HAS_SIGNAL | FE_HAS_CARRIER;
	else
		*status = FE_HAS_SIGNAL | FE_HAS_CARRIER | FE_HAS_VITERBI |
			  FE_HAS_SYNC | FE_HAS_LOCK;
	return 0;
}

static int
nerc_get_frontend(struct dvb_frontend *fe, struct dtv_frontend_properties *c)
{
	struct dvb_usb_device *d = fe_to_d(fe);
	struct nerc_priv *priv = d_to_priv(d);
	unsigned char frontend[6];
	unsigned char snr[2];
	unsigned char strength[4];
	int res;

	mutex_lock(&d->usb_mutex);

	res = nerc_control_msg(d, NERC_FRONTEND, true,
			       frontend, sizeof(frontend));
	if (res)
		goto end;

	res = nerc_control_msg(d, NERC_SNR, true, snr, sizeof(snr));
	if (res)
		goto end;

	res = nerc_control_msg(d, NERC_STRENGTH, true,
			       strength, sizeof(strength));
	if (res)
		goto end;

end:
	mutex_unlock(&d->usb_mutex);
	if (res)
		return res;

	switch (frontend[0]) {
	case 0:
		c->transmission_mode = TRANSMISSION_MODE_C1;
		break;
	case 1:
		c->transmission_mode = TRANSMISSION_MODE_C3780;
		break;
	default:
		c->transmission_mode = TRANSMISSION_MODE_AUTO;
	}

	switch (frontend[1]) {
	case 0:
		c->guard_interval = GUARD_INTERVAL_PN945;
		break;
	case 1:
		c->guard_interval = GUARD_INTERVAL_PN595;
		break;
	case 2:
		c->guard_interval = GUARD_INTERVAL_PN420;
		break;
	default:
		c->guard_interval = GUARD_INTERVAL_AUTO;
	}

	switch (frontend[2]) {
	case 0:
		c->fec_inner = FEC_2_5;
		break;
	case 1:
		c->fec_inner = FEC_3_5;
		break;
	case 2:
		c->fec_inner = FEC_4_5;
		break;
	default:
		c->fec_inner = FEC_AUTO;
	}

	switch (frontend[3]) {
	case 0:
		c->interleaving = INTERLEAVING_720;
		break;
	case 1:
		c->interleaving = INTERLEAVING_240;
		break;
	default:
		c->interleaving = INTERLEAVING_AUTO;
	}

	switch (frontend[4]) {
	case 0:
		c->modulation = QAM_4_NR;
		break;
	case 1:
		c->modulation = QPSK;
		break;
	case 2:
		c->modulation = QAM_16;
		break;
	case 3:
		c->modulation = QAM_32;
		break;
	case 4:
		c->modulation = QAM_64;
		break;
	default:
		c->modulation = QAM_AUTO;
	}

	switch (frontend[5]) {
	case 0:
		c->inversion = INVERSION_ON;
		break;
	case 1:
		c->inversion = INVERSION_OFF;
		break;
	default:
		c->inversion = INVERSION_AUTO;
	}

	if (priv->variant == NERC_VARIANT_CVB)
		c->strength.stat[0].svalue = -1000 * strength[3];
	else
		c->strength.stat[0].uvalue = strength[3] * 0xffff / 100;
	c->cnr.stat[0].svalue = 10 * (100 * snr[0] + snr[1]);

	return 0;
}

static int nerc_set_frontend(struct dvb_frontend *fe)
{
	struct dtv_frontend_properties *c = &fe->dtv_property_cache;
	__be32 freq = cpu_to_be32(c->frequency);
	struct dvb_usb_device *d = fe_to_d(fe);
	int res;

	mutex_lock(&d->usb_mutex);
	res = nerc_control_msg(d, NERC_FREQ_SET, false, &freq, sizeof(freq));
	mutex_unlock(&d->usb_mutex);

	return res;
}

static const struct dvb_frontend_ops nerc_ops = {
	.delsys = { SYS_DTMB },
	.info = {
		.name = "HDIC HD2312A (in NERC DtmbUSB)",
		.frequency_min_hz = 52 * MHz,
		.frequency_max_hz = 866 * MHz,
		.frequency_stepsize_hz = 10 * kHz,
		.caps = FE_CAN_INVERSION_AUTO | FE_CAN_FEC_AUTO |
			FE_CAN_QAM_AUTO | FE_CAN_TRANSMISSION_MODE_AUTO |
			FE_CAN_BANDWIDTH_AUTO | FE_CAN_GUARD_INTERVAL_AUTO
	},
	.get_tune_settings = nerc_get_tune_settings,
	.read_status = nerc_read_status,
	.get_frontend = nerc_get_frontend,
	.set_frontend = nerc_set_frontend,
};

static int nerc_streaming_ctrl(struct dvb_frontend *fe, int on)
{
	struct dvb_usb_device *d = fe_to_d(fe);
	int res;

	mutex_lock(&d->usb_mutex);
	res = nerc_control_msg(d, on ? NERC_STREAM_START : NERC_STREAM_STOP,
			       false, NULL, 0);
	mutex_unlock(&d->usb_mutex);

	return res;
}

static int nerc_frontend_attach(struct dvb_usb_adapter *adap)
{
	struct nerc_priv *priv = adap_to_priv(adap);
	struct dvb_frontend *fe = &priv->fe;
	struct dtv_frontend_properties *c = &fe->dtv_property_cache;

	/* init frontend callback ops */
	memcpy(&fe->ops, &nerc_ops, sizeof(struct dvb_frontend_ops));

	c->strength.len = 1;
	if (priv->variant == NERC_VARIANT_CVB)
		c->strength.stat[0].scale = FE_SCALE_DECIBEL;
	else
		c->strength.stat[0].scale = FE_SCALE_RELATIVE;
	c->cnr.len = 1;
	c->cnr.stat[0].scale = FE_SCALE_DECIBEL;

	adap->fe[0] = fe;
	return 0;
}

static int nerc_power_ctrl(struct dvb_usb_device *d, int on)
{
	int res;

	mutex_lock(&d->usb_mutex);
	res = nerc_control_msg(d, on ? NERC_POWER_ON : NERC_POWER_OFF,
			       false, NULL, 0);
	mutex_unlock(&d->usb_mutex);

	return res;
}

static int nerc_probe(struct dvb_usb_device *d)
{
	struct nerc_priv *priv = d_to_priv(d);
	unsigned char buf[4];
	int res;

	mutex_lock(&d->usb_mutex);
	res = nerc_control_msg(d, NERC_VERSION, true, buf, sizeof(buf));
	mutex_unlock(&d->usb_mutex);
	if (res)
		return res;

	if (buf[1] == 8 && buf[2] == 32 && buf[3] == 68) {
		if (buf[0] == 3)
			priv->variant = NERC_VARIANT_LETV;
		else if (buf[0] == 5)
			priv->variant = NERC_VARIANT_AIWA;
		else if (buf[0] == 6)
			priv->variant = NERC_VARIANT_CVB;
	}

	if (priv->variant == NERC_VARIANT_UNKNOWN) {
		dev_err(&d->udev->dev, "Unknown NERC DtmbUSB v%u.%u.%u%u",
			buf[0], buf[1], buf[2], buf[3]);
		return -ENODEV;
	}

	/* yes, missing the last dot */
	dev_info(&d->udev->dev, "NERC DtmbUSB v%u.%u.%u%u (%s)",
		 buf[0], buf[1], buf[2], buf[3],
		 nerc_variant_name(priv->variant));
	return 0;
}

static const struct dvb_usb_device_properties nerc_props = {
	.driver_name = KBUILD_MODNAME,
	.owner = THIS_MODULE,
	.adapter_nr = adapter_nr,
	.size_of_priv = sizeof(struct nerc_priv),

	.streaming_ctrl = nerc_streaming_ctrl,
	.frontend_attach = nerc_frontend_attach,
	.power_ctrl = nerc_power_ctrl,
	.probe = nerc_probe,

	.num_adapters = 1,
	.adapter = {
		{
			.stream = DVB_USB_STREAM_BULK(0x82, 8, 4096),
		},
	},
};

static const struct usb_device_id nerc_id_table[] = {
	{ DVB_USB_DEVICE(USB_VID_CYPRESS, USB_PID_NERC_DTMBUSB,
		&nerc_props, "NERC DtmbUSB", NULL) },
	{ }
};
MODULE_DEVICE_TABLE(usb, nerc_id_table);

static struct usb_driver nerc_usb_driver = {
	.name = KBUILD_MODNAME,
	.id_table = nerc_id_table,
	.probe = dvb_usbv2_probe,
	.disconnect = dvb_usbv2_disconnect,
	.suspend = dvb_usbv2_suspend,
	.resume = dvb_usbv2_resume,
	.reset_resume = dvb_usbv2_reset_resume,
	.no_dynamic_id = 1,
	.soft_unbind = 1,
};

module_usb_driver(nerc_usb_driver);

MODULE_AUTHOR("David Yang <mmyangfl@gmail.com>");
MODULE_DESCRIPTION("Driver for NERC DtmbUSB");
MODULE_LICENSE("GPL");
