/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved. */

#ifndef HINIC3_NIC_CFG_H
#define HINIC3_NIC_CFG_H

#include <linux/types.h>
#include <linux/mutex.h>

#include "hinic3_hwdev.h"
#include "hinic3_mgmt_interface.h"

struct hinic3_nic_dev;

#define HINIC3_MIN_MTU_SIZE             256
#define HINIC3_MAX_JUMBO_FRAME_SIZE     9600

#define HINIC3_PF_SET_VF_ALREADY        0x4
#define HINIC3_MGMT_STATUS_EXIST        0x6
#define CHECK_IPSU_15BIT                0x8000

enum hinic3_nic_event_type {
	EVENT_NIC_LINK_DOWN,
	EVENT_NIC_LINK_UP,
	EVENT_NIC_PORT_MODULE_EVENT,
	EVENT_NIC_DCB_STATE_CHANGE,
};

struct hinic3_sq_attr {
	u8  dma_attr_off;
	u8  pending_limit;
	u8  coalescing_time;
	u8  intr_en;
	u16 intr_idx;
	u32 l2nic_sqn;
	u64 ci_dma_base;
};

enum mag_cmd {
	SERDES_CMD_PROCESS             = 0,

	MAG_CMD_SET_PORT_CFG           = 1,
	MAG_CMD_SET_PORT_ADAPT         = 2,
	MAG_CMD_CFG_LOOPBACK_MODE      = 3,

	MAG_CMD_GET_PORT_ENABLE        = 5,
	MAG_CMD_SET_PORT_ENABLE        = 6,
	MAG_CMD_GET_LINK_STATUS        = 7,
	MAG_CMD_SET_LINK_FOLLOW        = 8,
	MAG_CMD_SET_PMA_ENABLE         = 9,
	MAG_CMD_CFG_FEC_MODE           = 10,

	/* reserved for future use */
	MAG_CMD_CFG_AN_TYPE            = 12,
	MAG_CMD_CFG_LINK_TIME          = 13,

	MAG_CMD_SET_PANGEA_ADAPT       = 15,

	/* bios link 30-49 */
	MAG_CMD_CFG_BIOS_LINK_CFG      = 31,
	MAG_CMD_RESTORE_LINK_CFG       = 32,
	MAG_CMD_ACTIVATE_BIOS_LINK_CFG = 33,

	/* LED */
	MAG_CMD_SET_LED_CFG            = 50,

	/* PHY, reserved for future use */
	MAG_CMD_GET_PHY_INIT_STATUS    = 55,

	MAG_CMD_GET_XSFP_INFO          = 60,
	MAG_CMD_SET_XSFP_ENABLE        = 61,
	MAG_CMD_GET_XSFP_PRESENT       = 62,
	MAG_CMD_SET_XSFP_RW            = 63,
	MAG_CMD_CFG_XSFP_TEMPERATURE   = 64,

	MAG_CMD_WIRE_EVENT             = 100,
	MAG_CMD_LINK_ERR_EVENT         = 101,

	MAG_CMD_EVENT_PORT_INFO        = 150,
	MAG_CMD_GET_PORT_STAT          = 151,
	MAG_CMD_CLR_PORT_STAT          = 152,
	MAG_CMD_GET_PORT_INFO          = 153,
	MAG_CMD_GET_PCS_ERR_CNT        = 154,
	MAG_CMD_GET_MAG_CNT            = 155,
	MAG_CMD_DUMP_ANTRAIN_INFO      = 156,

	MAG_CMD_MAX                    = 0xFF
};

enum mag_cmd_port_speed {
	PORT_SPEED_NOT_SET = 0,
	PORT_SPEED_10MB    = 1,
	PORT_SPEED_100MB   = 2,
	PORT_SPEED_1GB     = 3,
	PORT_SPEED_10GB    = 4,
	PORT_SPEED_25GB    = 5,
	PORT_SPEED_40GB    = 6,
	PORT_SPEED_50GB    = 7,
	PORT_SPEED_100GB   = 8,
	PORT_SPEED_200GB   = 9,
	PORT_SPEED_UNKNOWN
};

enum mag_cmd_port_an {
	PORT_AN_NOT_SET = 0,
	PORT_CFG_AN_ON  = 1,
	PORT_CFG_AN_OFF = 2
};

/* mag_cmd_set_port_cfg config bitmap */
#define MAG_CMD_SET_SPEED      0x1
#define MAG_CMD_SET_AUTONEG    0x2
#define MAG_CMD_SET_FEC        0x4
#define MAG_CMD_SET_LANES      0x8
struct mag_cmd_set_port_cfg {
	struct mgmt_msg_head head;

	u8                   port_id;
	u8                   rsvd0[3];

	u32                  config_bitmap;
	u8                   speed;
	u8                   autoneg;
	u8                   fec;
	u8                   lanes;
	u8                   rsvd1[20];
};

/* mag supported/advertised link mode bitmap */
enum mag_cmd_link_mode {
	LINK_MODE_GE            = 0,
	LINK_MODE_10GE_BASE_R   = 1,
	LINK_MODE_25GE_BASE_R   = 2,
	LINK_MODE_40GE_BASE_R4  = 3,
	LINK_MODE_50GE_BASE_R   = 4,
	LINK_MODE_50GE_BASE_R2  = 5,
	LINK_MODE_100GE_BASE_R  = 6,
	LINK_MODE_100GE_BASE_R2 = 7,
	LINK_MODE_100GE_BASE_R4 = 8,
	LINK_MODE_200GE_BASE_R2 = 9,
	LINK_MODE_200GE_BASE_R4 = 10,
	LINK_MODE_MAX_NUMBERS,

	LINK_MODE_UNKNOWN       = 0xFFFF
};

#define LINK_MODE_GE_BIT               0x1u
#define LINK_MODE_10GE_BASE_R_BIT      0x2u
#define LINK_MODE_25GE_BASE_R_BIT      0x4u
#define LINK_MODE_40GE_BASE_R4_BIT     0x8u
#define LINK_MODE_50GE_BASE_R_BIT      0x10u
#define LINK_MODE_50GE_BASE_R2_BIT     0x20u
#define LINK_MODE_100GE_BASE_R_BIT     0x40u
#define LINK_MODE_100GE_BASE_R2_BIT    0x80u
#define LINK_MODE_100GE_BASE_R4_BIT    0x100u
#define LINK_MODE_200GE_BASE_R2_BIT    0x200u
#define LINK_MODE_200GE_BASE_R4_BIT    0x400u

#define CABLE_10GE_BASE_R_BIT   LINK_MODE_10GE_BASE_R_BIT
#define CABLE_25GE_BASE_R_BIT   (LINK_MODE_25GE_BASE_R_BIT | LINK_MODE_10GE_BASE_R_BIT)
#define CABLE_40GE_BASE_R4_BIT  LINK_MODE_40GE_BASE_R4_BIT
#define CABLE_50GE_BASE_R_BIT \
	(LINK_MODE_50GE_BASE_R_BIT | LINK_MODE_25GE_BASE_R_BIT | LINK_MODE_10GE_BASE_R_BIT)
#define CABLE_50GE_BASE_R2_BIT  LINK_MODE_50GE_BASE_R2_BIT
#define CABLE_100GE_BASE_R2_BIT (LINK_MODE_100GE_BASE_R2_BIT | LINK_MODE_50GE_BASE_R2_BIT)
#define CABLE_100GE_BASE_R4_BIT (LINK_MODE_100GE_BASE_R4_BIT | LINK_MODE_40GE_BASE_R4_BIT)
#define CABLE_200GE_BASE_R4_BIT \
	(LINK_MODE_200GE_BASE_R4_BIT | LINK_MODE_100GE_BASE_R4_BIT | LINK_MODE_40GE_BASE_R4_BIT)

struct mag_cmd_get_port_info {
	struct mgmt_msg_head head;

	u8                   port_id;
	u8                   rsvd0[3];

	u8                   wire_type;
	u8                   an_support;
	u8                   an_en;
	u8                   duplex;

	u8                   speed;
	u8                   fec;
	u8                   lanes;
	u8                   rsvd1;

	u32                  supported_mode;
	u32                  advertised_mode;
	u8                   rsvd2[8];
};

#define MAG_CMD_PORT_DISABLE    0x0
#define MAG_CMD_TX_ENABLE       0x1
#define MAG_CMD_RX_ENABLE       0x2
/* the physical port is disabled only when all pf of the port are set to down,
 * if any pf is enabled, the port is enabled
 */
struct mag_cmd_set_port_enable {
	struct mgmt_msg_head head;

	u16                  function_id;
	u16                  rsvd0;

	/* bitmap bit0:tx_en bit1:rx_en */
	u8                   state;
	u8                   rsvd1[3];
};

/* firmware also use this cmd report link event to driver */
struct mag_cmd_get_link_status {
	struct mgmt_msg_head head;

	u8                   port_id;
	/* 0:link down  1:link up */
	u8                   status;
	u8                   rsvd0[2];
};

/* xsfp wire type, refers to cmis protocol definition */
enum mag_wire_type {
	MAG_CMD_WIRE_TYPE_UNKNOWN   = 0x0,
	MAG_CMD_WIRE_TYPE_MM        = 0x1,
	MAG_CMD_WIRE_TYPE_SM        = 0x2,
	MAG_CMD_WIRE_TYPE_COPPER    = 0x3,
	MAG_CMD_WIRE_TYPE_ACC       = 0x4,
	MAG_CMD_WIRE_TYPE_BASET     = 0x5,
	MAG_CMD_WIRE_TYPE_AOC       = 0x40,
	MAG_CMD_WIRE_TYPE_ELECTRIC  = 0x41,
	MAG_CMD_WIRE_TYPE_BACKPLANE = 0x42
};

#define XSFP_INFO_MAX_SIZE    640
struct mag_cmd_get_xsfp_info {
	struct mgmt_msg_head head;

	u8                   port_id;
	u8                   wire_type;
	u16                  out_len;
	u32                  rsvd;
	u8                   sfp_info[XSFP_INFO_MAX_SIZE];
};

#define MAG_CMD_XSFP_PRESENT    0x0
#define MAG_CMD_XSFP_ABSENT     0x1
struct mag_cmd_get_xsfp_present {
	struct mgmt_msg_head head;

	u8                   port_id;
	/* 0:present, 1:absent */
	u8                   abs_status;
	u8                   rsvd[2];
};

struct mag_cmd_port_stats {
	u64 mac_tx_fragment_pkt_num;
	u64 mac_tx_undersize_pkt_num;
	u64 mac_tx_undermin_pkt_num;
	u64 mac_tx_64_oct_pkt_num;
	u64 mac_tx_65_127_oct_pkt_num;
	u64 mac_tx_128_255_oct_pkt_num;
	u64 mac_tx_256_511_oct_pkt_num;
	u64 mac_tx_512_1023_oct_pkt_num;
	u64 mac_tx_1024_1518_oct_pkt_num;
	u64 mac_tx_1519_2047_oct_pkt_num;
	u64 mac_tx_2048_4095_oct_pkt_num;
	u64 mac_tx_4096_8191_oct_pkt_num;
	u64 mac_tx_8192_9216_oct_pkt_num;
	u64 mac_tx_9217_12287_oct_pkt_num;
	u64 mac_tx_12288_16383_oct_pkt_num;
	u64 mac_tx_1519_max_bad_pkt_num;
	u64 mac_tx_1519_max_good_pkt_num;
	u64 mac_tx_oversize_pkt_num;
	u64 mac_tx_jabber_pkt_num;
	u64 mac_tx_bad_pkt_num;
	u64 mac_tx_bad_oct_num;
	u64 mac_tx_good_pkt_num;
	u64 mac_tx_good_oct_num;
	u64 mac_tx_total_pkt_num;
	u64 mac_tx_total_oct_num;
	u64 mac_tx_uni_pkt_num;
	u64 mac_tx_multi_pkt_num;
	u64 mac_tx_broad_pkt_num;
	u64 mac_tx_pause_num;
	u64 mac_tx_pfc_pkt_num;
	u64 mac_tx_pfc_pri0_pkt_num;
	u64 mac_tx_pfc_pri1_pkt_num;
	u64 mac_tx_pfc_pri2_pkt_num;
	u64 mac_tx_pfc_pri3_pkt_num;
	u64 mac_tx_pfc_pri4_pkt_num;
	u64 mac_tx_pfc_pri5_pkt_num;
	u64 mac_tx_pfc_pri6_pkt_num;
	u64 mac_tx_pfc_pri7_pkt_num;
	u64 mac_tx_control_pkt_num;
	u64 mac_tx_err_all_pkt_num;
	u64 mac_tx_from_app_good_pkt_num;
	u64 mac_tx_from_app_bad_pkt_num;

	u64 mac_rx_fragment_pkt_num;
	u64 mac_rx_undersize_pkt_num;
	u64 mac_rx_undermin_pkt_num;
	u64 mac_rx_64_oct_pkt_num;
	u64 mac_rx_65_127_oct_pkt_num;
	u64 mac_rx_128_255_oct_pkt_num;
	u64 mac_rx_256_511_oct_pkt_num;
	u64 mac_rx_512_1023_oct_pkt_num;
	u64 mac_rx_1024_1518_oct_pkt_num;
	u64 mac_rx_1519_2047_oct_pkt_num;
	u64 mac_rx_2048_4095_oct_pkt_num;
	u64 mac_rx_4096_8191_oct_pkt_num;
	u64 mac_rx_8192_9216_oct_pkt_num;
	u64 mac_rx_9217_12287_oct_pkt_num;
	u64 mac_rx_12288_16383_oct_pkt_num;
	u64 mac_rx_1519_max_bad_pkt_num;
	u64 mac_rx_1519_max_good_pkt_num;
	u64 mac_rx_oversize_pkt_num;
	u64 mac_rx_jabber_pkt_num;
	u64 mac_rx_bad_pkt_num;
	u64 mac_rx_bad_oct_num;
	u64 mac_rx_good_pkt_num;
	u64 mac_rx_good_oct_num;
	u64 mac_rx_total_pkt_num;
	u64 mac_rx_total_oct_num;
	u64 mac_rx_uni_pkt_num;
	u64 mac_rx_multi_pkt_num;
	u64 mac_rx_broad_pkt_num;
	u64 mac_rx_pause_num;
	u64 mac_rx_pfc_pkt_num;
	u64 mac_rx_pfc_pri0_pkt_num;
	u64 mac_rx_pfc_pri1_pkt_num;
	u64 mac_rx_pfc_pri2_pkt_num;
	u64 mac_rx_pfc_pri3_pkt_num;
	u64 mac_rx_pfc_pri4_pkt_num;
	u64 mac_rx_pfc_pri5_pkt_num;
	u64 mac_rx_pfc_pri6_pkt_num;
	u64 mac_rx_pfc_pri7_pkt_num;
	u64 mac_rx_control_pkt_num;
	u64 mac_rx_sym_err_pkt_num;
	u64 mac_rx_fcs_err_pkt_num;
	u64 mac_rx_send_app_good_pkt_num;
	u64 mac_rx_send_app_bad_pkt_num;
	u64 mac_rx_unfilter_pkt_num;
};

struct mag_cmd_port_stats_info {
	struct mgmt_msg_head head;

	u8                   port_id;
	u8                   rsvd0[3];
};

struct mag_cmd_get_port_stat {
	struct mgmt_msg_head      head;

	struct mag_cmd_port_stats counter;
	u64                       rsvd1[15];
};

/* xsfp plug event */
struct mag_cmd_wire_event {
	struct mgmt_msg_head head;

	u8                   port_id;
	/* 0:present, 1:absent */
	u8                   status;
	u8                   rsvd[2];
};

enum link_err_type {
	LINK_ERR_MODULE_UNRECOGENIZED,
	LINK_ERR_NUM,
};

enum port_module_event_type {
	HINIC3_PORT_MODULE_CABLE_PLUGGED,
	HINIC3_PORT_MODULE_CABLE_UNPLUGGED,
	HINIC3_PORT_MODULE_LINK_ERR,
	HINIC3_PORT_MODULE_MAX_EVENT,
};

struct hinic3_port_module_event {
	enum port_module_event_type type;
	enum link_err_type          err_type;
};

struct nic_port_info {
	u8  port_type;
	u8  autoneg_cap;
	u8  autoneg_state;
	u8  duplex;
	u8  speed;
	u8  fec;
	u32 supported_mode;
	u32 advertised_mode;
};

struct nic_pause_config {
	u8 auto_neg;
	u8 rx_pause;
	u8 tx_pause;
};

struct hinic3_port_routine_cmd {
	bool                            mpu_send_sfp_info;
	bool                            mpu_send_sfp_abs;

	struct mag_cmd_get_xsfp_info    std_sfp_info;
	struct mag_cmd_get_xsfp_present abs;
};

struct hinic3_nic_cfg {
	struct semaphore               cfg_lock;

	/* Valid when pfc is disabled */
	bool                           pause_set;
	struct nic_pause_config        nic_pause;

	u8                             pfc_en;
	u8                             pfc_bitmap;

	struct nic_port_info           port_info;

	struct hinic3_port_routine_cmd rt_cmd;
	/* used for copying sfp info */
	struct mutex                   sfp_mutex;
};

int l2nic_msg_to_mgmt_sync(struct hinic3_hwdev *hwdev, u16 cmd,
			   const void *buf_in, u32 in_size,
			   void *buf_out, u32 *out_size);

int hinic3_get_nic_feature_from_hw(struct hinic3_nic_dev *nic_dev);
int hinic3_set_nic_feature_to_hw(struct hinic3_nic_dev *nic_dev);
bool hinic3_test_support(struct hinic3_nic_dev *nic_dev,
			 enum nic_feature_cap feature_bits);
void hinic3_update_nic_feature(struct hinic3_nic_dev *nic_dev, u64 feature_cap);

int hinic3_set_rx_lro_state(struct hinic3_hwdev *hwdev, u8 lro_en,
			    u32 lro_timer, u8 lro_max_pkt_len);
int hinic3_set_rx_vlan_offload(struct hinic3_hwdev *hwdev, u8 en);
int hinic3_set_vlan_fliter(struct hinic3_hwdev *hwdev, u32 vlan_filter_ctrl);

int hinic3_init_function_table(struct hinic3_nic_dev *nic_dev);
int hinic3_set_port_mtu(struct net_device *netdev, u16 new_mtu);

int hinic3_get_default_mac(struct hinic3_hwdev *hwdev, u8 *mac_addr);
int hinic3_set_mac(struct hinic3_hwdev *hwdev, const u8 *mac_addr, u16 vlan_id, u16 func_id);
int hinic3_del_mac(struct hinic3_hwdev *hwdev, const u8 *mac_addr, u16 vlan_id, u16 func_id);
int hinic3_update_mac(struct hinic3_hwdev *hwdev, const u8 *old_mac, u8 *new_mac, u16 vlan_id,
		      u16 func_id);

int hinic3_set_ci_table(struct hinic3_hwdev *hwdev, struct hinic3_sq_attr *attr);
int hinic3_flush_qps_res(struct hinic3_hwdev *hwdev);
int hinic3_force_drop_tx_pkt(struct hinic3_hwdev *hwdev);
int hinic3_set_rx_mode(struct hinic3_hwdev *hwdev, u32 enable);

int hinic3_sync_dcb_state(struct hinic3_hwdev *hwdev, u8 op_code, u8 state);
int hinic3_set_port_enable(struct hinic3_hwdev *hwdev, bool enable);
int hinic3_get_link_status(struct hinic3_hwdev *hwdev, bool *link_status_up);
int hinic3_get_port_info(struct hinic3_hwdev *hwdev, struct nic_port_info *port_info);
int hinic3_set_vport_enable(struct hinic3_hwdev *hwdev, u16 func_id, bool enable);
int hinic3_get_phy_port_stats(struct hinic3_hwdev *hwdev,
			      struct mag_cmd_port_stats *stats);
int hinic3_get_vport_stats(struct hinic3_hwdev *hwdev, u16 func_id,
			   struct hinic3_vport_stats *stats);

int hinic3_add_vlan(struct hinic3_hwdev *hwdev, u16 vlan_id, u16 func_id);
int hinic3_del_vlan(struct hinic3_hwdev *hwdev, u16 vlan_id, u16 func_id);

int hinic3_get_pause_info(struct hinic3_nic_dev *nic_dev,
			  struct nic_pause_config *nic_pause);

#endif
