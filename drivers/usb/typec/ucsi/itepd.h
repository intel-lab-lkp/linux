/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2025-2026, ITE. All Rights Reserved
 */
#ifndef __SOC_ITE_ITEPD_H__
#define __SOC_ITE_ITEPD_H__

#define ITEPD_MAX_PORTS						2

enum {
	ITEPD_CLIENT_UCSI,
	ITEPD_CLIENT_ALTMODE,
};

enum {
	ITEPD_SEND_UCSI_CONTROL,
	ITEPD_SEND_UCSI_MESSAGE_OUT,
};

enum {
	ITEPD_RECEIVE_UCSI_VERSION,
};

enum {
	ITEPD_USBPD_MUX_USB_0 = 0,
	ITEPD_USBPD_MUX_USB_1,
	ITEPD_USBPD_MUX_DP_0,
	ITEPD_USBPD_MUX_DP_1,
	ITEPD_USBPD_MUX_USB_DP_0,
	ITEPD_USBPD_MUX_USB_DP_1,
	ITEPD_USBPD_MUX_TBT_0,
	ITEPD_USBPD_MUX_TBT_1,
	ITEPD_USBPD_MUX_USB4_0,
	ITEPD_USBPD_MUX_USB4_1,
	ITEPD_USBPD_MUX_OFF
};

#define ITEPD_USBPD_MUX_FLIPPED				BIT(0)

struct itepd_altmode_data {
	u8 port;
	u8 mux;
	u32 dp_config;
	u32 dp_status;
};

struct itepd_ucsi_cb {
	u8 (*get_len)(void *priv, u32 cci);
	void (*notify)(void *priv, u32 cci, u8 *data);
	void *priv;
};

struct itepd_altmode_cb {
	void (*notify)(void *priv, struct itepd_altmode_data *data);
	void *priv;
};

int itepd_cmd_send(struct device *dev, unsigned int cmd, const void *val, size_t val_len);
int itepd_cmd_receive(struct device *dev, unsigned int cmd, void *val, size_t val_len);
int itepd_register_cb(struct device *dev, u8 id, void *cb);
int itepd_mode(struct device *dev, u8 port, u8 mux, u32 config, u32 status);
int itepd_hpd(struct device *dev, u8 port, u32 status);

#endif
