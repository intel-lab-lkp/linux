/* SPDX-License-Identifier: GPL-2.0 */
/* CAN driver base for IXXAT USB-to-CAN
 *
 * Copyright (C) 2018-2024 HMS Industrial Networks <socketcan@hms-networks.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published
 * by the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#ifndef IXXAT_USB_CORE_H
#define IXXAT_USB_CORE_H

#include <linux/types.h>
#include <linux/bitfield.h>
#include <linux/spinlock_types.h>
#include <linux/usb.h>
#include <linux/can/dev.h>

/* supported bus types */
#define IXXAT_USB_BUSTYPE_CAN		1

/* bus type: high-order byte */
#define IXXAT_USB_BUSTYPE_BITS		8
#define IXXAT_USB_BUSTYPE_MASK		GENMASK(IXXAT_USB_BUSTYPE_BITS + 7, 8)

#define IXXAT_USB_STATE_CONNECTED	BIT(0)
#define IXXAT_USB_STATE_STARTED		BIT(1)

#define IXXAT_USB_FREE_ENTRY		0xFFFF

#define IXXAT_USB_MAX_CHANNEL		5
#define IXXAT_USB_MAX_TYPES		32
#define IXXAT_USB_MAX_RX_URBS		4
#define IXXAT_USB_MAX_TX_URBS		10

#define IXXAT_USB_DEV_FWTYPE_BAL	3	/* BAL conform FW */

#define IXXAT_USB_OPMODE_STANDARD	BIT(0)
#define IXXAT_USB_OPMODE_EXTENDED	BIT(1)
#define IXXAT_USB_OPMODE_ERRFRAME	BIT(2)
#define IXXAT_USB_OPMODE_LISTONLY	BIT(3)

#define IXXAT_USB_EXMODE_EXTDATA	BIT(0)
#define IXXAT_USB_EXMODE_FASTDATA	BIT(1)
#define IXXAT_USB_EXMODE_ISOFD		BIT(2)

#define IXXAT_USB_BTMODE_NAT		BIT(0)
#define IXXAT_USB_BTMODE_TSM		BIT(1)

#define IXXAT_USB_STOP_ACTION_CLEARALL	3

#define IXXAT_USB_CAN_FEATURE_LISTONLY	BIT(6)   /* listen only mode */
#define IXXAT_USB_CAN_FEATURE_SSM	BIT(10)  /* single shot mode */
#define IXXAT_USB_CAN_FEATURE_ISOFD	BIT(15)  /* i: ISO CAN FD */
#define IXXAT_USB_CAN_FEATURE_NONISOFD	BIT(16)  /* i: non-ISO CAN FD */

#define IXXAT_USB_CAN_DATA		0x00
#define IXXAT_USB_CAN_INFO		0x01
#define IXXAT_USB_CAN_ERROR		0x02
#define IXXAT_USB_CAN_STATUS		0x03
#define IXXAT_USB_CAN_WAKEUP		0x04
#define IXXAT_USB_CAN_TIMEOVR		0x05
#define IXXAT_USB_CAN_TIMERST		0x06

#define IXXAT_USB_CAN_STATUS_OK		0
#define IXXAT_USB_CAN_STATUS_OVERRUN	BIT(1)
#define IXXAT_USB_CAN_STATUS_ERRLIM	BIT(2)
#define IXXAT_USB_CAN_STATUS_BUSOFF	BIT(3)
#define IXXAT_USB_CAN_STATUS_ERR_PAS	BIT(13)

#define IXXAT_USB_CAN_ERROR_CODE	0
#define IXXAT_USB_CAN_ERROR_COUNTER_RX	3
#define IXXAT_USB_CAN_ERROR_COUNTER_TX	4
#define IXXAT_USB_CAN_ERROR_LEN		5

#define IXXAT_USB_CAN_ERROR_STUFF	1
#define IXXAT_USB_CAN_ERROR_FORM	2
#define IXXAT_USB_CAN_ERROR_ACK		3
#define IXXAT_USB_CAN_ERROR_BIT		4
#define IXXAT_USB_CAN_ERROR_CRC		6

#define IXXAT_USB_MSG_FLAGS_TYPE_MASK	GENMASK(7, 0)
#define IXXAT_USB_MSG_FLAGS_DLC_MASK	GENMASK(19, 16)
#define IXXAT_USB_MSG_FLAGS_OVR		BIT(20)
#define IXXAT_USB_MSG_FLAGS_SRR		BIT(21)
#define IXXAT_USB_MSG_FLAGS_RTR		BIT(22)
#define IXXAT_USB_MSG_FLAGS_EXT		BIT(23)

#define IXXAT_USB_MSG_FLAGS_SINGLESHOT	BIT(8)

#define IXXAT_USB_FDMSG_FLAGS_EDL	BIT(10)
#define IXXAT_USB_FDMSG_FLAGS_FDR	BIT(11)
#define IXXAT_USB_FDMSG_FLAGS_ESI	BIT(12)

/* USB command timeout values  */
/* for IXXAT_USB_BRD_CMD_POWER (wakeup/sleep) */
#define IXXAT_USB_POWER_CMD_TIMEOUT	500

/* for other commands */
#define IXXAT_USB_CMD_TIMEOUT		100

/* port/socket index to address board functionality */
#define IXXAT_USB_BRD_PORT		0xffff
#define IXXAT_USB_BRD_SOCKET		0xffff

/* CAN related commands*/
#define IXXAT_USB_CAN_CMD_START		0x326
#define IXXAT_USB_CAN_CMD_STOP		0x327
#define IXXAT_USB_CAN_CMD_RESET		0x328

/* board related commands */
#define IXXAT_USB_BRD_CMD_GET_FWINFO	0x400
#define IXXAT_USB_BRD_CMD_GET_DEVCAPS	0x401
#define IXXAT_USB_BRD_CMD_GET_DEVINFO	0x402
#define IXXAT_USB_BRD_CMD_GET_FWINFO2	0x403
#define IXXAT_USB_BRD_CMD_POWER		0x421

/* struct ixxat_can_msg_base - IXXAT CAN message base (CL1/CL2)
 * @size: Message size (this field excluded)
 * @time: Message timestamp
 * @msg_id: Message ID
 * @flags: Message flags
 *
 * Contains the common fields of an IXXAT CAN message on both CL1 and CL2
 * devices
 */
struct ixxat_can_msg_base {
	u8 size;
	__le32 time;
	__le32 msg_id;
	__le32 flags;
} __packed;

/* struct ixxat_can_msg_cl1 - IXXAT CAN message (CL1)
 * @data: Message data (standard CAN frame)
 *
 * Contains the fields of an IXXAT CAN message on CL1 devices
 */
struct ixxat_can_msg_cl1 {
	u8 data[CAN_MAX_DLEN];
} __packed;

/* struct ixxat_can_msg_cl2 - IXXAT CAN message (CL2)
 * @client_id: Client ID
 * @data: Message data (CAN FD frame)
 *
 * Contains the fields of an IXXAT CAN message on CL2 devices
 */
struct ixxat_can_msg_cl2 {
	__le32 client_id;
	u8 data[CANFD_MAX_DLEN];
} __packed;

/* struct ixxat_can_msg - IXXAT CAN message
 * @base: Base message
 * @cl1: Cl1 message
 * @cl2: Cl2 message
 *
 * Contains an IXXAT CAN message
 */
struct ixxat_can_msg {
	struct ixxat_can_msg_base base;
	union {
		struct ixxat_can_msg_cl1 cl1;
		struct ixxat_can_msg_cl2 cl2;
	};
} __packed;

/* struct ixxat_dev_caps - Device capabilities
 * @bus_ctrl_count: Stores the bus controller counter
 * @bus_ctrl_types: Stores the bus controller types
 *
 * Contains the device capabilities
 */
struct ixxat_dev_caps {
	__le16 bus_ctrl_count;
	__le16 bus_ctrl_types[IXXAT_USB_MAX_TYPES];
} __packed;

/* struct ixxat_canbtp Bittiming parameters (CL2)
 * @mode: Operation mode
 * @bps: Bits per second
 * @ts1: First time segment
 * @ts2: Second time segment
 * @sjw: Synchronization jump width
 * @tdo: Transmitter delay offset
 *
 * Bittiming parameters of a CL2 initialization request
 */
struct ixxat_canbtp {
	__le32 mode;
	__le32 bps;
	__le16 ts1;
	__le16 ts2;
	__le16 sjw;
	__le16 tdo;
} __packed;

/* struct ixxat_dev_info IXXAT USB device information
 * @device_name: Name of the device
 * @device_id: Device identification (unique device id)
 * @device_version: Device version (0, 1, ...)
 * @device_fpga_version: Version of FPGA design
 *
 * Contains device information of IXXAT USB devices
 */
struct ixxat_dev_info {
	char device_name[16];
	char device_id[16];
	__le16 device_version;
	__le32 device_fpga_version;
} __packed;

/* struct ixxat_fw_info IXXAT USB device firmware information
 * @firmware_type: type of currently running firmware
 * @major_version: major firmware version number
 * @minor_version: minor firmware version number
 * @build_version: build firmware version number
 *
 * Contains device information of IXXAT USB devices
 */
struct ixxat_fw_info {
	__le32 firmware_type;
	__le16 res;
	__le16 major_version;
	__le16 minor_version;
	__le16 build_version;
} __packed;

/* struct ixxat_fw_info2 IXXAT USB device firmware information
 * @firmware_type: type of currently running firmware
 * @major_version: major firmware version number
 * @minor_version: minor firmware version number
 * @build_version: build firmware version number
 * @revision     : revision number
 *
 * Contains device information of IXXAT USB devices
 */
struct ixxat_fw_info2 {
	__le32 firmware_type;
	__le16 res;
	__le16 major_version;
	__le16 minor_version;
	__le16 build_version;
	__le16 revision;
} __packed;

/* struct ixxat_cancaps CAN controller capabilities
 * ctrltype;		Type of CAN controller (see IXXAT_USB_CAN_CTRL_ const)
 * buscoupling;		Type of Bus coupling (see IXXAT_USB_CAN_BUSC_ const)
 * features;		supported features (see IXXAT_USB_CAN_FEATURE_ constants)
 * can_clock_freq:	clock frequency of the primary counter in Hz
 * ts_clock_divisor:	divisor for the message time stamp counter
 * cms_clock_divisor:	divisor for the cyclic message scheduler
 * cms_max_ticks:	maximum tick count value of the cyclic message
 *			scheduler
 * dtx_clock_divisor:	divisor for the delayed message transmitter
 * dtx_max_ticks:	maximum tick count value of the delayed
 *			message transmitter
 *
 * Contains CAN controller capabilities information
 */
struct ixxat_cancaps {
	__le16 ctrltype;
	__le16 buscoupling;
	__le32 features;
	__le32 can_clock_freq;
	__le32 ts_clock_divisor;
	__le32 cms_clock_divisor;
	__le32 cms_max_ticks;
	__le32 dtx_clock_divisor;
	__le32 dtx_max_ticks;
} __packed;

/* struct ixxat_cancaps2 CANFD controller capabilities
 * ctrltype;		Type of CAN controller (see IXXAT_USB_CAN_CTRL_ const)
 * buscoupling;		Type of Bus coupling (see IXXAT_USB_CAN_BUSC_ const)
 * features;		supported features (see IXXAT_USB_CAN_FEATURE_ constants)
 * can_clock_freq:	CAN clock frequency in Hz (16/2 MHz for SJA1000)
 * sdr_range_min;	minimum bit timing values for standard bit rate
 * sdr_range_max;	maximum bit timing values for standard bit rate
 * fdr_range_min;	minimum bit timing values for fast data bit rate
 * fdr_range_max;	maximum bit timing values for fast data bit rate
 * ts_clock_freq;	clock frequency of the time stamp counter [Hz]
 * ts_clock_divisor;	divisor for the message time stamp counter
 * cms_clock_freq;	clock frequency of cyclic message scheduler [Hz]
 * cms_clock_divisor;	divisor for the cyclic message scheduler
 * cms_max_ticks;	maximum tick count value of the cyclic message
 *			scheduler
 * dtx_clock_freq;	clock frequency of the delayed message transmitter [Hz]
 * dtx_clock_divisor;	divisor for the delayed message transmitter
 * dtx_max_ticks;	maximum tick count value of the delayed
 *			message transmitter
 *
 * Contains CANFD controller capabilities information
 */
struct ixxat_cancaps2 {
	__le16 ctrltype;
	__le16 buscoupling;
	__le32 features;
	__le32 can_clock_freq;
	struct ixxat_canbtp sdr_range_min;
	struct ixxat_canbtp sdr_range_max;
	struct ixxat_canbtp fdr_range_min;
	struct ixxat_canbtp fdr_range_max;
	__le32 ts_clock_freq;
	__le32 ts_clock_divisor;
	__le32 cms_clock_freq;
	__le32 cms_clock_divisor;
	__le32 cms_max_ticks;
	__le32 dtx_clock_freq;
	__le32 dtx_clock_divisor;
	__le32 dtx_max_ticks;
} __packed;

/* Device timestamps
 *
 * Every device maintains an internal clock used for message timestamps.
 * The resolution of the clock is reported by the controller capabilities and
 * denotes the resolution of one clock tick by the two values:
 * ts_clock_freq/ts_clock_divisor
 * The following timeline occurs during the operation of a controller:
 *
 *         t_start                                      t_stop
 *     <------------>                               <------------>
 *
 *     A            B           D                   E            F          H
 *     |            |           |                   |            |          |
 *     v            v           v                   v            v          v
 *  ---------------------------------~  ...  ~----------------------------------> t
 *           ^                                            ^
 *           |                                            |
 *           C                                            G
 *
 * A: controller start command (request) is sent to the device
 * B: controller start command (response) returns from device and returns device
 *    time stamp of C
 * C: assumed CAN controller start time stamp, it is assumed to be in the middle
 *     between A and B (A + (t_start/2))
 * D: controller start info message arrives over the message fifo, it contains
 *    the device time stamp of C, too
 * E: controller stop command (request) is sent to the device
 * F: controller stop command (response) returns
 * G: assumed CAN controller stop time stamp, it is assumed to be in the middle
 *    between E and F (E + (t_stop/2))
 * H: controller stop info message arrives over the message fifo, it contains
 *    the device time stamp of G
 *
 * To correlate the start timestamp to the host clock we determine:
 *
 *     t_host_C = (t_host_B + t_host_A) / 2
 *              = t_host_A + ((t_host_B - t_host_A) / 2)
 *
 * t_C_host then corresponds to the device clock tick at C (t_C_device).
 * The current time stamp correlated to the host clock (t_current_host) can then
 * be determined via
 *
 *     t_host_current = t_host_C + conv_to_nsec(t_dev_current - t_dev_C)
 *
 * struct ixxat_time_ref Time reference
 * tick_multiplier:	used to convert ticks to ns
 * tick_divider:	used to convert ticks to ns
 * @ts_overrun_ticks:	number of timer overruns since start
 *
 * Contains time references of the device and the host
 */
struct ixxat_time_ref {
	u64 tick_multiplier;
	u64 tick_divider;
	u64 ts_overrun_ticks;
};

/* struct ixxat_tx_urb_context URB content for transmission
 * @dev: pointer to the IXXAT USB CAN device
 * @urb: USB request block
 * @urb_index: index of this URB (used to mark the context as occupied)
 * @msg_index: index of message (client_id)
 *
 * Contains content for USB request block transmissions
 */
struct ixxat_tx_urb_context {
	struct ixxat_usb_candevice *dev;
	struct urb *urb;
	u16  urb_index;
	u16  msg_index;
};

/* struct ixxat_usb_candevice IXXAT USB CAN device
 * @can: CAN common private data
 * @adapter: USB network descriptor
 * @udev: USB device
 * @netdev: Net_device
 * @active_tx_urbs: Active tx urbs
 * @tx_anchor: Submitted tx USB anchor
 * @tx_contexts: Buffer for tx contexts
 * @rx_anchor: Submitted rx USB anchor
 * @msgs: store to set the used message indexes
 * @msg_lastindex: last index which was set
 * @loopback:  global loopback
 * @state: Device state
 * @ctrl_index: Controller index
 * @ep_msg_in: USB endpoint for incoming messages
 * @ep_msg_out: USB endpoint for outgoing messages
 * @prev_dev: Previous opened device
 * @time_ref: Time reference
 * @dev_info: Device information
 * @bec: CAN error counter
 *
 * IXXAT USB based CAN device
 */
struct ixxat_usb_candevice {
	struct can_priv can;
	const struct ixxat_usb_adapter *adapter;
	struct usb_device *udev;
	struct net_device *netdev;

	atomic_t active_tx_urbs;
	struct usb_anchor tx_anchor;
	struct ixxat_tx_urb_context tx_contexts[IXXAT_USB_MAX_TX_URBS];

	struct usb_anchor rx_anchor;
	void  *rx_buf[IXXAT_USB_MAX_RX_URBS];

	u32 msgs;
	u32 msg_lastindex;

	bool loopback;
	u32 state;
	u16 ctrl_index;

	u8 ep_msg_in;
	u8 ep_msg_out;

	/* This lock prevents a race condition between xmit and receive. */
	spinlock_t dev_lock;

	/* pointer to shared USB device data  */
	struct ixxat_usb_device_data *shareddata;

	/* used to link can devices together */
	struct ixxat_usb_candevice *prev_dev;

	struct ixxat_time_ref time_ref;

	struct can_berr_counter bec;
};

/* struct ixxat_usb_dal_req IXXAT device request block
 * @size: Request size
 * @port: Request port
 * @socket: Request socket
 * @code: Request code
 *
 * IXXAT device request block
 */
struct ixxat_usb_dal_req {
	__le32 size;
	__le16 port;
	__le16 socket;
	__le32 code;
} __packed;

/* struct ixxat_usb_dal_res IXXAT device response block
 * @res_size: Expected response size
 * @ret_size: Actual response size
 * @code: Return code
 *
 * IXXAT device response block
 */
struct ixxat_usb_dal_res {
	__le32 res_size;
	__le32 ret_size;
	__le32 code;
} __packed;

/* struct ixxat_usb_dal_cmd IXXAT device command
 * @req: Request block
 * @req: Response block
 *
 * IXXAT device command
 */
struct ixxat_usb_dal_cmd {
	struct ixxat_usb_dal_req req;
	struct ixxat_usb_dal_res res;
} __packed;

/* struct ixxat_usb_caps_cmd Device capabilities command
 * @req: Request block
 * @res: Response block
 * @caps: Device capabilities
 *
 * Can be sent to a device to request its capabilities
 */
struct ixxat_usb_caps_cmd {
	struct ixxat_usb_dal_req req;
	struct ixxat_usb_dal_res res;
	struct ixxat_dev_caps caps;
	__le16 rsvd;
} __packed;

/* struct ixxat_usb_init_cl1_cmd Initialization command (CL1)
 * @req: Request block
 * @mode: Operation mode
 * @btr0: Bittiming register 0
 * @btr1: Bittiming register 1
 * @padding: 1 byte padding
 * @res: Response block
 *
 * Can be sent to a CL1 device to initialize it
 */
struct ixxat_usb_init_cl1_cmd {
	struct ixxat_usb_dal_req req;
	u8 mode;
	u8 btr0;
	u8 btr1;
	u8 padding;
	struct ixxat_usb_dal_res res;
} __packed;

/* struct ixxat_usb_init_cl2_cmd Initialization command (CL2)
 * @req: Request block
 * @opmode: Operation mode
 * @exmode: Extended mode
 * @sdr: Standard bittiming parameters
 * @fdr: Fast data bittiming parameters
 * @_padding: 2 bytes padding
 * @res: Response block
 *
 * Can be sent to a CL2 device to initialize it
 */
struct ixxat_usb_init_cl2_cmd {
	struct ixxat_usb_dal_req req;
	u8 opmode;
	u8 exmode;
	struct ixxat_canbtp sdr;
	struct ixxat_canbtp fdr;
	__le16 _padding;
	struct ixxat_usb_dal_res res;
} __packed;

/* struct ixxat_usb_getcaps_cl1_cmd Controller read capabilities
 * @req: Request block
 * @res: Response block
 * @caps: Controller capabilities
 *
 * Can be sent to a device to get the controller capabilities
 */
struct ixxat_usb_getcaps_cl1_cmd {
	struct ixxat_usb_dal_req req;
	struct ixxat_usb_dal_res res;
	struct ixxat_cancaps caps;
} __packed;

/* struct ixxat_usb_getcaps_cl2_cmd Controller read capabilities
 * @req: Request block
 * @res: Response block
 * @caps: Controller capabilities
 *
 * Can be sent to a device to get the controller capabilities
 */
struct ixxat_usb_getcaps_cl2_cmd {
	struct ixxat_usb_dal_req req;
	struct ixxat_usb_dal_res res;
	struct ixxat_cancaps2 caps;
} __packed;

/* struct ixxat_usb_start_cmd Controller start command
 * @req: Request block
 * @res: Response block
 * @time: Timestamp
 *
 * Can be sent to a device to start its controller
 */
struct ixxat_usb_start_cmd {
	struct ixxat_usb_dal_req req;
	struct ixxat_usb_dal_res res;
	__le32 time;
} __packed;

/* struct ixxat_usb_stop_cmd Controller stop command
 * @req: Request block
 * @action: Stop action
 * @res: Response block
 *
 * Can be sent to a device to start its controller
 */
struct ixxat_usb_stop_cmd {
	struct ixxat_usb_dal_req req;
	__le32 action;
	struct ixxat_usb_dal_res res;
} __packed;

/* struct ixxat_usb_power_cmd Power command
 * @req: Request block
 * @mode: Power mode
 * @_padding1: 1 byte padding
 * @_padding2: 2 byte padding
 * @res: Response block
 *
 * Can be sent to a device to set its power mode
 */
struct ixxat_usb_power_cmd {
	struct ixxat_usb_dal_req req;
	u8 mode;
	u8 _padding1;
	__le16 _padding2;
	struct ixxat_usb_dal_res res;
} __packed;

/* struct ixxat_usb_info_cmd Device information command
 * @req: Request block
 * @res: Response block
 * @info: Device information
 *
 * Can be sent to a device to request its device information
 */
struct ixxat_usb_info_cmd {
	struct ixxat_usb_dal_req req;
	struct ixxat_usb_dal_res res;
	struct ixxat_dev_info info;
	__le16 rsvd;
} __packed;

/* struct ixxat_usb_fwinfo_cmd Firmware information command
 * @req: Request block
 * @res: Response block
 * @info: Firmware information
 *
 * Can be sent to a device to request its firmware information
 */
struct ixxat_usb_fwinfo_cmd {
	struct ixxat_usb_dal_req req;
	struct ixxat_usb_dal_res res;
	struct ixxat_fw_info info;
} __packed;

/* struct ixxat_usb_fwinfo2_cmd Firmware information command
 * @req: Request block
 * @res: Response block
 * @info: Firmware information
 *
 * Can be sent to a device to request its firmware information
 */
struct ixxat_usb_fwinfo2_cmd {
	struct ixxat_usb_dal_req req;
	struct ixxat_usb_dal_res res;
	struct ixxat_fw_info2 info;
} __packed;

/* USB command buffer */
union ixxat_usb_cmd {
	struct ixxat_usb_caps_cmd caps;
	struct ixxat_usb_info_cmd info;
	struct ixxat_usb_fwinfo_cmd fwinfo;
	struct ixxat_usb_fwinfo2_cmd fwinfo2;
	struct ixxat_usb_start_cmd start;
	struct ixxat_usb_stop_cmd stop;
	struct ixxat_usb_power_cmd power;
	struct ixxat_usb_dal_cmd dal;

	struct ixxat_usb_init_cl1_cmd cl1;
	struct ixxat_usb_getcaps_cl1_cmd caps_cl1;

	struct ixxat_usb_init_cl2_cmd cl2;
	struct ixxat_usb_getcaps_cl2_cmd caps_cl2;
};

/* struct ixxat_usb_device_data IXXAT USB device data
 * @timeref_valid: Time reference valid
 * @kt_host_start: Host start time
 * @ts_dev_start: Device start time
 */
struct ixxat_usb_device_data {
	/* used to lock write access to the device members */
	spinlock_t access_lock;

	/* lock access to command channel on endpoint 0 */
	struct mutex cmd_channel_lock;

	/* device and firmware info */
	struct ixxat_dev_info dev_info;
	struct ixxat_fw_info2 fw_info;

	/* controller capabilities */
	struct ixxat_cancaps2 caps;

	/* time reference of first controller start */
	bool    timeref_valid;
	ktime_t kt_host_start;
	u32     ts_dev_start;

	/* USB commands buffer */
	union ixxat_usb_cmd *cmdbuf;
};

/* struct ixxat_usb_adapter IXXAT USB device adapter
 * @clock: Clock frequency
 * @bt: Bittiming constants
 * @btd: Data bittiming constants
 * @tdc: Transmission Delay Compensation constants
 * @modes: Supported modes
 * @buffer_size_rx: Buffer size for receiving
 * @buffer_size_tx: Buffer size for transfer
 * @ep_msg_in: USB endpoint buffer for incoming messages
 * @ep_msg_out: USB endpoint buffer for outgoing messages
 * @ep_offs: Endpoint offset (device depended)
 * @cl1_msg_format: Set if the device uses the CL1 CAN message wire format
 *
 * Device Adapter for IXXAT USB devices
 */
struct ixxat_usb_adapter {
	const u32 modes;
	const u32 clock;
	const struct can_bittiming_const *bt;
	const struct can_bittiming_const *btd;
	const struct can_tdc_const *tdc;
	const u16 buffer_size_rx;
	const u16 buffer_size_tx;
	const u8 ep_msg_in[IXXAT_USB_MAX_CHANNEL];
	const u8 ep_msg_out[IXXAT_USB_MAX_CHANNEL];
	const u8 ep_offs;
	const bool cl1_msg_format;

	int (*get_ctrl_caps)(struct ixxat_usb_candevice *dev,
			     struct ixxat_cancaps2 *caps);
	int (*init_ctrl)(struct ixxat_usb_candevice *dev);
};

/* ixxat_usb_setup_cmd() - Setup a device command
 * @req: Request block
 * @res: Response block
 *
 * This function sets the default values in the request and the response block
 * of a device command
 */
void ixxat_usb_setup_cmd(struct ixxat_usb_dal_req *req,
			 struct ixxat_usb_dal_res *res);

/* ixxat_usb_send_cmd - send a command to the IXXAT USB device
 * @dev: pointer to the USB device
 * @port: port number to send the command to
 * @cmd: pointer to the entire command buffer to send,
 *       - starting with a struct ixxat_usb_dal_req object,
 *       - ending with a struct ixxat_usb_dal_res object
 * @cmd_size: size of the whole command (>= sizeof(struct ixxat_usb_dal_cmd))
 * @res: pointer to the response structure
 * @res_size: size of the response structure (>= sizeof(struct ixxat_usb_dal_res)
 * @cmd_delay: delay in milliseconds to wait for a response
 *
 * This function sends a command to the IXXAT USB device and waits for the
 * response, if it is still connected.
 *
 * Returns >= 0 on success, negative error code on failure.
 * If the device is disconnected it returns -ENODEV.
 */
int ixxat_usb_send_cmd(struct ixxat_usb_candevice *pdev, const u16 port,
		       struct ixxat_usb_dal_req *cmd, u16 cmd_size,
		       struct ixxat_usb_dal_res *res, u16 res_size,
		       const unsigned long cmd_delay);

#endif /* IXXAT_USB_CORE_H */
