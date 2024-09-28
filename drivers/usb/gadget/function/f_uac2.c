// SPDX-License-Identifier: GPL-2.0+
/*
 * f_uac2.c -- USB Audio Class 2.0 Function
 *
 * Copyright (C) 2011
 *    Yadwinder Singh (yadi.brar01@gmail.com)
 *    Jaswinder Singh (jaswinder.singh@linaro.org)
 *
 * Copyright (C) 2020
 *    Ruslan Bilovol (ruslan.bilovol@gmail.com)
 */

#include <linux/usb/audio.h>
#include <linux/usb/audio-v2.h>
#include <linux/module.h>

#include "u_audio.h"

#include "u_uac2.h"
#include "u_uac_utils.h"

#define HOST_TO_DEVICE 0
#define DEVICE_TO_HOST 1

/* UAC2 spec: 4.1 Audio Channel Cluster Descriptor */
#define UAC2_CHANNEL_MASK 0x07FFFFFF

/*
 * The driver implements a simple UAC_2 topology.
 * USB-OUT -> IT_1 -> FU -> OT_3 -> ALSA_Capture
 * ALSA_Playback -> IT_2 -> FU -> OT_4 -> USB-IN
 * Capture and Playback sampling rates are independently
 *  controlled by two clock sources :
 *    CLK_5 := c_srate, and CLK_6 := p_srate
 */
#define USB_OUT_CLK_ID	(out_clk_src_desc.bClockID)
#define USB_IN_CLK_ID	(in_clk_src_desc.bClockID)
#define USB_OUT_FU_ID	(out_feature_unit_desc->bUnitID)
#define USB_IN_FU_ID	(in_feature_unit_desc->bUnitID)

#define CONTROL_ABSENT	0
#define CONTROL_RDONLY	1
#define CONTROL_RDWR	3

#define CLK_FREQ_CTRL	0
#define CLK_VLD_CTRL	2
#define FU_MUTE_CTRL	0
#define FU_VOL_CTRL	2

#define COPY_CTRL	0
#define CONN_CTRL	2
#define OVRLD_CTRL	4
#define CLSTR_CTRL	6
#define UNFLW_CTRL	8
#define OVFLW_CTRL	10

#define EPIN_EN(_opts) ((_opts)->p_chmask != 0)
#define EPOUT_EN(_opts) ((_opts)->c_chmask != 0)
#define FUIN_EN(_opts) (EPIN_EN(_opts) \
				&& ((_opts)->p_mute_present \
				|| (_opts)->p_volume_present))
#define FUOUT_EN(_opts) (EPOUT_EN(_opts) \
				&& ((_opts)->c_mute_present \
				|| (_opts)->c_volume_present))
#define EPOUT_FBACK_IN_EN(_opts) ((_opts)->c_sync == USB_ENDPOINT_SYNC_ASYNC)

struct f_uac2 {
	struct g_audio g_audio;
	u8 ac_intf, as_in_intf, as_out_intf;
	u8 ac_alt, as_in_alt, as_out_alt;	/* needed for get_alt() */

	struct usb_ctrlrequest setup_cr;	/* will be used in data stage */

	/* Interrupt IN endpoint of AC interface */
	struct usb_ep	*int_ep;
	atomic_t	int_count;
	/* transient state, only valid during handling of a single control request */
	int clock_id;
};

static inline struct f_uac2 *func_to_uac2(struct usb_function *f)
{
	return container_of(f, struct f_uac2, g_audio.func);
}

static inline
struct f_uac2_opts *g_audio_to_uac2_opts(struct g_audio *agdev)
{
	return container_of(agdev->func.fi, struct f_uac2_opts, func_inst);
}

static int afunc_notify(struct g_audio *agdev, int unit_id, int cs);

/* --------- USB Function Interface ------------- */

static struct usb_interface_assoc_descriptor iad_desc = {
	.bLength = sizeof iad_desc,
	.bDescriptorType = USB_DT_INTERFACE_ASSOCIATION,

	.bFirstInterface = 0,
	.bInterfaceCount = 3,
	.bFunctionClass = USB_CLASS_AUDIO,
	.bFunctionSubClass = UAC2_FUNCTION_SUBCLASS_UNDEFINED,
	.bFunctionProtocol = UAC_VERSION_2,
};

/* Audio Control Interface */
static struct usb_interface_descriptor std_ac_if_desc = {
	.bLength = sizeof std_ac_if_desc,
	.bDescriptorType = USB_DT_INTERFACE,

	.bAlternateSetting = 0,
	/* .bNumEndpoints = DYNAMIC */
	.bInterfaceClass = USB_CLASS_AUDIO,
	.bInterfaceSubClass = USB_SUBCLASS_AUDIOCONTROL,
	.bInterfaceProtocol = UAC_VERSION_2,
};

/* Clock source for IN traffic */
static struct uac_clock_source_descriptor in_clk_src_desc = {
	.bLength = sizeof in_clk_src_desc,
	.bDescriptorType = USB_DT_CS_INTERFACE,

	.bDescriptorSubtype = UAC2_CLOCK_SOURCE,
	/* .bClockID = DYNAMIC */
	.bmAttributes = UAC_CLOCK_SOURCE_TYPE_INT_FIXED,
	.bmControls = (CONTROL_RDWR << CLK_FREQ_CTRL),
	.bAssocTerminal = 0,
};

/* Clock source for OUT traffic */
static struct uac_clock_source_descriptor out_clk_src_desc = {
	.bLength = sizeof out_clk_src_desc,
	.bDescriptorType = USB_DT_CS_INTERFACE,

	.bDescriptorSubtype = UAC2_CLOCK_SOURCE,
	/* .bClockID = DYNAMIC */
	.bmAttributes = UAC_CLOCK_SOURCE_TYPE_INT_FIXED,
	.bmControls = (CONTROL_RDWR << CLK_FREQ_CTRL),
	.bAssocTerminal = 0,
};

/* Input Terminal for USB_OUT */
static struct uac2_input_terminal_descriptor usb_out_it_desc = {
	.bLength = sizeof usb_out_it_desc,
	.bDescriptorType = USB_DT_CS_INTERFACE,

	.bDescriptorSubtype = UAC_INPUT_TERMINAL,
	/* .bTerminalID = DYNAMIC */
	.wTerminalType = cpu_to_le16(UAC_TERMINAL_STREAMING),
	.bAssocTerminal = 0,
	/* .bCSourceID = DYNAMIC */
	.iChannelNames = 0,
	.bmControls = cpu_to_le16(CONTROL_RDWR << COPY_CTRL),
};

/* Input Terminal for I/O-In */
static struct uac2_input_terminal_descriptor io_in_it_desc = {
	.bLength = sizeof io_in_it_desc,
	.bDescriptorType = USB_DT_CS_INTERFACE,

	.bDescriptorSubtype = UAC_INPUT_TERMINAL,
	/* .bTerminalID = DYNAMIC */
	/* .wTerminalType = DYNAMIC */
	.bAssocTerminal = 0,
	/* .bCSourceID = DYNAMIC */
	.iChannelNames = 0,
	.bmControls = cpu_to_le16(CONTROL_RDWR << COPY_CTRL),
};

/* Ouput Terminal for USB_IN */
static struct uac2_output_terminal_descriptor usb_in_ot_desc = {
	.bLength = sizeof usb_in_ot_desc,
	.bDescriptorType = USB_DT_CS_INTERFACE,

	.bDescriptorSubtype = UAC_OUTPUT_TERMINAL,
	/* .bTerminalID = DYNAMIC */
	.wTerminalType = cpu_to_le16(UAC_TERMINAL_STREAMING),
	.bAssocTerminal = 0,
	/* .bSourceID = DYNAMIC */
	/* .bCSourceID = DYNAMIC */
	.bmControls = cpu_to_le16(CONTROL_RDWR << COPY_CTRL),
};

/* Ouput Terminal for I/O-Out */
static struct uac2_output_terminal_descriptor io_out_ot_desc = {
	.bLength = sizeof io_out_ot_desc,
	.bDescriptorType = USB_DT_CS_INTERFACE,

	.bDescriptorSubtype = UAC_OUTPUT_TERMINAL,
	/* .bTerminalID = DYNAMIC */
	/* .wTerminalType = DYNAMIC */
	.bAssocTerminal = 0,
	/* .bSourceID = DYNAMIC */
	/* .bCSourceID = DYNAMIC */
	.bmControls = cpu_to_le16(CONTROL_RDWR << COPY_CTRL),
};

static struct uac2_feature_unit_descriptor *in_feature_unit_desc;
static struct uac2_feature_unit_descriptor *out_feature_unit_desc;

static struct uac2_ac_header_descriptor ac_hdr_desc = {
	.bLength = sizeof ac_hdr_desc,
	.bDescriptorType = USB_DT_CS_INTERFACE,

	.bDescriptorSubtype = UAC_MS_HEADER,
	.bcdADC = cpu_to_le16(0x200),
	.bCategory = UAC2_FUNCTION_IO_BOX,
	/* .wTotalLength = DYNAMIC */
	.bmControls = 0,
};

/* AC IN Interrupt Endpoint */
static struct usb_endpoint_descriptor fs_ep_int_desc = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,

	.bEndpointAddress = USB_DIR_IN,
	.bmAttributes = USB_ENDPOINT_XFER_INT,
	.wMaxPacketSize = cpu_to_le16(6),
	.bInterval = 1,
};

static struct usb_endpoint_descriptor hs_ep_int_desc = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,

	.bmAttributes = USB_ENDPOINT_XFER_INT,
	.wMaxPacketSize = cpu_to_le16(6),
	.bInterval = 4,
};

static struct usb_endpoint_descriptor ss_ep_int_desc = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,

	.bEndpointAddress = USB_DIR_IN,
	.bmAttributes = USB_ENDPOINT_XFER_INT,
	.wMaxPacketSize = cpu_to_le16(6),
	.bInterval = 4,
};

static struct usb_ss_ep_comp_descriptor ss_ep_int_desc_comp = {
	.bLength = sizeof(ss_ep_int_desc_comp),
	.bDescriptorType = USB_DT_SS_ENDPOINT_COMP,
	.wBytesPerInterval = cpu_to_le16(6),
};

/* Audio Streaming OUT Interface - Alt0 */
static struct usb_interface_descriptor std_as_out_if0_desc = {
	.bLength = sizeof std_as_out_if0_desc,
	.bDescriptorType = USB_DT_INTERFACE,

	.bAlternateSetting = 0,
	.bNumEndpoints = 0,
	.bInterfaceClass = USB_CLASS_AUDIO,
	.bInterfaceSubClass = USB_SUBCLASS_AUDIOSTREAMING,
	.bInterfaceProtocol = UAC_VERSION_2,
};

/* Audio Streaming OUT Interface - Alt1 */
static struct usb_interface_descriptor std_as_out_if1_desc = {
	.bLength = sizeof std_as_out_if1_desc,
	.bDescriptorType = USB_DT_INTERFACE,

	.bAlternateSetting = 1,
	.bNumEndpoints = 1,
	.bInterfaceClass = USB_CLASS_AUDIO,
	.bInterfaceSubClass = USB_SUBCLASS_AUDIOSTREAMING,
	.bInterfaceProtocol = UAC_VERSION_2,
};

/* Audio Stream OUT Intface Desc */
static struct uac2_as_header_descriptor as_out_hdr_desc = {
	.bLength = sizeof as_out_hdr_desc,
	.bDescriptorType = USB_DT_CS_INTERFACE,

	.bDescriptorSubtype = UAC_AS_GENERAL,
	/* .bTerminalLink = DYNAMIC */
	.bmControls = 0,
	.bFormatType = UAC_FORMAT_TYPE_I,
	.bmFormats = cpu_to_le32(UAC_FORMAT_TYPE_I_PCM),
	.iChannelNames = 0,
};

/* Audio USB_OUT Format */
static struct uac2_format_type_i_descriptor as_out_fmt1_desc = {
	.bLength = sizeof as_out_fmt1_desc,
	.bDescriptorType = USB_DT_CS_INTERFACE,
	.bDescriptorSubtype = UAC_FORMAT_TYPE,
	.bFormatType = UAC_FORMAT_TYPE_I,
};

/* STD AS ISO OUT Endpoint */
static struct usb_endpoint_descriptor fs_epout_desc = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,

	.bEndpointAddress = USB_DIR_OUT,
	/* .bmAttributes = DYNAMIC */
	/* .wMaxPacketSize = DYNAMIC */
	.bInterval = 1,
};

static struct usb_endpoint_descriptor hs_epout_desc = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,

	/* .bmAttributes = DYNAMIC */
	/* .wMaxPacketSize = DYNAMIC */
	/* .bInterval = DYNAMIC */
};

static struct usb_endpoint_descriptor ss_epout_desc = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,

	.bEndpointAddress = USB_DIR_OUT,
	/* .bmAttributes = DYNAMIC */
	/* .wMaxPacketSize = DYNAMIC */
	/* .bInterval = DYNAMIC */
};

static struct usb_ss_ep_comp_descriptor ss_epout_desc_comp = {
	.bLength		= sizeof(ss_epout_desc_comp),
	.bDescriptorType	= USB_DT_SS_ENDPOINT_COMP,
	.bMaxBurst		= 0,
	.bmAttributes		= 0,
	/* wBytesPerInterval = DYNAMIC */
};

/* CS AS ISO OUT Endpoint */
static struct uac2_iso_endpoint_descriptor as_iso_out_desc = {
	.bLength = sizeof as_iso_out_desc,
	.bDescriptorType = USB_DT_CS_ENDPOINT,

	.bDescriptorSubtype = UAC_EP_GENERAL,
	.bmAttributes = 0,
	.bmControls = 0,
	.bLockDelayUnits = 0,
	.wLockDelay = 0,
};

/* STD AS ISO IN Feedback Endpoint */
static struct usb_endpoint_descriptor fs_epin_fback_desc = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,

	.bEndpointAddress = USB_DIR_IN,
	.bmAttributes = USB_ENDPOINT_XFER_ISOC | USB_ENDPOINT_USAGE_FEEDBACK,
	.wMaxPacketSize = cpu_to_le16(3),
	.bInterval = 1,
};

static struct usb_endpoint_descriptor hs_epin_fback_desc = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,

	.bmAttributes = USB_ENDPOINT_XFER_ISOC | USB_ENDPOINT_USAGE_FEEDBACK,
	.wMaxPacketSize = cpu_to_le16(4),
	.bInterval = 4,
};

static struct usb_endpoint_descriptor ss_epin_fback_desc = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,

	.bEndpointAddress = USB_DIR_IN,
	.bmAttributes = USB_ENDPOINT_XFER_ISOC | USB_ENDPOINT_USAGE_FEEDBACK,
	.wMaxPacketSize = cpu_to_le16(4),
	.bInterval = 4,
};

static struct usb_ss_ep_comp_descriptor ss_epin_fback_desc_comp = {
	.bLength		= sizeof(ss_epin_fback_desc_comp),
	.bDescriptorType	= USB_DT_SS_ENDPOINT_COMP,
	.bMaxBurst		= 0,
	.bmAttributes		= 0,
	.wBytesPerInterval	= cpu_to_le16(4),
};


/* Audio Streaming IN Interface - Alt0 */
static struct usb_interface_descriptor std_as_in_if0_desc = {
	.bLength = sizeof std_as_in_if0_desc,
	.bDescriptorType = USB_DT_INTERFACE,

	.bAlternateSetting = 0,
	.bNumEndpoints = 0,
	.bInterfaceClass = USB_CLASS_AUDIO,
	.bInterfaceSubClass = USB_SUBCLASS_AUDIOSTREAMING,
	.bInterfaceProtocol = UAC_VERSION_2,
};

/* Audio Streaming IN Interface - Alt1 */
static struct usb_interface_descriptor std_as_in_if1_desc = {
	.bLength = sizeof std_as_in_if1_desc,
	.bDescriptorType = USB_DT_INTERFACE,

	.bAlternateSetting = 1,
	.bNumEndpoints = 1,
	.bInterfaceClass = USB_CLASS_AUDIO,
	.bInterfaceSubClass = USB_SUBCLASS_AUDIOSTREAMING,
	.bInterfaceProtocol = UAC_VERSION_2,
};

/* Audio Stream IN Intface Desc */
static struct uac2_as_header_descriptor as_in_hdr_desc = {
	.bLength = sizeof as_in_hdr_desc,
	.bDescriptorType = USB_DT_CS_INTERFACE,

	.bDescriptorSubtype = UAC_AS_GENERAL,
	/* .bTerminalLink = DYNAMIC */
	.bmControls = 0,
	.bFormatType = UAC_FORMAT_TYPE_I,
	.bmFormats = cpu_to_le32(UAC_FORMAT_TYPE_I_PCM),
	.iChannelNames = 0,
};

/* Audio USB_IN Format */
static struct uac2_format_type_i_descriptor as_in_fmt1_desc = {
	.bLength = sizeof as_in_fmt1_desc,
	.bDescriptorType = USB_DT_CS_INTERFACE,
	.bDescriptorSubtype = UAC_FORMAT_TYPE,
	.bFormatType = UAC_FORMAT_TYPE_I,
};

/* STD AS ISO IN Endpoint */
static struct usb_endpoint_descriptor fs_epin_desc = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,

	.bEndpointAddress = USB_DIR_IN,
	.bmAttributes = USB_ENDPOINT_XFER_ISOC | USB_ENDPOINT_SYNC_ASYNC,
	/* .wMaxPacketSize = DYNAMIC */
	.bInterval = 1,
};

static struct usb_endpoint_descriptor hs_epin_desc = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,

	.bmAttributes = USB_ENDPOINT_XFER_ISOC | USB_ENDPOINT_SYNC_ASYNC,
	/* .wMaxPacketSize = DYNAMIC */
	/* .bInterval = DYNAMIC */
};

static struct usb_endpoint_descriptor ss_epin_desc = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,

	.bEndpointAddress = USB_DIR_IN,
	.bmAttributes = USB_ENDPOINT_XFER_ISOC | USB_ENDPOINT_SYNC_ASYNC,
	/* .wMaxPacketSize = DYNAMIC */
	/* .bInterval = DYNAMIC */
};

static struct usb_ss_ep_comp_descriptor ss_epin_desc_comp = {
	.bLength		= sizeof(ss_epin_desc_comp),
	.bDescriptorType	= USB_DT_SS_ENDPOINT_COMP,
	.bMaxBurst		= 0,
	.bmAttributes		= 0,
	/* wBytesPerInterval = DYNAMIC */
};

/* CS AS ISO IN Endpoint */
static struct uac2_iso_endpoint_descriptor as_iso_in_desc = {
	.bLength = sizeof as_iso_in_desc,
	.bDescriptorType = USB_DT_CS_ENDPOINT,

	.bDescriptorSubtype = UAC_EP_GENERAL,
	.bmAttributes = 0,
	.bmControls = 0,
	.bLockDelayUnits = 0,
	.wLockDelay = 0,
};

static struct usb_descriptor_header *fs_audio_desc[] = {
	(struct usb_descriptor_header *)&iad_desc,
	(struct usb_descriptor_header *)&std_ac_if_desc,

	(struct usb_descriptor_header *)&ac_hdr_desc,
	(struct usb_descriptor_header *)&in_clk_src_desc,
	(struct usb_descriptor_header *)&out_clk_src_desc,
	(struct usb_descriptor_header *)&usb_out_it_desc,
	(struct usb_descriptor_header *)&out_feature_unit_desc,
	(struct usb_descriptor_header *)&io_in_it_desc,
	(struct usb_descriptor_header *)&usb_in_ot_desc,
	(struct usb_descriptor_header *)&in_feature_unit_desc,
	(struct usb_descriptor_header *)&io_out_ot_desc,

	(struct usb_descriptor_header *)&fs_ep_int_desc,

	(struct usb_descriptor_header *)&std_as_out_if0_desc,
	(struct usb_descriptor_header *)&std_as_out_if1_desc,

	(struct usb_descriptor_header *)&as_out_hdr_desc,
	(struct usb_descriptor_header *)&as_out_fmt1_desc,
	(struct usb_descriptor_header *)&fs_epout_desc,
	(struct usb_descriptor_header *)&as_iso_out_desc,
	(struct usb_descriptor_header *)&fs_epin_fback_desc,

	(struct usb_descriptor_header *)&std_as_in_if0_desc,
	(struct usb_descriptor_header *)&std_as_in_if1_desc,

	(struct usb_descriptor_header *)&as_in_hdr_desc,
	(struct usb_descriptor_header *)&as_in_fmt1_desc,
	(struct usb_descriptor_header *)&fs_epin_desc,
	(struct usb_descriptor_header *)&as_iso_in_desc,
	NULL,
};

static struct usb_descriptor_header *hs_audio_desc[] = {
	(struct usb_descriptor_header *)&iad_desc,
	(struct usb_descriptor_header *)&std_ac_if_desc,

	(struct usb_descriptor_header *)&ac_hdr_desc,
	(struct usb_descriptor_header *)&in_clk_src_desc,
	(struct usb_descriptor_header *)&out_clk_src_desc,
	(struct usb_descriptor_header *)&usb_out_it_desc,
	(struct usb_descriptor_header *)&out_feature_unit_desc,
	(struct usb_descriptor_header *)&io_in_it_desc,
	(struct usb_descriptor_header *)&usb_in_ot_desc,
	(struct usb_descriptor_header *)&in_feature_unit_desc,
	(struct usb_descriptor_header *)&io_out_ot_desc,

	(struct usb_descriptor_header *)&hs_ep_int_desc,

	(struct usb_descriptor_header *)&std_as_out_if0_desc,
	(struct usb_descriptor_header *)&std_as_out_if1_desc,

	(struct usb_descriptor_header *)&as_out_hdr_desc,
	(struct usb_descriptor_header *)&as_out_fmt1_desc,
	(struct usb_descriptor_header *)&hs_epout_desc,
	(struct usb_descriptor_header *)&as_iso_out_desc,
	(struct usb_descriptor_header *)&hs_epin_fback_desc,

	(struct usb_descriptor_header *)&std_as_in_if0_desc,
	(struct usb_descriptor_header *)&std_as_in_if1_desc,

	(struct usb_descriptor_header *)&as_in_hdr_desc,
	(struct usb_descriptor_header *)&as_in_fmt1_desc,
	(struct usb_descriptor_header *)&hs_epin_desc,
	(struct usb_descriptor_header *)&as_iso_in_desc,
	NULL,
};

static struct usb_descriptor_header *ss_audio_desc[] = {
	(struct usb_descriptor_header *)&iad_desc,
	(struct usb_descriptor_header *)&std_ac_if_desc,

	(struct usb_descriptor_header *)&ac_hdr_desc,
	(struct usb_descriptor_header *)&in_clk_src_desc,
	(struct usb_descriptor_header *)&out_clk_src_desc,
	(struct usb_descriptor_header *)&usb_out_it_desc,
  (struct usb_descriptor_header *)&out_feature_unit_desc,
	(struct usb_descriptor_header *)&io_in_it_desc,
	(struct usb_descriptor_header *)&usb_in_ot_desc,
	(struct usb_descriptor_header *)&in_feature_unit_desc,
	(struct usb_descriptor_header *)&io_out_ot_desc,

	(struct usb_descriptor_header *)&ss_ep_int_desc,
	(struct usb_descriptor_header *)&ss_ep_int_desc_comp,

	(struct usb_descriptor_header *)&std_as_out_if0_desc,
	(struct usb_descriptor_header *)&std_as_out_if1_desc,

	(struct usb_descriptor_header *)&as_out_hdr_desc,
	(struct usb_descriptor_header *)&as_out_fmt1_desc,
	(struct usb_descriptor_header *)&ss_epout_desc,
	(struct usb_descriptor_header *)&ss_epout_desc_comp,
	(struct usb_descriptor_header *)&as_iso_out_desc,
	(struct usb_descriptor_header *)&ss_epin_fback_desc,
	(struct usb_descriptor_header *)&ss_epin_fback_desc_comp,

	(struct usb_descriptor_header *)&std_as_in_if0_desc,
	(struct usb_descriptor_header *)&std_as_in_if1_desc,

	(struct usb_descriptor_header *)&as_in_hdr_desc,
	(struct usb_descriptor_header *)&as_in_fmt1_desc,
	(struct usb_descriptor_header *)&ss_epin_desc,
	(struct usb_descriptor_header *)&ss_epin_desc_comp,
	(struct usb_descriptor_header *)&as_iso_in_desc,
	NULL,
};

struct cntrl_cur_lay2 {
	__le16	wCUR;
};

struct cntrl_range_lay2 {
	__le16	wNumSubRanges;
	__le16	wMIN;
	__le16	wMAX;
	__le16	wRES;
} __packed;

struct cntrl_cur_lay3 {
	__le32	dCUR;
};

struct cntrl_subrange_lay3 {
	__le32	dMIN;
	__le32	dMAX;
	__le32	dRES;
} __packed;

#define ranges_lay3_size(c) (sizeof(c.wNumSubRanges)	\
		+ le16_to_cpu(c.wNumSubRanges)		\
		* sizeof(struct cntrl_subrange_lay3))

#define DECLARE_UAC2_CNTRL_RANGES_LAY3(k, n)		\
	struct cntrl_ranges_lay3_##k {			\
	__le16	wNumSubRanges;				\
	struct cntrl_subrange_lay3 r[n];		\
} __packed

DECLARE_UAC2_CNTRL_RANGES_LAY3(srates, UAC_MAX_RATES);

/*-------------------------------------------------------------------------*/

/*
 * String handling
 */

#define MAX_STRINGS 256

static int add_string(struct usb_string *strings, const char *s)
{
	int i;

	if (!s || s[0] == '\0')
		return 0;

	for (i = 0; i < MAX_STRINGS; i++) {
		if (!strings[i].s) {
			strings[i].s = s;
			return 0; /* IDs aren't allocated yet */
		}

		if (!strcmp(s, strings[i].s))
			return strings[i].id;
	}

	return -1;
}

static void add_alt_strings(struct usb_string *strings, struct f_uac2_alt_opts *alt_opts, bool fu)
{
	add_string(strings, alt_opts->name);
	add_string(strings, alt_opts->it_name);
	add_string(strings, alt_opts->it_ch_name);
	add_string(strings, alt_opts->ot_name);
	if (fu)
		add_string(strings, alt_opts->fu_vol_name);
}

static struct usb_string *attach_strings(struct usb_composite_dev *cdev,
					 struct f_uac2_opts *audio_opts)
{
	struct usb_string	*strings = kzalloc(sizeof(struct usb_string) * MAX_STRINGS,
						   GFP_KERNEL);
	struct f_uac2_alt_opts	*alt_opts;
	struct usb_string	*us;
	int			strings_fn_length;

	struct usb_gadget_strings strings_fn = {
		.language = 0x0409,	/* en-us */
		.strings = strings
	};

	struct usb_gadget_strings *fn_strings[] = {
		&strings_fn,
		NULL,
	};

	if (!strings)
		return ERR_PTR(-ENOMEM);

	/* Add all the strings from all the alt mode options */
	add_string(strings, audio_opts->function_name);
	add_string(strings, audio_opts->if_ctrl_name);
	add_string(strings, audio_opts->clksrc_in_name);
	add_string(strings, audio_opts->clksrc_out_name);
	add_string(strings, audio_opts->c_alt_0_opts.name);
	add_string(strings, audio_opts->p_alt_0_opts.name);
	add_alt_strings(strings, &audio_opts->c_alt_1_opts, FUOUT_EN(audio_opts));
	add_alt_strings(strings, &audio_opts->p_alt_1_opts, FUIN_EN(audio_opts));
	list_for_each_entry(alt_opts, &audio_opts->c_alt_opts, list) {
		add_alt_strings(strings, alt_opts, FUOUT_EN(audio_opts));
	}
	list_for_each_entry(alt_opts, &audio_opts->p_alt_opts, list) {
		add_alt_strings(strings, alt_opts, FUIN_EN(audio_opts));
	}

	for (strings_fn_length = 0; strings[strings_fn_length].s; strings_fn_length++)
		;

	/* Attach strings to the composite device and get string IDs assigned */
	us = usb_gstrings_attach(cdev, fn_strings, strings_fn_length);

	/* Strings are now copied to the composite device and we use the
	 * copy in "us" going forward, that has all the string IDs.
	 */
	kfree(strings);

	return us;
}

/*-------------------------------------------------------------------------*/

static int set_ep_max_packet_size_bint(struct device *dev, const struct f_uac2_alt_opts *alt_opts,
	struct usb_endpoint_descriptor *ep_desc,
	enum usb_device_speed speed, bool is_playback)
{
	return uac_set_ep_max_packet_size_bint(
		dev, ep_desc, speed, is_playback, alt_opts->hs_bint, alt_opts->chmask,
		get_max_srate(is_playback ? alt_opts->c.opts->p_srates
					  : alt_opts->c.opts->c_srates),
		alt_opts->ssize, alt_opts->sync, alt_opts->c.opts->fb_max);
}

static struct uac2_feature_unit_descriptor *build_fu_desc(int chmask)
{
	struct uac2_feature_unit_descriptor *fu_desc;
	int channels = num_channels(chmask);
	int fu_desc_size = UAC2_DT_FEATURE_UNIT_SIZE(channels);

	fu_desc = kzalloc(fu_desc_size, GFP_KERNEL);
	if (!fu_desc)
		return NULL;

	fu_desc->bLength = fu_desc_size;
	fu_desc->bDescriptorType = USB_DT_CS_INTERFACE;

	fu_desc->bDescriptorSubtype = UAC_FEATURE_UNIT;

	/* bUnitID, bSourceID and bmaControls will be defined later */

	return fu_desc;
}

/* Use macro to overcome line length limitation */
#define USBDHDR(p) (struct usb_descriptor_header *)(p)

static void setup_headers(struct f_uac2_opts *opts,
			  struct usb_descriptor_header **headers,
			  enum usb_device_speed speed)
{
	struct usb_ss_ep_comp_descriptor *epout_desc_comp = NULL;
	struct usb_ss_ep_comp_descriptor *epin_desc_comp = NULL;
	struct usb_ss_ep_comp_descriptor *epin_fback_desc_comp = NULL;
	struct usb_ss_ep_comp_descriptor *ep_int_desc_comp = NULL;
	struct usb_endpoint_descriptor *epout_desc;
	struct usb_endpoint_descriptor *epin_desc;
	struct usb_endpoint_descriptor *epin_fback_desc;
	struct usb_endpoint_descriptor *ep_int_desc;
	int i;

	switch (speed) {
	case USB_SPEED_FULL:
		epout_desc = &fs_epout_desc;
		epin_desc = &fs_epin_desc;
		epin_fback_desc = &fs_epin_fback_desc;
		ep_int_desc = &fs_ep_int_desc;
		break;
	case USB_SPEED_HIGH:
		epout_desc = &hs_epout_desc;
		epin_desc = &hs_epin_desc;
		epin_fback_desc = &hs_epin_fback_desc;
		ep_int_desc = &hs_ep_int_desc;
		break;
	default:
		epout_desc = &ss_epout_desc;
		epin_desc = &ss_epin_desc;
		epout_desc_comp = &ss_epout_desc_comp;
		epin_desc_comp = &ss_epin_desc_comp;
		epin_fback_desc = &ss_epin_fback_desc;
		epin_fback_desc_comp = &ss_epin_fback_desc_comp;
		ep_int_desc = &ss_ep_int_desc;
		ep_int_desc_comp = &ss_ep_int_desc_comp;
	}

	i = 0;
	headers[i++] = USBDHDR(&iad_desc);
	headers[i++] = USBDHDR(&std_ac_if_desc);
	headers[i++] = USBDHDR(&ac_hdr_desc);
	if (EPIN_EN(opts))
		headers[i++] = USBDHDR(&in_clk_src_desc);
	if (EPOUT_EN(opts)) {
		headers[i++] = USBDHDR(&out_clk_src_desc);
		headers[i++] = USBDHDR(&usb_out_it_desc);

		if (FUOUT_EN(opts))
			headers[i++] = USBDHDR(out_feature_unit_desc);
	}

	if (EPIN_EN(opts)) {
		headers[i++] = USBDHDR(&io_in_it_desc);

		if (FUIN_EN(opts))
			headers[i++] = USBDHDR(in_feature_unit_desc);

		headers[i++] = USBDHDR(&usb_in_ot_desc);
	}

	if (EPOUT_EN(opts))
		headers[i++] = USBDHDR(&io_out_ot_desc);

	if (FUOUT_EN(opts) || FUIN_EN(opts)) {
		headers[i++] = USBDHDR(ep_int_desc);
		if (ep_int_desc_comp)
			headers[i++] = USBDHDR(ep_int_desc_comp);
	}

	if (EPOUT_EN(opts)) {
		headers[i++] = USBDHDR(&std_as_out_if0_desc);
		headers[i++] = USBDHDR(&std_as_out_if1_desc);
		headers[i++] = USBDHDR(&as_out_hdr_desc);
		headers[i++] = USBDHDR(&as_out_fmt1_desc);
		headers[i++] = USBDHDR(epout_desc);
		if (epout_desc_comp)
			headers[i++] = USBDHDR(epout_desc_comp);

		headers[i++] = USBDHDR(&as_iso_out_desc);

		if (EPOUT_FBACK_IN_EN(opts)) {
			headers[i++] = USBDHDR(epin_fback_desc);
			if (epin_fback_desc_comp)
				headers[i++] = USBDHDR(epin_fback_desc_comp);
		}
	}

	if (EPIN_EN(opts)) {
		headers[i++] = USBDHDR(&std_as_in_if0_desc);
		headers[i++] = USBDHDR(&std_as_in_if1_desc);
		headers[i++] = USBDHDR(&as_in_hdr_desc);
		headers[i++] = USBDHDR(&as_in_fmt1_desc);
		headers[i++] = USBDHDR(epin_desc);
		if (epin_desc_comp)
			headers[i++] = USBDHDR(epin_desc_comp);

		headers[i++] = USBDHDR(&as_iso_in_desc);
	}
	headers[i] = NULL;
}

static void setup_descriptor(struct f_uac2_opts *opts)
{
	/* patch descriptors */
	int i = 1; /* ID's start with 1 */

	if (EPOUT_EN(opts))
		usb_out_it_desc.bTerminalID = i++;
	if (EPIN_EN(opts))
		io_in_it_desc.bTerminalID = i++;
	if (EPOUT_EN(opts))
		io_out_ot_desc.bTerminalID = i++;
	if (EPIN_EN(opts))
		usb_in_ot_desc.bTerminalID = i++;
	if (FUOUT_EN(opts))
		out_feature_unit_desc->bUnitID = i++;
	if (FUIN_EN(opts))
		in_feature_unit_desc->bUnitID = i++;
	if (EPOUT_EN(opts))
		out_clk_src_desc.bClockID = i++;
	if (EPIN_EN(opts))
		in_clk_src_desc.bClockID = i++;

	usb_out_it_desc.bCSourceID = out_clk_src_desc.bClockID;

	if (FUIN_EN(opts)) {
		usb_in_ot_desc.bSourceID = in_feature_unit_desc->bUnitID;
		in_feature_unit_desc->bSourceID = io_in_it_desc.bTerminalID;
	} else {
		usb_in_ot_desc.bSourceID = io_in_it_desc.bTerminalID;
	}

	usb_in_ot_desc.bCSourceID = in_clk_src_desc.bClockID;
	io_in_it_desc.bCSourceID = in_clk_src_desc.bClockID;
	io_out_ot_desc.bCSourceID = out_clk_src_desc.bClockID;

	if (FUOUT_EN(opts)) {
		io_out_ot_desc.bSourceID = out_feature_unit_desc->bUnitID;
		out_feature_unit_desc->bSourceID = usb_out_it_desc.bTerminalID;
	} else {
		io_out_ot_desc.bSourceID = usb_out_it_desc.bTerminalID;
	}

	as_out_hdr_desc.bTerminalLink = usb_out_it_desc.bTerminalID;
	as_in_hdr_desc.bTerminalLink = usb_in_ot_desc.bTerminalID;

	iad_desc.bInterfaceCount = 1;
	ac_hdr_desc.wTotalLength = cpu_to_le16(sizeof(ac_hdr_desc));

	if (EPIN_EN(opts)) {
		u16 len = le16_to_cpu(ac_hdr_desc.wTotalLength);

		len += sizeof(in_clk_src_desc);
		len += sizeof(usb_in_ot_desc);

		if (FUIN_EN(opts))
			len += in_feature_unit_desc->bLength;

		len += sizeof(io_in_it_desc);
		ac_hdr_desc.wTotalLength = cpu_to_le16(len);
		iad_desc.bInterfaceCount++;
	}
	if (EPOUT_EN(opts)) {
		u16 len = le16_to_cpu(ac_hdr_desc.wTotalLength);

		len += sizeof(out_clk_src_desc);
		len += sizeof(usb_out_it_desc);

		if (FUOUT_EN(opts))
			len += out_feature_unit_desc->bLength;

		len += sizeof(io_out_ot_desc);
		ac_hdr_desc.wTotalLength = cpu_to_le16(len);
		iad_desc.bInterfaceCount++;
	}

	io_in_it_desc.wTerminalType = cpu_to_le16(opts->c_terminal_type);
	io_out_ot_desc.wTerminalType = cpu_to_le16(opts->p_terminal_type);

	setup_headers(opts, fs_audio_desc, USB_SPEED_FULL);
	setup_headers(opts, hs_audio_desc, USB_SPEED_HIGH);
	setup_headers(opts, ss_audio_desc, USB_SPEED_SUPER);
}

static int afunc_validate_opts(struct g_audio *agdev, struct device *dev)
{
	struct f_uac2_opts *opts = g_audio_to_uac2_opts(agdev);
	const char *msg = NULL;

	if (!opts->p_chmask && !opts->c_chmask)
		msg = "no playback and capture channels";
	else if (opts->p_chmask & ~UAC2_CHANNEL_MASK)
		msg = "unsupported playback channels mask";
	else if (opts->c_chmask & ~UAC2_CHANNEL_MASK)
		msg = "unsupported capture channels mask";
	else if ((opts->p_ssize < 1) || (opts->p_ssize > 4))
		msg = "incorrect playback sample size";
	else if ((opts->c_ssize < 1) || (opts->c_ssize > 4))
		msg = "incorrect capture sample size";
	else if (!opts->p_srates[0])
		msg = "incorrect playback sampling rate";
	else if (!opts->c_srates[0])
		msg = "incorrect capture sampling rate";

	else if (opts->p_volume_max <= opts->p_volume_min)
		msg = "incorrect playback volume max/min";
	else if (opts->c_volume_max <= opts->c_volume_min)
		msg = "incorrect capture volume max/min";
	else if (opts->p_volume_res <= 0)
		msg = "negative/zero playback volume resolution";
	else if (opts->c_volume_res <= 0)
		msg = "negative/zero capture volume resolution";

	else if ((opts->p_volume_max - opts->p_volume_min) % opts->p_volume_res)
		msg = "incorrect playback volume resolution";
	else if ((opts->c_volume_max - opts->c_volume_min) % opts->c_volume_res)
		msg = "incorrect capture volume resolution";

	else if ((opts->p_hs_bint < 0) || (opts->p_hs_bint > 4))
		msg = "incorrect playback HS/SS bInterval (1-4: fixed, 0: auto)";
	else if ((opts->c_hs_bint < 0) || (opts->c_hs_bint > 4))
		msg = "incorrect capture HS/SS bInterval (1-4: fixed, 0: auto)";

	if (msg) {
		dev_err(dev, "Error: %s\n", msg);
		return -EINVAL;
	}

	return 0;
}

/*-------------------------------------------------------------------------*/

/*
 * Configfs alt mode handling
 */

static void init_alt_0_opts(struct f_uac2_alt_0_opts *alt_0_opts,
			    struct f_uac2_opts *opts, int playback)
{
	alt_0_opts->c.opts = opts;
	alt_0_opts->c.alt_num = 0;

	// Note: Strings are from the host perspective, opt prefixes are from the device perspective
	scnprintf(alt_0_opts->name, sizeof(alt_0_opts->name),
		  (!playback) ? "Playback Inactive" : "Capture Inactive");
}

static void init_alt_opts(struct f_uac2_alt_opts *alt_opts, struct f_uac2_opts *opts,
			  int alt_num, int playback)
{
	alt_opts->c.opts = opts;
	alt_opts->c.alt_num = alt_num;

	INIT_LIST_HEAD(&alt_opts->list);

	// Note: Strings are from the host perspective, opt prefixes are from the device perspective
	scnprintf(alt_opts->name, sizeof(alt_opts->name),
		  (!playback) ? "Playback Active" : "Capture Active");
	strscpy(alt_opts->it_name, (playback) ? opts->p_it_name : opts->c_it_name,
		sizeof(alt_opts->it_name));
	strscpy(alt_opts->it_ch_name, (playback) ? opts->p_it_ch_name : opts->c_it_ch_name,
		sizeof(alt_opts->it_ch_name));
	strscpy(alt_opts->ot_name, (playback) ? opts->p_ot_name : opts->c_ot_name,
		sizeof(alt_opts->ot_name));
	strscpy(alt_opts->fu_vol_name, (playback) ? opts->p_fu_vol_name : opts->c_fu_vol_name,
		sizeof(alt_opts->fu_vol_name));

	/* Copy default options from the main opts */
	alt_opts->chmask = (playback) ? opts->p_chmask : opts->c_chmask;
	alt_opts->ssize = (playback) ? opts->p_ssize : opts->c_ssize;
	alt_opts->sync = (playback) ? USB_ENDPOINT_SYNC_ASYNC : opts->c_sync; /* only for capture */
	alt_opts->hs_bint = (playback) ? opts->p_hs_bint : opts->c_hs_bint;

	/* NOTE: These are backwards with relation to other c_/p_ settings in the existing
	 * userspace API. Correct terminal type is copied into c/p_alt.x
	 * (eg p_termial_type == c_alt.x/terminal_type)
	 */
	alt_opts->terminal_type = (!playback) ? opts->p_terminal_type : opts->c_terminal_type;
}


static int
afunc_bind(struct usb_configuration *cfg, struct usb_function *fn)
{
	struct f_uac2 *uac2 = func_to_uac2(fn);
	struct g_audio *agdev = func_to_g_audio(fn);
	struct usb_composite_dev *cdev = cfg->cdev;
	struct usb_gadget *gadget = cdev->gadget;
	struct device *dev = &gadget->dev;
	struct f_uac2_opts *uac2_opts = g_audio_to_uac2_opts(agdev);
	struct usb_string *us;
	int ret;

	/* Copy main options to alt modes 0/1 if the groups don't exist
	 * before validation since they will be checked.
	 */
	if (!uac2_opts->c_alt_0_opts.c.group.cg_item.ci_name)
		init_alt_0_opts(&uac2_opts->c_alt_0_opts, uac2_opts, HOST_TO_DEVICE);
	if (!uac2_opts->p_alt_0_opts.c.group.cg_item.ci_name)
		init_alt_0_opts(&uac2_opts->p_alt_0_opts, uac2_opts, DEVICE_TO_HOST);
	if (!uac2_opts->c_alt_1_opts.c.group.cg_item.ci_name)
		init_alt_opts(&uac2_opts->c_alt_1_opts, uac2_opts, 1, HOST_TO_DEVICE);
	if (!uac2_opts->p_alt_1_opts.c.group.cg_item.ci_name)
		init_alt_opts(&uac2_opts->p_alt_1_opts, uac2_opts, 1, DEVICE_TO_HOST);

	ret = afunc_validate_opts(agdev, dev);
	if (ret)
		return ret;

	/* Past this point, all settings that apply to an alt mode should
	 * be used from their alt mode opts.
	 */

	us = attach_strings(cdev, uac2_opts);
	if (IS_ERR(us))
		return PTR_ERR(us);

	if (FUOUT_EN(uac2_opts)) {
		out_feature_unit_desc = build_fu_desc(uac2_opts->c_chmask);
		if (!out_feature_unit_desc)
			return -ENOMEM;
	}
	if (FUIN_EN(uac2_opts)) {
		in_feature_unit_desc = build_fu_desc(uac2_opts->p_chmask);
		if (!in_feature_unit_desc) {
			ret = -ENOMEM;
			goto err_free_fu;
		}
	}

	iad_desc.iFunction = add_string(us, uac2_opts->function_name);
	std_ac_if_desc.iInterface = add_string(us, uac2_opts->if_ctrl_name);
	in_clk_src_desc.iClockSource = add_string(us, uac2_opts->clksrc_in_name);
	out_clk_src_desc.iClockSource = add_string(us, uac2_opts->clksrc_out_name);
	usb_out_it_desc.iTerminal = add_string(us, uac2_opts->c_alt_1_opts.it_name);
	usb_out_it_desc.iChannelNames = add_string(us, uac2_opts->c_alt_1_opts.it_ch_name);
	io_in_it_desc.iTerminal = add_string(us, uac2_opts->p_alt_1_opts.it_name);
	io_in_it_desc.iChannelNames = add_string(us, uac2_opts->p_alt_1_opts.it_ch_name);
	usb_in_ot_desc.iTerminal = add_string(us, uac2_opts->p_alt_1_opts.ot_name);
	io_out_ot_desc.iTerminal = add_string(us, uac2_opts->c_alt_1_opts.ot_name);
	std_as_out_if0_desc.iInterface = add_string(us, uac2_opts->c_alt_0_opts.name);
	std_as_out_if1_desc.iInterface = add_string(us, uac2_opts->c_alt_1_opts.name);
	std_as_in_if0_desc.iInterface = add_string(us, uac2_opts->p_alt_0_opts.name);
	std_as_in_if1_desc.iInterface = add_string(us, uac2_opts->p_alt_0_opts.name);

	if (FUOUT_EN(uac2_opts)) {
		u8 *i_feature = (u8 *)out_feature_unit_desc +
				out_feature_unit_desc->bLength - 1;
		*i_feature = add_string(us, uac2_opts->c_alt_1_opts.fu_vol_name);
	}
	if (FUIN_EN(uac2_opts)) {
		u8 *i_feature = (u8 *)in_feature_unit_desc +
				in_feature_unit_desc->bLength - 1;
		*i_feature = add_string(us, uac2_opts->p_alt_1_opts.fu_vol_name);
	}


	/* Initialize the configurable parameters */
	usb_out_it_desc.bNrChannels = num_channels(uac2_opts->c_chmask);
	usb_out_it_desc.bmChannelConfig = cpu_to_le32(uac2_opts->c_chmask);
	io_in_it_desc.bNrChannels = num_channels(uac2_opts->p_chmask);
	io_in_it_desc.bmChannelConfig = cpu_to_le32(uac2_opts->p_chmask);
	as_out_hdr_desc.bNrChannels = num_channels(uac2_opts->c_chmask);
	as_out_hdr_desc.bmChannelConfig = cpu_to_le32(uac2_opts->c_chmask);
	as_in_hdr_desc.bNrChannels = num_channels(uac2_opts->p_chmask);
	as_in_hdr_desc.bmChannelConfig = cpu_to_le32(uac2_opts->p_chmask);
	as_out_fmt1_desc.bSubslotSize = uac2_opts->c_ssize;
	as_out_fmt1_desc.bBitResolution = uac2_opts->c_ssize * 8;
	as_in_fmt1_desc.bSubslotSize = uac2_opts->p_ssize;
	as_in_fmt1_desc.bBitResolution = uac2_opts->p_ssize * 8;
	if (FUOUT_EN(uac2_opts)) {
		__le32 *bma = (__le32 *)&out_feature_unit_desc->bmaControls[0];
		u32 control = 0;

		if (uac2_opts->c_mute_present)
			control |= CONTROL_RDWR << FU_MUTE_CTRL;
		if (uac2_opts->c_volume_present)
			control |= CONTROL_RDWR << FU_VOL_CTRL;
		*bma = cpu_to_le32(control);
	}
	if (FUIN_EN(uac2_opts)) {
		__le32 *bma = (__le32 *)&in_feature_unit_desc->bmaControls[0];
		u32 control = 0;

		if (uac2_opts->p_mute_present)
			control |= CONTROL_RDWR << FU_MUTE_CTRL;
		if (uac2_opts->p_volume_present)
			control |= CONTROL_RDWR << FU_VOL_CTRL;
		*bma = cpu_to_le32(control);
	}

	ret = usb_interface_id(cfg, fn);
	if (ret < 0) {
		dev_err(dev, "%s:%d Error!\n", __func__, __LINE__);
		goto err_free_fu;
	}
	iad_desc.bFirstInterface = ret;

	std_ac_if_desc.bInterfaceNumber = ret;
	uac2->ac_intf = ret;
	uac2->ac_alt = 0;

	if (EPOUT_EN(uac2_opts)) {
		ret = usb_interface_id(cfg, fn);
		if (ret < 0) {
			dev_err(dev, "%s:%d Error!\n", __func__, __LINE__);
			goto err_free_fu;
		}
		std_as_out_if0_desc.bInterfaceNumber = ret;
		std_as_out_if1_desc.bInterfaceNumber = ret;
		std_as_out_if1_desc.bNumEndpoints = 1;
		uac2->as_out_intf = ret;
		uac2->as_out_alt = 0;

		if (EPOUT_FBACK_IN_EN(uac2_opts)) {
			fs_epout_desc.bmAttributes =
			  USB_ENDPOINT_XFER_ISOC | USB_ENDPOINT_SYNC_ASYNC;
			hs_epout_desc.bmAttributes =
			  USB_ENDPOINT_XFER_ISOC | USB_ENDPOINT_SYNC_ASYNC;
			ss_epout_desc.bmAttributes =
			  USB_ENDPOINT_XFER_ISOC | USB_ENDPOINT_SYNC_ASYNC;
			std_as_out_if1_desc.bNumEndpoints++;
		} else {
			fs_epout_desc.bmAttributes =
			  USB_ENDPOINT_XFER_ISOC | USB_ENDPOINT_SYNC_ADAPTIVE;
			hs_epout_desc.bmAttributes =
			  USB_ENDPOINT_XFER_ISOC | USB_ENDPOINT_SYNC_ADAPTIVE;
			ss_epout_desc.bmAttributes =
			  USB_ENDPOINT_XFER_ISOC | USB_ENDPOINT_SYNC_ADAPTIVE;
		}
	}

	if (EPIN_EN(uac2_opts)) {
		ret = usb_interface_id(cfg, fn);
		if (ret < 0) {
			dev_err(dev, "%s:%d Error!\n", __func__, __LINE__);
			goto err_free_fu;
		}
		std_as_in_if0_desc.bInterfaceNumber = ret;
		std_as_in_if1_desc.bInterfaceNumber = ret;
		uac2->as_in_intf = ret;
		uac2->as_in_alt = 0;
	}

	if (FUOUT_EN(uac2_opts) || FUIN_EN(uac2_opts)) {
		uac2->int_ep = usb_ep_autoconfig(gadget, &fs_ep_int_desc);
		if (!uac2->int_ep) {
			dev_err(dev, "%s:%d Error!\n", __func__, __LINE__);
			ret = -ENODEV;
			goto err_free_fu;
		}

		std_ac_if_desc.bNumEndpoints = 1;
	}

	hs_epin_desc.bInterval = uac2_opts->p_hs_bint;
	ss_epin_desc.bInterval = uac2_opts->p_hs_bint;
	hs_epout_desc.bInterval = uac2_opts->c_hs_bint;
	ss_epout_desc.bInterval = uac2_opts->c_hs_bint;

	/* Calculate wMaxPacketSize according to audio bandwidth */
	ret = set_ep_max_packet_size_bint(dev, &uac2_opts->p_alt_1_opts, &fs_epin_desc,
					USB_SPEED_FULL, true);
	if (ret < 0) {
		dev_err(dev, "%s:%d Error!\n", __func__, __LINE__);
		return ret;
	}

	ret = set_ep_max_packet_size_bint(dev, &uac2_opts->c_alt_1_opts, &fs_epout_desc,
					USB_SPEED_FULL, false);
	if (ret < 0) {
		dev_err(dev, "%s:%d Error!\n", __func__, __LINE__);
		return ret;
	}

	ret = set_ep_max_packet_size_bint(dev, &uac2_opts->p_alt_1_opts, &hs_epin_desc,
					USB_SPEED_HIGH, true);
	if (ret < 0) {
		dev_err(dev, "%s:%d Error!\n", __func__, __LINE__);
		return ret;
	}

	ret = set_ep_max_packet_size_bint(dev, &uac2_opts->c_alt_1_opts, &hs_epout_desc,
					USB_SPEED_HIGH, false);
	if (ret < 0) {
		dev_err(dev, "%s:%d Error!\n", __func__, __LINE__);
		return ret;
	}

	ret = set_ep_max_packet_size_bint(dev, &uac2_opts->p_alt_1_opts, &ss_epin_desc,
					USB_SPEED_SUPER, true);
	if (ret < 0) {
		dev_err(dev, "%s:%d Error!\n", __func__, __LINE__);
		return ret;
	}

	ret = set_ep_max_packet_size_bint(dev, &uac2_opts->c_alt_1_opts, &ss_epout_desc,
					USB_SPEED_SUPER, false);
	if (ret < 0) {
		dev_err(dev, "%s:%d Error!\n", __func__, __LINE__);
		return ret;
	}

	if (EPOUT_EN(uac2_opts)) {
		agdev->out_ep = usb_ep_autoconfig(gadget, &fs_epout_desc);
		if (!agdev->out_ep) {
			dev_err(dev, "%s:%d Error!\n", __func__, __LINE__);
			ret = -ENODEV;
			goto err_free_fu;
		}
		if (EPOUT_FBACK_IN_EN(uac2_opts)) {
			agdev->in_ep_fback = usb_ep_autoconfig(gadget,
						       &fs_epin_fback_desc);
			if (!agdev->in_ep_fback) {
				dev_err(dev, "%s:%d Error!\n",
					__func__, __LINE__);
				ret = -ENODEV;
				goto err_free_fu;
			}
		}
	}

	if (EPIN_EN(uac2_opts)) {
		agdev->in_ep = usb_ep_autoconfig(gadget, &fs_epin_desc);
		if (!agdev->in_ep) {
			dev_err(dev, "%s:%d Error!\n", __func__, __LINE__);
			ret = -ENODEV;
			goto err_free_fu;
		}
	}

	agdev->in_ep_maxpsize = max_t(u16,
				le16_to_cpu(fs_epin_desc.wMaxPacketSize),
				le16_to_cpu(hs_epin_desc.wMaxPacketSize));
	agdev->out_ep_maxpsize = max_t(u16,
				le16_to_cpu(fs_epout_desc.wMaxPacketSize),
				le16_to_cpu(hs_epout_desc.wMaxPacketSize));

	agdev->in_ep_maxpsize = max_t(u16, agdev->in_ep_maxpsize,
				le16_to_cpu(ss_epin_desc.wMaxPacketSize));
	agdev->out_ep_maxpsize = max_t(u16, agdev->out_ep_maxpsize,
				le16_to_cpu(ss_epout_desc.wMaxPacketSize));

	ss_epin_desc_comp.wBytesPerInterval = ss_epin_desc.wMaxPacketSize;
	ss_epout_desc_comp.wBytesPerInterval = ss_epout_desc.wMaxPacketSize;

	// HS and SS endpoint addresses are copied from autoconfigured FS descriptors
	hs_ep_int_desc.bEndpointAddress = fs_ep_int_desc.bEndpointAddress;
	hs_epout_desc.bEndpointAddress = fs_epout_desc.bEndpointAddress;
	hs_epin_fback_desc.bEndpointAddress = fs_epin_fback_desc.bEndpointAddress;
	hs_epin_desc.bEndpointAddress = fs_epin_desc.bEndpointAddress;
	ss_epout_desc.bEndpointAddress = fs_epout_desc.bEndpointAddress;
	ss_epin_fback_desc.bEndpointAddress = fs_epin_fback_desc.bEndpointAddress;
	ss_epin_desc.bEndpointAddress = fs_epin_desc.bEndpointAddress;
	ss_ep_int_desc.bEndpointAddress = fs_ep_int_desc.bEndpointAddress;

	setup_descriptor(uac2_opts);

	ret = usb_assign_descriptors(fn, fs_audio_desc, hs_audio_desc, ss_audio_desc,
				     ss_audio_desc);
	if (ret)
		goto err_free_fu;

	agdev->gadget = gadget;

	agdev->params.p_chmask = uac2_opts->p_chmask;
	memcpy(agdev->params.p_srates, uac2_opts->p_srates,
			sizeof(agdev->params.p_srates));
	agdev->params.p_ssize = uac2_opts->p_ssize;
	if (FUIN_EN(uac2_opts)) {
		agdev->params.p_fu.id = USB_IN_FU_ID;
		agdev->params.p_fu.mute_present = uac2_opts->p_mute_present;
		agdev->params.p_fu.volume_present = uac2_opts->p_volume_present;
		agdev->params.p_fu.volume_min = uac2_opts->p_volume_min;
		agdev->params.p_fu.volume_max = uac2_opts->p_volume_max;
		agdev->params.p_fu.volume_res = uac2_opts->p_volume_res;
	}
	agdev->params.c_chmask = uac2_opts->c_chmask;
	memcpy(agdev->params.c_srates, uac2_opts->c_srates,
			sizeof(agdev->params.c_srates));
	agdev->params.c_ssize = uac2_opts->c_ssize;
	if (FUOUT_EN(uac2_opts)) {
		agdev->params.c_fu.id = USB_OUT_FU_ID;
		agdev->params.c_fu.mute_present = uac2_opts->c_mute_present;
		agdev->params.c_fu.volume_present = uac2_opts->c_volume_present;
		agdev->params.c_fu.volume_min = uac2_opts->c_volume_min;
		agdev->params.c_fu.volume_max = uac2_opts->c_volume_max;
		agdev->params.c_fu.volume_res = uac2_opts->c_volume_res;
	}
	agdev->params.req_number = uac2_opts->req_number;
	agdev->params.fb_max = uac2_opts->fb_max;

	if (FUOUT_EN(uac2_opts) || FUIN_EN(uac2_opts))
    agdev->notify = afunc_notify;

	ret = g_audio_setup(agdev, "UAC2 PCM", "UAC2_Gadget");
	if (ret)
		goto err_free_descs;

	return 0;

err_free_descs:
	usb_free_all_descriptors(fn);
	agdev->gadget = NULL;
err_free_fu:
	kfree(out_feature_unit_desc);
	out_feature_unit_desc = NULL;
	kfree(in_feature_unit_desc);
	in_feature_unit_desc = NULL;
	return ret;
}

static void
afunc_notify_complete(struct usb_ep *_ep, struct usb_request *req)
{
	struct g_audio *agdev = req->context;
	struct f_uac2 *uac2 = func_to_uac2(&agdev->func);

	atomic_dec(&uac2->int_count);
	kfree(req->buf);
	usb_ep_free_request(_ep, req);
}

static int
afunc_notify(struct g_audio *agdev, int unit_id, int cs)
{
	struct f_uac2 *uac2 = func_to_uac2(&agdev->func);
	struct usb_request *req;
	struct uac2_interrupt_data_msg *msg;
	u16 w_index, w_value;
	int ret;

	if (!uac2->int_ep->enabled)
		return 0;

	if (atomic_inc_return(&uac2->int_count) > UAC2_DEF_INT_REQ_NUM) {
		atomic_dec(&uac2->int_count);
		return 0;
	}

	req = usb_ep_alloc_request(uac2->int_ep, GFP_ATOMIC);
	if (req == NULL) {
		ret = -ENOMEM;
		goto err_dec_int_count;
	}

	msg = kzalloc(sizeof(*msg), GFP_ATOMIC);
	if (msg == NULL) {
		ret = -ENOMEM;
		goto err_free_request;
	}

	w_index = unit_id << 8 | uac2->ac_intf;
	w_value = cs << 8;

	msg->bInfo = 0; /* Non-vendor, interface interrupt */
	msg->bAttribute = UAC2_CS_CUR;
	msg->wIndex = cpu_to_le16(w_index);
	msg->wValue = cpu_to_le16(w_value);

	req->length = sizeof(*msg);
	req->buf = msg;
	req->context = agdev;
	req->complete = afunc_notify_complete;

	ret = usb_ep_queue(uac2->int_ep, req, GFP_ATOMIC);

	if (ret)
		goto err_free_msg;

	return 0;

err_free_msg:
	kfree(msg);
err_free_request:
	usb_ep_free_request(uac2->int_ep, req);
err_dec_int_count:
	atomic_dec(&uac2->int_count);

	return ret;
}

static int
afunc_set_alt(struct usb_function *fn, unsigned intf, unsigned alt)
{
	struct usb_composite_dev *cdev = fn->config->cdev;
	struct f_uac2 *uac2 = func_to_uac2(fn);
	struct g_audio *agdev = func_to_g_audio(fn);
	struct usb_gadget *gadget = cdev->gadget;
	struct device *dev = &gadget->dev;
	int ret = 0;

	/* No i/f has more than 2 alt settings */
	if (alt > 1) {
		dev_err(dev, "%s:%d Error!\n", __func__, __LINE__);
		return -EINVAL;
	}

	if (intf == uac2->ac_intf) {
		/* Control I/f has only 1 AltSetting - 0 */
		if (alt) {
			dev_err(dev, "%s:%d Error!\n", __func__, __LINE__);
			return -EINVAL;
		}

		/* restart interrupt endpoint */
		if (uac2->int_ep) {
			usb_ep_disable(uac2->int_ep);
			config_ep_by_speed(gadget, &agdev->func, uac2->int_ep);
			usb_ep_enable(uac2->int_ep);
		}

		return 0;
	}

	if (intf == uac2->as_out_intf) {
		uac2->as_out_alt = alt;

		if (alt)
			ret = u_audio_start_capture(&uac2->g_audio);
		else
			u_audio_stop_capture(&uac2->g_audio);
	} else if (intf == uac2->as_in_intf) {
		uac2->as_in_alt = alt;

		if (alt)
			ret = u_audio_start_playback(&uac2->g_audio);
		else
			u_audio_stop_playback(&uac2->g_audio);
	} else {
		dev_err(dev, "%s:%d Error!\n", __func__, __LINE__);
		return -EINVAL;
	}

	return ret;
}

static int
afunc_get_alt(struct usb_function *fn, unsigned intf)
{
	struct f_uac2 *uac2 = func_to_uac2(fn);
	struct g_audio *agdev = func_to_g_audio(fn);

	if (intf == uac2->ac_intf)
		return uac2->ac_alt;
	else if (intf == uac2->as_out_intf)
		return uac2->as_out_alt;
	else if (intf == uac2->as_in_intf)
		return uac2->as_in_alt;
	else
		dev_err(&agdev->gadget->dev,
			"%s:%d Invalid Interface %d!\n",
			__func__, __LINE__, intf);

	return -EINVAL;
}

static void
afunc_disable(struct usb_function *fn)
{
	struct f_uac2 *uac2 = func_to_uac2(fn);

	uac2->as_in_alt = 0;
	uac2->as_out_alt = 0;
	u_audio_stop_capture(&uac2->g_audio);
	u_audio_stop_playback(&uac2->g_audio);
	if (uac2->int_ep)
		usb_ep_disable(uac2->int_ep);
}

static void
afunc_suspend(struct usb_function *fn)
{
	struct f_uac2 *uac2 = func_to_uac2(fn);

	u_audio_suspend(&uac2->g_audio);
}

static int
in_rq_cur(struct usb_function *fn, const struct usb_ctrlrequest *cr)
{
	struct usb_request *req = fn->config->cdev->req;
	struct g_audio *agdev = func_to_g_audio(fn);
	struct f_uac2_opts *opts = g_audio_to_uac2_opts(agdev);
	u16 w_length = le16_to_cpu(cr->wLength);
	u16 w_index = le16_to_cpu(cr->wIndex);
	u16 w_value = le16_to_cpu(cr->wValue);
	u8 entity_id = (w_index >> 8) & 0xff;
	u8 control_selector = w_value >> 8;
	int value = -EOPNOTSUPP;
	u32 p_srate, c_srate;

	u_audio_get_playback_srate(agdev, &p_srate);
	u_audio_get_capture_srate(agdev, &c_srate);

	if ((entity_id == USB_IN_CLK_ID) || (entity_id == USB_OUT_CLK_ID)) {
		if (control_selector == UAC2_CS_CONTROL_SAM_FREQ) {
			struct cntrl_cur_lay3 c;

			memset(&c, 0, sizeof(struct cntrl_cur_lay3));

			if (entity_id == USB_IN_CLK_ID)
				c.dCUR = cpu_to_le32(p_srate);
			else if (entity_id == USB_OUT_CLK_ID)
				c.dCUR = cpu_to_le32(c_srate);

			value = min_t(unsigned int, w_length, sizeof(c));
			memcpy(req->buf, &c, value);
		} else if (control_selector == UAC2_CS_CONTROL_CLOCK_VALID) {
			*(u8 *)req->buf = 1;
			value = min_t(unsigned int, w_length, 1);
		} else {
			dev_err(&agdev->gadget->dev,
				"%s:%d control_selector=%d TODO!\n",
				__func__, __LINE__, control_selector);
		}
	} else if ((FUIN_EN(opts) && (entity_id == USB_IN_FU_ID)) ||
			(FUOUT_EN(opts) && (entity_id == USB_OUT_FU_ID))) {
		unsigned int is_playback = 0;

		if (FUIN_EN(opts) && (entity_id == USB_IN_FU_ID))
			is_playback = 1;

		if (control_selector == UAC_FU_MUTE) {
			unsigned int mute;

			u_audio_get_mute(agdev, is_playback, &mute);

			*(u8 *)req->buf = mute;
			value = min_t(unsigned int, w_length, 1);
		} else if (control_selector == UAC_FU_VOLUME) {
			struct cntrl_cur_lay2 c;
			s16 volume;

			memset(&c, 0, sizeof(struct cntrl_cur_lay2));

			u_audio_get_volume(agdev, is_playback, &volume);
			c.wCUR = cpu_to_le16(volume);

			value = min_t(unsigned int, w_length, sizeof(c));
			memcpy(req->buf, &c, value);
		} else {
			dev_err(&agdev->gadget->dev,
				"%s:%d control_selector=%d TODO!\n",
				__func__, __LINE__, control_selector);
		}
	} else {
		dev_err(&agdev->gadget->dev,
			"%s:%d entity_id=%d control_selector=%d TODO!\n",
			__func__, __LINE__, entity_id, control_selector);
	}

	return value;
}

static int
in_rq_range(struct usb_function *fn, const struct usb_ctrlrequest *cr)
{
	struct usb_request *req = fn->config->cdev->req;
	struct g_audio *agdev = func_to_g_audio(fn);
	struct f_uac2_opts *opts = g_audio_to_uac2_opts(agdev);
	u16 w_length = le16_to_cpu(cr->wLength);
	u16 w_index = le16_to_cpu(cr->wIndex);
	u16 w_value = le16_to_cpu(cr->wValue);
	u8 entity_id = (w_index >> 8) & 0xff;
	u8 control_selector = w_value >> 8;
	int value = -EOPNOTSUPP;

	if ((entity_id == USB_IN_CLK_ID) || (entity_id == USB_OUT_CLK_ID)) {
		if (control_selector == UAC2_CS_CONTROL_SAM_FREQ) {
			struct cntrl_ranges_lay3_srates rs;
			int i;
			int wNumSubRanges = 0;
			int srate;
			int *srates;

			if (entity_id == USB_IN_CLK_ID)
				srates = opts->p_srates;
			else if (entity_id == USB_OUT_CLK_ID)
				srates = opts->c_srates;
			else
				return -EOPNOTSUPP;
			for (i = 0; i < UAC_MAX_RATES; i++) {
				srate = srates[i];
				if (srate == 0)
					break;

				rs.r[wNumSubRanges].dMIN = cpu_to_le32(srate);
				rs.r[wNumSubRanges].dMAX = cpu_to_le32(srate);
				rs.r[wNumSubRanges].dRES = 0;
				wNumSubRanges++;
				dev_dbg(&agdev->gadget->dev,
					"%s(): clk %d: rate ID %d: %d\n",
					__func__, entity_id, wNumSubRanges, srate);
			}
			rs.wNumSubRanges = cpu_to_le16(wNumSubRanges);
			value = min_t(unsigned int, w_length, ranges_lay3_size(rs));
			dev_dbg(&agdev->gadget->dev, "%s(): sending %d rates, size %d\n",
				__func__, rs.wNumSubRanges, value);
			memcpy(req->buf, &rs, value);
		} else {
			dev_err(&agdev->gadget->dev,
				"%s:%d control_selector=%d TODO!\n",
				__func__, __LINE__, control_selector);
		}
	} else if ((FUIN_EN(opts) && (entity_id == USB_IN_FU_ID)) ||
			(FUOUT_EN(opts) && (entity_id == USB_OUT_FU_ID))) {
		unsigned int is_playback = 0;

		if (FUIN_EN(opts) && (entity_id == USB_IN_FU_ID))
			is_playback = 1;

		if (control_selector == UAC_FU_VOLUME) {
			struct cntrl_range_lay2 r;
			s16 max_db, min_db, res_db;

			if (is_playback) {
				max_db = opts->p_volume_max;
				min_db = opts->p_volume_min;
				res_db = opts->p_volume_res;
			} else {
				max_db = opts->c_volume_max;
				min_db = opts->c_volume_min;
				res_db = opts->c_volume_res;
			}

			r.wMAX = cpu_to_le16(max_db);
			r.wMIN = cpu_to_le16(min_db);
			r.wRES = cpu_to_le16(res_db);
			r.wNumSubRanges = cpu_to_le16(1);

			value = min_t(unsigned int, w_length, sizeof(r));
			memcpy(req->buf, &r, value);
		} else {
			dev_err(&agdev->gadget->dev,
				"%s:%d control_selector=%d TODO!\n",
				__func__, __LINE__, control_selector);
		}
	} else {
		dev_err(&agdev->gadget->dev,
			"%s:%d entity_id=%d control_selector=%d TODO!\n",
			__func__, __LINE__, entity_id, control_selector);
	}

	return value;
}

static int
ac_rq_in(struct usb_function *fn, const struct usb_ctrlrequest *cr)
{
	if (cr->bRequest == UAC2_CS_CUR)
		return in_rq_cur(fn, cr);
	else if (cr->bRequest == UAC2_CS_RANGE)
		return in_rq_range(fn, cr);
	else
		return -EOPNOTSUPP;
}

static void uac2_cs_control_sam_freq(struct usb_ep *ep, struct usb_request *req)
{
	struct usb_function *fn = ep->driver_data;
	struct g_audio *agdev = func_to_g_audio(fn);
	struct f_uac2 *uac2 = func_to_uac2(fn);
	u32 val;

	if (req->actual != 4)
		return;

	val = le32_to_cpu(*((__le32 *)req->buf));
	dev_dbg(&agdev->gadget->dev, "%s val: %d.\n", __func__, val);
	if (uac2->clock_id == USB_IN_CLK_ID) {
		u_audio_set_playback_srate(agdev, val);
	} else if (uac2->clock_id == USB_OUT_CLK_ID) {
		u_audio_set_capture_srate(agdev, val);
	}
}

static void
out_rq_cur_complete(struct usb_ep *ep, struct usb_request *req)
{
	struct g_audio *agdev = req->context;
	struct usb_composite_dev *cdev = agdev->func.config->cdev;
	struct f_uac2_opts *opts = g_audio_to_uac2_opts(agdev);
	struct f_uac2 *uac2 = func_to_uac2(&agdev->func);
	struct usb_ctrlrequest *cr = &uac2->setup_cr;
	u16 w_index = le16_to_cpu(cr->wIndex);
	u16 w_value = le16_to_cpu(cr->wValue);
	u8 entity_id = (w_index >> 8) & 0xff;
	u8 control_selector = w_value >> 8;

	if (req->status != 0) {
		dev_dbg(&cdev->gadget->dev, "completion err %d\n", req->status);
		return;
	}

	if ((FUIN_EN(opts) && (entity_id == USB_IN_FU_ID)) ||
		(FUOUT_EN(opts) && (entity_id == USB_OUT_FU_ID))) {
		unsigned int is_playback = 0;

		if (FUIN_EN(opts) && (entity_id == USB_IN_FU_ID))
			is_playback = 1;

		if (control_selector == UAC_FU_MUTE) {
			u8 mute = *(u8 *)req->buf;

			u_audio_set_mute(agdev, is_playback, mute);

			return;
		} else if (control_selector == UAC_FU_VOLUME) {
			struct cntrl_cur_lay2 *c = req->buf;
			s16 volume;

			volume = le16_to_cpu(c->wCUR);
			u_audio_set_volume(agdev, is_playback, volume);

			return;
		} else {
			dev_err(&agdev->gadget->dev,
				"%s:%d control_selector=%d TODO!\n",
				__func__, __LINE__, control_selector);
			usb_ep_set_halt(ep);
		}
	}
}

static int
out_rq_cur(struct usb_function *fn, const struct usb_ctrlrequest *cr)
{
	struct usb_composite_dev *cdev = fn->config->cdev;
	struct usb_request *req = fn->config->cdev->req;
	struct g_audio *agdev = func_to_g_audio(fn);
	struct f_uac2_opts *opts = g_audio_to_uac2_opts(agdev);
	struct f_uac2 *uac2 = func_to_uac2(fn);
	u16 w_length = le16_to_cpu(cr->wLength);
	u16 w_index = le16_to_cpu(cr->wIndex);
	u16 w_value = le16_to_cpu(cr->wValue);
	u8 entity_id = (w_index >> 8) & 0xff;
	u8 control_selector = w_value >> 8;
	u8 clock_id = w_index >> 8;

	if ((entity_id == USB_IN_CLK_ID) || (entity_id == USB_OUT_CLK_ID)) {
		if (control_selector == UAC2_CS_CONTROL_SAM_FREQ) {
			dev_dbg(&agdev->gadget->dev,
				"control_selector UAC2_CS_CONTROL_SAM_FREQ, clock: %d\n", clock_id);
			cdev->gadget->ep0->driver_data = fn;
			uac2->clock_id = clock_id;
			req->complete = uac2_cs_control_sam_freq;
			return w_length;
		}
	} else if ((FUIN_EN(opts) && (entity_id == USB_IN_FU_ID)) ||
			(FUOUT_EN(opts) && (entity_id == USB_OUT_FU_ID))) {
		memcpy(&uac2->setup_cr, cr, sizeof(*cr));
		req->context = agdev;
		req->complete = out_rq_cur_complete;

		return w_length;
	} else {
		dev_err(&agdev->gadget->dev,
			"%s:%d entity_id=%d control_selector=%d TODO!\n",
			__func__, __LINE__, entity_id, control_selector);
	}
	return -EOPNOTSUPP;
}

static int
setup_rq_inf(struct usb_function *fn, const struct usb_ctrlrequest *cr)
{
	struct f_uac2 *uac2 = func_to_uac2(fn);
	struct g_audio *agdev = func_to_g_audio(fn);
	u16 w_index = le16_to_cpu(cr->wIndex);
	u8 intf = w_index & 0xff;

	if (intf != uac2->ac_intf) {
		dev_err(&agdev->gadget->dev,
			"%s:%d Error!\n", __func__, __LINE__);
		return -EOPNOTSUPP;
	}

	if (cr->bRequestType & USB_DIR_IN)
		return ac_rq_in(fn, cr);
	else if (cr->bRequest == UAC2_CS_CUR)
		return out_rq_cur(fn, cr);

	return -EOPNOTSUPP;
}

static int
afunc_setup(struct usb_function *fn, const struct usb_ctrlrequest *cr)
{
	struct usb_composite_dev *cdev = fn->config->cdev;
	struct g_audio *agdev = func_to_g_audio(fn);
	struct usb_request *req = cdev->req;
	u16 w_length = le16_to_cpu(cr->wLength);
	int value = -EOPNOTSUPP;

	/* Only Class specific requests are supposed to reach here */
	if ((cr->bRequestType & USB_TYPE_MASK) != USB_TYPE_CLASS)
		return -EOPNOTSUPP;

	if ((cr->bRequestType & USB_RECIP_MASK) == USB_RECIP_INTERFACE)
		value = setup_rq_inf(fn, cr);
	else
		dev_err(&agdev->gadget->dev, "%s:%d Error!\n",
				__func__, __LINE__);

	if (value >= 0) {
		req->length = value;
		req->zero = value < w_length;
		value = usb_ep_queue(cdev->gadget->ep0, req, GFP_ATOMIC);
		if (value < 0) {
			dev_err(&agdev->gadget->dev,
				"%s:%d Error!\n", __func__, __LINE__);
			req->status = 0;
		}
	}

	return value;
}

static inline struct f_uac2_opts *to_f_uac2_opts(struct config_item *item)
{
	return container_of(to_config_group(item), struct f_uac2_opts,
			    func_inst.group);
}

static inline struct f_uac2_alt_opts *to_f_uac2_alt_opts(struct config_item *item)
{
	return container_of(to_config_group(item), struct f_uac2_alt_opts,
			    c.group);
}

#define UAC2_ALT_ATTR_TO_OPTS struct f_uac2_alt_opts *alt_opts = to_f_uac2_alt_opts(item)
#define UAC2_ALT_ATTRIBUTE(type, name)					\
	UAC_ATTRIBUTE(f_uac2_alt_opts, UAC2_ALT_ATTR_TO_OPTS, alt_opts,	\
		      alt_opts->c.opts->lock, alt_opts->c.opts->refcnt,	\
		      type, name)

#define UAC2_ALT_ATTRIBUTE_SYNC(name)					\
	UAC_ATTRIBUTE_SYNC(f_uac2_alt_opts, UAC2_ALT_ATTR_TO_OPTS,	\
			   alt_opts, alt_opts->c.opts->lock,		\
			   alt_opts->c.opts->refcnt, name)

#define UAC2_ALT_ATTRIBUTE_STRING(name)					\
	UAC_ATTRIBUTE_STRING(f_uac2_alt_opts, UAC2_ALT_ATTR_TO_OPTS,	\
			     alt_opts, alt_opts->c.opts->lock,		\
			     alt_opts->c.opts->refcnt, name)


UAC2_ALT_ATTRIBUTE_STRING(name);
UAC2_ALT_ATTRIBUTE_STRING(it_name);
UAC2_ALT_ATTRIBUTE_STRING(it_ch_name);
UAC2_ALT_ATTRIBUTE_STRING(ot_name);
UAC2_ALT_ATTRIBUTE_STRING(fu_vol_name);

UAC2_ALT_ATTRIBUTE(u32, ssize);
UAC2_ALT_ATTRIBUTE(u32, chmask);
UAC2_ALT_ATTRIBUTE_SYNC(sync);
UAC2_ALT_ATTRIBUTE(u8, hs_bint);
UAC2_ALT_ATTRIBUTE(s16, terminal_type);

static struct configfs_attribute *f_uac2_alt_0_attrs[] = {
	&f_uac2_alt_opts_attr_name,

	NULL,
};

static const struct config_item_type alt_mode_0_type = {
	.ct_attrs	= f_uac2_alt_0_attrs,
	.ct_owner       = THIS_MODULE,
};

static struct configfs_attribute *f_uac2_alt_attrs_c[] = {
	&f_uac2_alt_opts_attr_name,
	&f_uac2_alt_opts_attr_it_name,
	&f_uac2_alt_opts_attr_it_ch_name,
	&f_uac2_alt_opts_attr_ot_name,
	&f_uac2_alt_opts_attr_fu_vol_name,
	&f_uac2_alt_opts_attr_ssize,
	&f_uac2_alt_opts_attr_chmask,
	&f_uac2_alt_opts_attr_sync,
	&f_uac2_alt_opts_attr_hs_bint,
	&f_uac2_alt_opts_attr_terminal_type,

	NULL,
};

static struct configfs_attribute *f_uac2_alt_attrs_p[] = {
	&f_uac2_alt_opts_attr_name,
	&f_uac2_alt_opts_attr_it_name,
	&f_uac2_alt_opts_attr_it_ch_name,
	&f_uac2_alt_opts_attr_ot_name,
	&f_uac2_alt_opts_attr_fu_vol_name,
	&f_uac2_alt_opts_attr_ssize,
	&f_uac2_alt_opts_attr_chmask,
	/* Playback does not have sync */
	&f_uac2_alt_opts_attr_hs_bint,
	&f_uac2_alt_opts_attr_terminal_type,

	NULL,
};


static void f_uac2_alt_attr_release(struct config_item *item)
{
	struct f_uac2_alt_opts *alt_opts = to_f_uac2_alt_opts(item);

	/* Opts 0 and 1 are fixed structures, 2+ are kzalloc'd */
	if (alt_opts->c.alt_num > 1)
		kfree(alt_opts);
}

static struct configfs_item_operations f_uac2_alt_item_ops = {
	.release	= f_uac2_alt_attr_release,
};

static const struct config_item_type alt_mode_c_type = {
	.ct_item_ops	= &f_uac2_alt_item_ops,
	.ct_attrs	= f_uac2_alt_attrs_c,
	.ct_owner       = THIS_MODULE,
};

static const struct config_item_type alt_mode_p_type = {
	.ct_item_ops	= &f_uac2_alt_item_ops,
	.ct_attrs	= f_uac2_alt_attrs_p,
	.ct_owner       = THIS_MODULE,
};

/*-------------------------------------------------------------------------*/

static void f_uac2_attr_release(struct config_item *item)
{
	struct f_uac2_opts *opts = to_f_uac2_opts(item);

	usb_put_function_instance(&opts->func_inst);
}

static struct configfs_item_operations f_uac2_item_ops = {
	.release	= f_uac2_attr_release,
};

#define UAC2_ATTR_TO_OPTS struct f_uac2_opts *opts = to_f_uac2_opts(item)
#define UAC2_ATTRIBUTE(type, name)					\
	UAC_ATTRIBUTE(f_uac2_opts, UAC2_ATTR_TO_OPTS, opts,		\
		      opts->lock, opts->refcnt, type, name)

#define UAC2_ATTRIBUTE_SYNC(name)					\
	UAC_ATTRIBUTE_SYNC(f_uac2_opts, UAC2_ATTR_TO_OPTS, opts,	\
			   opts->lock, opts->refcnt, name)

#define UAC2_RATE_ATTRIBUTE(name)					\
	UAC_RATE_ATTRIBUTE(f_uac2_opts, UAC2_ATTR_TO_OPTS, opts,	\
			   opts->lock, opts->refcnt, name)

#define UAC2_ATTRIBUTE_STRING(name)					\
	UAC_ATTRIBUTE_STRING(f_uac2_opts, UAC2_ATTR_TO_OPTS, opts,	\
			     opts->lock, opts->refcnt, name)

UAC2_ATTRIBUTE(u32, p_chmask);
UAC2_RATE_ATTRIBUTE(p_srate);
UAC2_ATTRIBUTE(u32, p_ssize);
UAC2_ATTRIBUTE(u8, p_hs_bint);
UAC2_ATTRIBUTE(u32, c_chmask);
UAC2_RATE_ATTRIBUTE(c_srate);
UAC2_ATTRIBUTE_SYNC(c_sync);
UAC2_ATTRIBUTE(u32, c_ssize);
UAC2_ATTRIBUTE(u8, c_hs_bint);
UAC2_ATTRIBUTE(u32, req_number);

UAC2_ATTRIBUTE(bool, p_mute_present);
UAC2_ATTRIBUTE(bool, p_volume_present);
UAC2_ATTRIBUTE(s16, p_volume_min);
UAC2_ATTRIBUTE(s16, p_volume_max);
UAC2_ATTRIBUTE(s16, p_volume_res);

UAC2_ATTRIBUTE(bool, c_mute_present);
UAC2_ATTRIBUTE(bool, c_volume_present);
UAC2_ATTRIBUTE(s16, c_volume_min);
UAC2_ATTRIBUTE(s16, c_volume_max);
UAC2_ATTRIBUTE(s16, c_volume_res);
UAC2_ATTRIBUTE(u32, fb_max);
UAC2_ATTRIBUTE_STRING(function_name);
UAC2_ATTRIBUTE_STRING(if_ctrl_name);
UAC2_ATTRIBUTE_STRING(clksrc_in_name);
UAC2_ATTRIBUTE_STRING(clksrc_out_name);

UAC2_ATTRIBUTE_STRING(p_it_name);
UAC2_ATTRIBUTE_STRING(p_it_ch_name);
UAC2_ATTRIBUTE_STRING(p_ot_name);
UAC2_ATTRIBUTE_STRING(p_fu_vol_name);

UAC2_ATTRIBUTE_STRING(c_it_name);
UAC2_ATTRIBUTE_STRING(c_it_ch_name);
UAC2_ATTRIBUTE_STRING(c_ot_name);
UAC2_ATTRIBUTE_STRING(c_fu_vol_name);

UAC2_ATTRIBUTE(s16, p_terminal_type);
UAC2_ATTRIBUTE(s16, c_terminal_type);


static struct configfs_attribute *f_uac2_attrs[] = {
	&f_uac2_opts_attr_p_chmask,
	&f_uac2_opts_attr_p_srate,
	&f_uac2_opts_attr_p_ssize,
	&f_uac2_opts_attr_p_hs_bint,
	&f_uac2_opts_attr_c_chmask,
	&f_uac2_opts_attr_c_srate,
	&f_uac2_opts_attr_c_ssize,
	&f_uac2_opts_attr_c_hs_bint,
	&f_uac2_opts_attr_c_sync,
	&f_uac2_opts_attr_req_number,
	&f_uac2_opts_attr_fb_max,

	&f_uac2_opts_attr_p_mute_present,
	&f_uac2_opts_attr_p_volume_present,
	&f_uac2_opts_attr_p_volume_min,
	&f_uac2_opts_attr_p_volume_max,
	&f_uac2_opts_attr_p_volume_res,

	&f_uac2_opts_attr_c_mute_present,
	&f_uac2_opts_attr_c_volume_present,
	&f_uac2_opts_attr_c_volume_min,
	&f_uac2_opts_attr_c_volume_max,
	&f_uac2_opts_attr_c_volume_res,

	&f_uac2_opts_attr_function_name,
	&f_uac2_opts_attr_if_ctrl_name,
	&f_uac2_opts_attr_clksrc_in_name,
	&f_uac2_opts_attr_clksrc_out_name,

	&f_uac2_opts_attr_p_it_name,
	&f_uac2_opts_attr_p_it_ch_name,
	&f_uac2_opts_attr_p_ot_name,
	&f_uac2_opts_attr_p_fu_vol_name,

	&f_uac2_opts_attr_c_it_name,
	&f_uac2_opts_attr_c_it_ch_name,
	&f_uac2_opts_attr_c_ot_name,
	&f_uac2_opts_attr_c_fu_vol_name,

	&f_uac2_opts_attr_p_terminal_type,
	&f_uac2_opts_attr_c_terminal_type,

	NULL,
};

static struct config_group *f_uac2_group_make(
		struct config_group *group,
		const char *name)
{
	struct f_uac2_opts *opts = to_f_uac2_opts(&group->cg_item);
	struct f_uac2_alt_opts *alt_opts;
	struct f_uac2_alt_opts *pos;
	struct config_group *ret;
	unsigned int alt_num;
	int playback = 0;

	mutex_lock(&opts->lock);
	if (opts->refcnt) {
		ret = ERR_PTR(-EBUSY);
		goto end;
	}

	if (sscanf(name, "c_alt.%u", &alt_num) != 1) {
		playback = 1;
		if (sscanf(name, "p_alt.%u", &alt_num) != 1) {
			ret = ERR_PTR(-EINVAL);
			goto end;
		}
	}

	if (alt_num > 255) {
		ret = ERR_PTR(-EINVAL);
		goto end;
	}

	/* Alt mode 0 has less properties */
	if (alt_num == 0) {
		struct f_uac2_alt_0_opts *alt_0_opts = (playback) ? &opts->p_alt_0_opts
								  : &opts->c_alt_0_opts;

		init_alt_0_opts(alt_0_opts, opts, playback);
		config_group_init_type_name(&alt_0_opts->c.group, name, &alt_mode_0_type);
		ret = &alt_0_opts->c.group;
		goto end;
	}

	if (alt_num == 1) {
		/* Alt mode 1 always exists */
		alt_opts = (playback) ? &opts->p_alt_1_opts : &opts->c_alt_1_opts;
	} else {
		/* Allocate a structure for alt mode 2+ */
		alt_opts = kzalloc(sizeof(*alt_opts), GFP_KERNEL);
		if (!alt_opts) {
			ret = ERR_PTR(-ENOMEM);
			goto end;
		}
	}

	ret = &alt_opts->c.group;

	config_group_init_type_name(&alt_opts->c.group, name, (playback) ? &alt_mode_p_type
									 : &alt_mode_c_type);

	init_alt_opts(alt_opts, opts, alt_num, playback);

	/* Alt mode 1 doesn't go in the list. It is handled separately to
	 * also handle the case where the alt.1 group is not created.
	 */
	if (alt_num == 1)
		goto end;

	/* Insert the new alt mode in the list, sorted by alt_num */
	list_for_each_entry(pos, (playback) ? &opts->p_alt_opts : &opts->c_alt_opts, list) {
		if (alt_opts->c.alt_num < pos->c.alt_num) {
			list_add_tail(&alt_opts->list, &pos->list);
			mutex_unlock(&opts->lock);
			goto end;
		}
	}

	list_add_tail(&alt_opts->list, (playback) ? &opts->p_alt_opts : &opts->c_alt_opts);

end:
	mutex_unlock(&opts->lock);

	return ret;
}

static void f_uac2_group_drop(struct config_group *group, struct config_item *item)
{
	struct f_uac2_alt_opts *alt_opts = to_f_uac2_alt_opts(item);

	/* Alt modes 0 and 1 are preallocated and not included in the list */
	if (alt_opts->c.alt_num > 1) {
		mutex_lock(&alt_opts->c.opts->lock);
		list_del(&alt_opts->list);
		mutex_unlock(&alt_opts->c.opts->lock);
	}

	config_item_put(item);
}

static struct configfs_group_operations f_uac2_group_ops = {
	.make_group     = &f_uac2_group_make,
	.drop_item      = &f_uac2_group_drop,
};

static const struct config_item_type f_uac2_func_type = {
	.ct_item_ops	= &f_uac2_item_ops,
	.ct_group_ops	= &f_uac2_group_ops,
	.ct_attrs	= f_uac2_attrs,
	.ct_owner	= THIS_MODULE,
};

static void afunc_free_inst(struct usb_function_instance *f)
{
	struct f_uac2_opts *opts;

	opts = container_of(f, struct f_uac2_opts, func_inst);
	kfree(opts);
}

static struct usb_function_instance *afunc_alloc_inst(void)
{
	struct f_uac2_opts *opts;

	opts = kzalloc(sizeof(*opts), GFP_KERNEL);
	if (!opts)
		return ERR_PTR(-ENOMEM);

	mutex_init(&opts->lock);
	opts->func_inst.free_func_inst = afunc_free_inst;

	INIT_LIST_HEAD(&opts->c_alt_opts);
	INIT_LIST_HEAD(&opts->p_alt_opts);

	config_group_init_type_name(&opts->func_inst.group, "",
				    &f_uac2_func_type);

	opts->p_chmask = UAC2_DEF_PCHMASK;
	opts->p_srates[0] = UAC2_DEF_PSRATE;
	opts->p_ssize = UAC2_DEF_PSSIZE;
	opts->p_hs_bint = UAC2_DEF_PHSBINT;
	opts->c_chmask = UAC2_DEF_CCHMASK;
	opts->c_srates[0] = UAC2_DEF_CSRATE;
	opts->c_ssize = UAC2_DEF_CSSIZE;
	opts->c_hs_bint = UAC2_DEF_CHSBINT;
	opts->c_sync = UAC2_DEF_CSYNC;

	opts->p_mute_present = UAC2_DEF_MUTE_PRESENT;
	opts->p_volume_present = UAC2_DEF_VOLUME_PRESENT;
	opts->p_volume_min = UAC2_DEF_MIN_DB;
	opts->p_volume_max = UAC2_DEF_MAX_DB;
	opts->p_volume_res = UAC2_DEF_RES_DB;

	opts->c_mute_present = UAC2_DEF_MUTE_PRESENT;
	opts->c_volume_present = UAC2_DEF_VOLUME_PRESENT;
	opts->c_volume_min = UAC2_DEF_MIN_DB;
	opts->c_volume_max = UAC2_DEF_MAX_DB;
	opts->c_volume_res = UAC2_DEF_RES_DB;

	opts->req_number = UAC2_DEF_REQ_NUM;
	opts->fb_max = FBACK_FAST_MAX;

	scnprintf(opts->function_name, sizeof(opts->function_name), "Source/Sink");
	scnprintf(opts->if_ctrl_name, sizeof(opts->if_ctrl_name), "Topology Control");
	scnprintf(opts->clksrc_in_name, sizeof(opts->clksrc_in_name), "Input Clock");
	scnprintf(opts->clksrc_out_name, sizeof(opts->clksrc_out_name), "Output Clock");

	// Note: Strings are from the host perspective, opt prefixes are from the device perspective
	scnprintf(opts->p_it_name, sizeof(opts->p_it_name), "USBD Out");
	scnprintf(opts->p_it_ch_name, sizeof(opts->p_it_ch_name), "Capture Channels");
	scnprintf(opts->p_ot_name, sizeof(opts->p_ot_name), "USBH In");
	scnprintf(opts->p_fu_vol_name, sizeof(opts->p_fu_vol_name), "Capture Volume");

	scnprintf(opts->c_it_name, sizeof(opts->c_it_name), "USBH Out");
	scnprintf(opts->c_it_ch_name, sizeof(opts->c_it_ch_name), "Playback Channels");
	scnprintf(opts->c_ot_name, sizeof(opts->c_ot_name), "USBD In");
	scnprintf(opts->c_fu_vol_name, sizeof(opts->c_fu_vol_name), "Playback Volume");

	opts->p_terminal_type = UAC2_DEF_P_TERM_TYPE;
	opts->c_terminal_type = UAC2_DEF_C_TERM_TYPE;

	return &opts->func_inst;
}

static void afunc_free(struct usb_function *f)
{
	struct g_audio *agdev;
	struct f_uac2_opts *opts;

	agdev = func_to_g_audio(f);
	opts = container_of(f->fi, struct f_uac2_opts, func_inst);
	kfree(agdev);
	mutex_lock(&opts->lock);
	--opts->refcnt;
	mutex_unlock(&opts->lock);
}

static void afunc_unbind(struct usb_configuration *c, struct usb_function *f)
{
	struct g_audio *agdev = func_to_g_audio(f);

	g_audio_cleanup(agdev);
	usb_free_all_descriptors(f);

	agdev->gadget = NULL;

	kfree(out_feature_unit_desc);
	out_feature_unit_desc = NULL;
	kfree(in_feature_unit_desc);
	in_feature_unit_desc = NULL;
}

static struct usb_function *afunc_alloc(struct usb_function_instance *fi)
{
	struct f_uac2	*uac2;
	struct f_uac2_opts *opts;

	uac2 = kzalloc(sizeof(*uac2), GFP_KERNEL);
	if (uac2 == NULL)
		return ERR_PTR(-ENOMEM);

	opts = container_of(fi, struct f_uac2_opts, func_inst);
	mutex_lock(&opts->lock);
	++opts->refcnt;
	mutex_unlock(&opts->lock);

	uac2->g_audio.func.name = "uac2_func";
	uac2->g_audio.func.bind = afunc_bind;
	uac2->g_audio.func.unbind = afunc_unbind;
	uac2->g_audio.func.set_alt = afunc_set_alt;
	uac2->g_audio.func.get_alt = afunc_get_alt;
	uac2->g_audio.func.disable = afunc_disable;
	uac2->g_audio.func.suspend = afunc_suspend;
	uac2->g_audio.func.setup = afunc_setup;
	uac2->g_audio.func.free_func = afunc_free;

	return &uac2->g_audio.func;
}

DECLARE_USB_FUNCTION_INIT(uac2, afunc_alloc_inst, afunc_alloc);
MODULE_DESCRIPTION("USB Audio Class 2.0 Function");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Yadwinder Singh");
MODULE_AUTHOR("Jaswinder Singh");
MODULE_AUTHOR("Ruslan Bilovol");
