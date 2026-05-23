/* SPDX-License-Identifier: GPL-2.0 */
/*
   BlueZ - Bluetooth protocol stack for Linux

   Copyright (C) 2011-2012  Intel Corporation
*/

#ifndef __HCI_MON_H
#define __HCI_MON_H

struct hci_mon_hdr {
	__le16	opcode;
	__le16	index;
	__le16	len;
} __packed;
#define HCI_MON_HDR_SIZE 6

#define HCI_MON_NEW_INDEX	0
#define HCI_MON_DEL_INDEX	1
#define HCI_MON_COMMAND_PKT	2
#define HCI_MON_EVENT_PKT	3
#define HCI_MON_ACL_TX_PKT	4
#define HCI_MON_ACL_RX_PKT	5
#define HCI_MON_SCO_TX_PKT	6
#define HCI_MON_SCO_RX_PKT	7
#define HCI_MON_OPEN_INDEX	8
#define HCI_MON_CLOSE_INDEX	9
#define HCI_MON_INDEX_INFO	10
#define HCI_MON_VENDOR_DIAG	11
#define HCI_MON_SYSTEM_NOTE	12
#define HCI_MON_USER_LOGGING	13
#define HCI_MON_CTRL_OPEN	14
#define HCI_MON_CTRL_CLOSE	15
#define HCI_MON_CTRL_COMMAND	16
#define HCI_MON_CTRL_EVENT	17
#define HCI_MON_ISO_TX_PKT	18
#define HCI_MON_ISO_RX_PKT	19
#define HCI_MON_DRV_TX_PKT	20
#define HCI_MON_DRV_RX_PKT	21

struct hci_mon_new_index {
	__u8		type;
	__u8		bus;
	bdaddr_t	bdaddr;
	char		name[8] __nonstring;
} __packed;
#define HCI_MON_NEW_INDEX_SIZE 16

struct hci_mon_index_info {
	bdaddr_t	bdaddr;
	__le16		manufacturer;
} __packed;
#define HCI_MON_INDEX_INFO_SIZE 8

#endif /* __HCI_MON_H */
