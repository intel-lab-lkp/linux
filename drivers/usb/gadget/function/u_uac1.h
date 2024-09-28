/* SPDX-License-Identifier: GPL-2.0 */
/*
 * u_uac1.h - Utility definitions for UAC1 function
 *
 * Copyright (C) 2016 Ruslan Bilovol <ruslan.bilovol@gmail.com>
 */

#ifndef __U_UAC1_H
#define __U_UAC1_H

#include <linux/usb/composite.h>
#include <linux/usb/audio.h>
#include "uac_common.h"

#define UAC1_OUT_EP_MAX_PACKET_SIZE	200
#define UAC1_DEF_CCHMASK	0x3
#define UAC1_DEF_CSRATE		48000
#define UAC1_DEF_CSYNC		USB_ENDPOINT_SYNC_ADAPTIVE
#define UAC1_DEF_CSSIZE		2
#define UAC1_DEF_CHSBINT	0
#define UAC1_DEF_PCHMASK	0x3
#define UAC1_DEF_PSRATE		48000
#define UAC1_DEF_PSSIZE		2
#define UAC1_DEF_PHSBINT	0
#define UAC1_DEF_REQ_NUM	2
#define UAC1_DEF_INT_REQ_NUM	10

#define UAC1_DEF_MUTE_PRESENT	1
#define UAC1_DEF_VOLUME_PRESENT 1
#define UAC1_DEF_MIN_DB		(-100*256)	/* -100 dB */
#define UAC1_DEF_MAX_DB		0		/* 0 dB */
#define UAC1_DEF_RES_DB		(1*256)	/* 1 dB */

#define UAC1_DEF_P_TERM_TYPE UAC_INPUT_TERMINAL_MICROPHONE
#define UAC1_DEF_C_TERM_TYPE UAC_OUTPUT_TERMINAL_SPEAKER


struct f_uac1_opts;

struct f_uac1_alt_opts_common {
	struct config_group	group;
	struct f_uac1_opts	*opts;
	u8			alt_num;
};

/* Alt mode 0 only has a name */
struct f_uac1_alt_0_opts {
	struct f_uac1_alt_opts_common c;

	char			name[USB_MAX_STRING_LEN];

	/* Descriptors */
	struct usb_interface_descriptor	intf_desc;
};

DECLARE_UAC_FORMAT_TYPE_I_DISCRETE_DESC(UAC_MAX_RATES);
#define uac_format_type_i_discrete_descriptor			\
	uac_format_type_i_discrete_descriptor_##UAC_MAX_RATES

/* Alt modes 1+ */
struct f_uac1_alt_opts {
	struct f_uac1_alt_opts_common c;

	struct list_head	list;

	/* Strings */
	char			name[USB_MAX_STRING_LEN];
	char			it_name[USB_MAX_STRING_LEN];
	char			it_ch_name[USB_MAX_STRING_LEN];
	char			ot_name[USB_MAX_STRING_LEN];
	char			fu_vol_name[USB_MAX_STRING_LEN];

	/* Audio options */
	int			chmask;
	int			*srates; /* Reference to p/c_srates in opts */
	int			sync;
	int			ssize;
	u8			hs_bint;
	s16			terminal_type;

	/* Descriptors */
	struct usb_interface_descriptor			intf_desc;
	struct uac1_as_header_descriptor		as_header_desc;
	struct uac_format_type_i_discrete_descriptor	fmt_desc;

	struct usb_endpoint_descriptor			fs_iso_ep_desc;
	struct usb_endpoint_descriptor			hs_iso_ep_desc;
	struct usb_endpoint_descriptor			ss_iso_ep_desc;
	struct usb_ss_ep_comp_descriptor		ss_iso_ep_desc_comp;

	u8 it_id; /* Input Terminal Descriptor bTerminalID */
	u8 fu_id; /* Feature Unit Descriptor bUnitID */
	u8 ot_id; /* Output Terminal Descriptor bTerminalID */
};

#undef uac_format_type_i_discrete_descriptor

struct f_uac1_path_descriptors {
	struct list_head list;

	int dir; /* HOST_TO_DEVICE or DEVICE_TO_HOST */

	/* Alt mode opts this path descriptor is from */
	struct f_uac1_alt_opts *alt_opts;

	struct uac_input_terminal_descriptor it_desc;
	struct uac1_output_terminal_descriptor ot_desc;

	/* Feature unit is optional */
	struct uac_feature_unit_descriptor *fu_desc;
};

struct f_uac1_opts {
	struct usb_function_instance	func_inst;

	/* Alt mode 0 options */
	struct f_uac1_alt_0_opts	c_alt_0_opts;
	struct f_uac1_alt_0_opts	p_alt_0_opts;

	/* Alt mode 1 options */
	struct f_uac1_alt_opts		c_alt_1_opts;
	struct f_uac1_alt_opts		p_alt_1_opts;

	/* Alt mode 2+ options */
	struct list_head		c_alt_opts;
	struct list_head		p_alt_opts;

	/* Default options and Alt mode 1 if no c/p_alt.1 created */
	int				c_chmask;
	int				c_srates[UAC_MAX_RATES];
	int				c_sync;
	int				c_ssize;
	u8				c_hs_bint;
	int				p_chmask;
	int				p_srates[UAC_MAX_RATES];
	int				p_ssize;
	u8				p_hs_bint;

	bool				p_mute_present;
	bool				p_volume_present;
	s16				p_volume_min;
	s16				p_volume_max;
	s16				p_volume_res;

	bool				c_mute_present;
	bool				c_volume_present;
	s16				c_volume_min;
	s16				c_volume_max;
	s16				c_volume_res;

	int				req_number;
	int				fb_max;
	unsigned			bound:1;

	char				function_name[USB_MAX_STRING_LEN];

	char				p_it_name[USB_MAX_STRING_LEN];
	char				p_it_ch_name[USB_MAX_STRING_LEN];
	char				p_ot_name[USB_MAX_STRING_LEN];
	char				p_fu_vol_name[USB_MAX_STRING_LEN];

	char				c_it_name[USB_MAX_STRING_LEN];
	char				c_it_ch_name[USB_MAX_STRING_LEN];
	char				c_ot_name[USB_MAX_STRING_LEN];
	char				c_fu_vol_name[USB_MAX_STRING_LEN];

	s16				p_terminal_type;
	s16				c_terminal_type;

	struct mutex			lock;
	int				refcnt;
};

#endif /* __U_UAC1_H */
