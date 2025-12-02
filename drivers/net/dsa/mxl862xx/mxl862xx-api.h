/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * struct mdio_relay_data - relayed access to the switch internal MDIO bus
 * @data: data to be read or written
 * @phy: PHY index
 * @mmd: MMD device
 * @reg: register rndex
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
 * struct mxl862xx_ss_sp_tag
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
 * @MXL862XX_LOGICAL_PORT_GPON: GPON OMCI context
 * @MXL862XX_LOGICAL_PORT_EPON: EPON context
 * @MXL862XX_LOGICAL_PORT_GINT: G.INT context
 * @MXL862XX_LOGICAL_PORT_OTHER: Others
 */
enum mxl862xx_logical_port_mode {
	MXL862XX_LOGICAL_PORT_8BIT_WLAN = 0,
	MXL862XX_LOGICAL_PORT_9BIT_WLAN,
	MXL862XX_LOGICAL_PORT_GPON,
	MXL862XX_LOGICAL_PORT_EPON,
	MXL862XX_LOGICAL_PORT_GINT,
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
	enum mxl862xx_logical_port_mode mode;
	__le16 bridge_port_id;
} __packed;

/**
 * struct mxl862xx_sys_fw_image_version - VLAN counter mapping configuration
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
