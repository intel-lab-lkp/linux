/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2026 David Yang
 */

#ifndef _DVB_USB_NERC_H_
#define _DVB_USB_NERC_H_

#include "dvb_usb.h"

#define NERC_STREAM_START	0xab
#define NERC_STREAM_STOP	0xac
#define NERC_POWER_ON		0xad
#define NERC_POWER_OFF		0xae
#define NERC_FRONTEND		0xe7
#define NERC_SNR		0xe8
#define NERC_QUALITY		0xe9
#define NERC_WAIT_LOCK		0xea
#define NERC_STRENGTH		0xeb
#define NERC_HAS_SIGNAL		0xec	/* 0.7s after set freq */
#define NERC_VERSION		0xed
#define NERC_FREQ_SET		0xfc

#define NERC_USB_TIMEOUT	1000

enum nerc_variant {
	NERC_VARIANT_UNKNOWN,
	NERC_VARIANT_LETV,
	NERC_VARIANT_AIWA,
	NERC_VARIANT_CVB,
};

struct nerc_priv {
	struct dvb_frontend fe;

	unsigned char variant;
	u8 buf[31];
};

#endif
