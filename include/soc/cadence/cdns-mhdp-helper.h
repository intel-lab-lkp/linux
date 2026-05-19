/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2023-2024 NXP Semiconductor, Inc.
 */
#ifndef __CDNS_MHDP_HELPER_H__
#define __CDNS_MHDP_HELPER_H__

#include <linux/iopoll.h>
#include <linux/mutex.h>
#include <linux/unaligned.h>

/* mailbox regs offset */
#define CDNS_MAILBOX_FULL			0x00008
#define CDNS_MAILBOX_EMPTY			0x0000c
#define CDNS_MAILBOX_TX_DATA			0x00010
#define CDNS_MAILBOX_RX_DATA			0x00014

#define MAILBOX_RETRY_US			1000
#define MAILBOX_TIMEOUT_US			2000000

/* Module ID Code */
#define MB_MODULE_ID_DP_TX			0x01
#define MB_MODULE_ID_HDMI_TX			0x03
#define MB_MODULE_ID_HDCP_TX			0x07
#define MB_MODULE_ID_HDCP_RX			0x08
#define MB_MODULE_ID_HDCP_GENERAL		0x09
#define MB_MODULE_ID_GENERAL			0x0A

/* General Commands */
#define GENERAL_MAIN_CONTROL			0x01
#define GENERAL_TEST_ECHO			0x02
#define GENERAL_BUS_SETTINGS			0x03
#define GENERAL_TEST_ACCESS			0x04
#define GENERAL_REGISTER_WRITE			0x05
#define GENERAL_WRITE_FIELD			0x06
#define GENERAL_REGISTER_READ			0x07
#define GENERAL_GET_HPD_STATE			0x11

/* DPTX Commands */
#define DPTX_SET_POWER_MNG			0x00
#define DPTX_SET_HOST_CAPABILITIES		0x01
#define DPTX_GET_EDID				0x02
#define DPTX_READ_DPCD				0x03
#define DPTX_WRITE_DPCD				0x04
#define DPTX_ENABLE_EVENT			0x05
#define DPTX_WRITE_REGISTER			0x06
#define DPTX_READ_REGISTER			0x07
#define DPTX_WRITE_FIELD			0x08
#define DPTX_TRAINING_CONTROL			0x09
#define DPTX_READ_EVENT				0x0a
#define DPTX_READ_LINK_STAT			0x0b
#define DPTX_SET_VIDEO				0x0c
#define DPTX_SET_AUDIO				0x0d
#define DPTX_GET_LAST_AUX_STAUS			0x0e
#define DPTX_SET_LINK_BREAK_POINT		0x0f
#define DPTX_FORCE_LANES			0x10
#define DPTX_HPD_STATE				0x11
#define DPTX_ADJUST_LT				0x12

/* HDMI TX Commands */
#define HDMI_TX_READ				0x00
#define HDMI_TX_WRITE				0x01
#define HDMI_TX_UPDATE_READ			0x02
#define HDMI_TX_EDID				0x03
#define HDMI_TX_EVENTS				0x04
#define HDMI_TX_HPD_STATUS			0x05

/* HDCP TX Commands */
#define HDCP_TRAN_CONFIGURATION			0x00
#define HDCP2X_TX_SET_PUBLIC_KEY_PARAMS		0x01
#define HDCP2X_TX_SET_DEBUG_RANDOM_NUMBERS	0x02
#define HDCP2X_TX_RESPOND_KM			0x03
#define HDCP1_TX_SEND_KEYS			0x04
#define HDCP1_TX_SEND_RANDOM_AN			0x05
#define HDCP_TRAN_STATUS_CHANGE			0x06
#define HDCP2X_TX_IS_KM_STORED			0x07
#define HDCP2X_TX_STORE_KM			0x08
#define HDCP_TRAN_IS_REC_ID_VALID		0x09
#define HDCP_TRAN_RESPOND_RECEIVER_ID_VALID	0x0a
#define HDCP_TRAN_TEST_KEYS			0x0b
#define HDCP2X_TX_SET_KM_KEY_PARAMS		0x0c
#define HDCP_NUM_OF_SUPPORTED_MESSAGES		0x0d

/**
 * struct cdns_mhdp_base - Base structure for CDNS MHDP devices
 * @dev: Pointer to the device structure
 * @regs: Base address of the regular register space
 * @sapb_regs: Base address of the secure APB register space
 * @mailbox_mutex: Mutex to protect mailbox communications with firmware
 *
 * This structure contains the base resources needed for CDNS MHDP helper
 * functions. Each device instance should have its own cdns_mhdp_base structure
 * to ensure proper isolation of mailbox operations between multiple devices.
 */
struct cdns_mhdp_base {
	struct device *dev;
	void __iomem *regs;
	void __iomem *sapb_regs;
	struct mutex mailbox_mutex;
};

/* Mailbox helper functions */
int cdns_mhdp_mailbox_send(struct cdns_mhdp_base *base,
			   u8 module_id, u8 opcode,
			   u16 size, u8 *message);
int cdns_mhdp_mailbox_send_recv(struct cdns_mhdp_base *base,
				u8 module_id, u8 opcode,
				u16 msg_size, u8 *msg,
				u16 resp_size, u8 *resp);
int cdns_mhdp_mailbox_send_recv_multi(struct cdns_mhdp_base *base,
				      u8 module_id, u8 opcode,
				      u16 msg_size, u8 *msg,
				      u8 opcode_resp,
				      u16 resp1_size, u8 *resp1,
				      u16 resp2_size, u8 *resp2);

/* Secure mailbox helper functions */
int cdns_mhdp_secure_mailbox_send(struct cdns_mhdp_base *base,
				  u8 module_id, u8 opcode,
				  u16 size, u8 *message);
int cdns_mhdp_secure_mailbox_send_recv(struct cdns_mhdp_base *base,
				       u8 module_id, u8 opcode,
				       u16 msg_size, u8 *msg,
				       u16 resp_size, u8 *resp);
int cdns_mhdp_secure_mailbox_send_recv_multi(struct cdns_mhdp_base *base,
					     u8 module_id, u8 opcode,
					     u16 msg_size, u8 *msg,
					     u8 opcode_resp,
					     u16 resp1_size, u8 *resp1,
					     u16 resp2_size, u8 *resp2);

/* General commands helper functions */
int cdns_mhdp_reg_read(struct cdns_mhdp_base *base, u32 addr, u32 *value);
int cdns_mhdp_reg_write(struct cdns_mhdp_base *base, u32 addr, u32 val);

/* DPTX commands helper functions */
int cdns_mhdp_dp_reg_write_bit(struct cdns_mhdp_base *base, u16 addr,
			       u8 start_bit, u8 bits_no, u32 val);
int cdns_mhdp_dpcd_read(struct cdns_mhdp_base *base,
			u32 addr, u8 *data, u16 len);
int cdns_mhdp_dpcd_write(struct cdns_mhdp_base *base, u32 addr, u8 value);

#endif /* __CDNS_MHDP_HELPER_H__ */
