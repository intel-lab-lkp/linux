/* SPDX-License-Identifier: GPL-2.0-or-later */

#include <linux/if_ether.h>

/**
 * struct mdio_relay_data - relayed access to the switch internal MDIO bus
 * @data: data to be read or written
 * @phy: PHY index
 * @mmd: MMD device
 * @reg: register index
 */
struct mdio_relay_data {
	__le16 data;
	u8 phy;
	u8 mmd;
	__le16 reg;
} __packed;

/* Register access parameter to directly modify internal registers */
struct mxl862xx_register_mod {
	__le16 addr;
	__le16 data;
	__le16 mask;
} __packed;

/**
 * enum mxl862xx_mac_clear_type - MAC table clear type
 * @MXL862XX_MAC_CLEAR_PHY_PORT: clear dynamic entries based on port_id
 * @MXL862XX_MAC_CLEAR_DYNAMIC: clear all dynamic entries
 */
enum mxl862xx_mac_clear_type {
	MXL862XX_MAC_CLEAR_PHY_PORT = 0,
	MXL862XX_MAC_CLEAR_DYNAMIC,
};

/**
 * struct mxl862xx_mac_table_clear - MAC table clear
 * @type: see &enum mxl862xx_mac_clear_type
 * @port_id: physical port id
 */
struct mxl862xx_mac_table_clear {
	u8 type;
	u8 port_id;
} __packed;

/**
 * enum mxl862xx_age_timer - Aging Timer Value.
 * @MXL862XX_AGETIMER_1_SEC: 1 second aging time
 * @MXL862XX_AGETIMER_10_SEC: 10 seconds aging time
 * @MXL862XX_AGETIMER_300_SEC: 300 seconds aging time
 * @MXL862XX_AGETIMER_1_HOUR: 1 hour aging time
 * @MXL862XX_AGETIMER_1_DAY: 24 hours aging time
 * @MXL862XX_AGETIMER_CUSTOM: Custom aging time in seconds
 */
enum mxl862xx_age_timer {
	MXL862XX_AGETIMER_1_SEC = 1,
	MXL862XX_AGETIMER_10_SEC,
	MXL862XX_AGETIMER_300_SEC,
	MXL862XX_AGETIMER_1_HOUR,
	MXL862XX_AGETIMER_1_DAY,
	MXL862XX_AGETIMER_CUSTOM,
};

/**
 * struct mxl862xx_cfg -  Global Switch configuration Attributes
 * @mac_table_age_timer: See &enum mxl862xx_age_timer
 * @age_timer: Custom MAC table aging timer in seconds
 * @max_packet_len: Maximum Ethernet packet length.
 * @learning_limit_action: Automatic MAC address table learning limitation consecutive action
 * @mac_locking_action: Accept or discard MAC port locking violation packets
 * @mac_spoofing_action: Accept or discard MAC spoofing and port MAC locking violation packets
 * @pause_mac_mode_src: Pause frame MAC source address mode
 * @pause_mac_src: Pause frame MAC source address
 */
struct mxl862xx_cfg {
	__le32 mac_table_age_timer; /* enum mxl862xx_age_timer */
	__le32 age_timer;
	__le16 max_packet_len;
	u8 learning_limit_action;
	u8 mac_locking_action;
	u8 mac_spoofing_action;
	u8 pause_mac_mode_src;
	u8 pause_mac_src[ETH_ALEN];
} __packed;

#define MXL862XX_SS_SP_TAG_MASK_RX			BIT(0)
#define MXL862XX_SS_SP_TAG_MASK_TX			BIT(1)
#define MXL862XX_SS_SP_TAG_MASK_RX_PEN			BIT(2)
#define MXL862XX_SS_SP_TAG_MASK_TX_PEN			BIT(3)

#define MXL862XX_SS_SP_TAG_RX_NO_TAG_NO_INSERT		0
#define MXL862XX_SS_SP_TAG_RX_NO_TAG_INSERT		1
#define MXL862XX_SS_SP_TAG_RX_TAG_NO_INSERT		2

#define MXL862XX_SS_SP_TAG_TX_NO_TAG_NO_REMOVE		0
#define MXL862XX_SS_SP_TAG_TX_TAG_REPLACE		1
#define MXL862XX_SS_SP_TAG_TX_TAG_NO_REMOVE		2
#define MXL862XX_SS_SP_TAG_TX_TAG_REMOVE		3

/**
 * struct mxl862xx_ss_sp_tag - Special tag port settings
 * @pid: port ID (1~16)
 * @mask: bit value 1 to indicate valid field
 *	0 - rx
 *	1 - tx
 *	2 - rx_pen
 *	3 - tx_pen
 * @rx: RX special tag mode
 *	0 - packet does NOT have special tag and special tag is NOT inserted
 *	1 - packet does NOT have special tag and special tag is inserted
 *	2 - packet has special tag and special tag is NOT inserted
 * @tx: TX special tag mode
 *	0 - packet does NOT have special tag and special tag is NOT removed
 *	1 - packet has special tag and special tag is replaced
 *	2 - packet has special tag and special tag is NOT removed
 *	3 - packet has special tag and special tag is removed
 * @rx_pen: RX special tag info over preamble
 *	0 - special tag info inserted from byte 2 to 7 are all 0
 *	1 - special tag byte 5 is 16, other bytes from 2 to 7 are 0
 *	2 - special tag byte 5 is from preamble field, others are 0
 *	3 - special tag byte 2 to 7 are from preabmle field
 * @tx_pen: TX special tag info over preamble
 *	0 - disabled
 *	1 - enabled
 */
struct mxl862xx_ss_sp_tag {
	u8 pid;
	u8 mask;
	u8 rx;
	u8 tx;
	u8 rx_pen;
	u8 tx_pen;
} __packed;

/**
 * enum mxl862xx_logical_port_mode - Logical port mode
 * @MXL862XX_LOGICAL_PORT_8BIT_WLAN: WLAN with 8-bit station ID
 * @MXL862XX_LOGICAL_PORT_9BIT_WLAN: WLAN with 9-bit station ID
 * @MXL862XX_LOGICAL_PORT_ETHERNET: Ethernet port
 * @MXL862XX_LOGICAL_PORT_OTHER: Others
 */
enum mxl862xx_logical_port_mode {
	MXL862XX_LOGICAL_PORT_8BIT_WLAN = 0,
	MXL862XX_LOGICAL_PORT_9BIT_WLAN,
	MXL862XX_LOGICAL_PORT_ETHERNET,
	MXL862XX_LOGICAL_PORT_OTHER = 0xFF,
};

/**
 * struct mxl862xx_ctp_port_assignment - CTP Port Assignment/association with logical port
 * @logical_port_id: Logical Port Id. The valid range is hardware dependent
 * @first_ctp_port_id: First CTP Port ID mapped to above logical port ID
 * @number_of_ctp_port: Total number of CTP Ports mapped above logical port ID
 * @mode: See &enum mxl862xx_logical_port_mode
 * @bridge_port_id: Bridge ID (FID)
 */
struct mxl862xx_ctp_port_assignment {
	u8 logical_port_id;
	__le16 first_ctp_port_id;
	__le16 number_of_ctp_port;
	__le32 mode; /* enum mxl862xx_logical_port_mode */
	__le16 bridge_port_id;
} __packed;

/**
 * struct mxl862xx_sys_fw_image_version - Firmware version information
 * @iv_major: firmware major version
 * @iv_minor: firmware minor version
 * @iv_revision: firmware revision
 * @iv_build_num: firmware build number
 */
struct mxl862xx_sys_fw_image_version {
	u8 iv_major;
	u8 iv_minor;
	__le16 iv_revision;
	__le32 iv_build_num;
} __packed;
