/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2025 Broadcom */

#ifndef _BNGE_LINK_H_
#define _BNGE_LINK_H_

#define BNGE_PHY_AUTO_SPEEDS2_MASK	\
	PORT_PHY_CFG_REQ_ENABLES_AUTO_LINK_SPEEDS2_MASK
#define BNGE_PHY_AUTO_SPEED_MASK	\
	PORT_PHY_CFG_REQ_ENABLES_AUTO_LINK_SPEED_MASK
#define BNGE_PHY_AUTO_PAM4_SPEED_MASK	\
	PORT_PHY_CFG_REQ_ENABLES_AUTO_PAM4_LINK_SPEED_MASK
#define BNGE_PHY_FLAGS_RESTART_AUTO	\
	PORT_PHY_CFG_REQ_FLAGS_RESTART_AUTONEG
#define BNGE_PHY_FLAGS_ENA_FORCE_SPEEDS2	\
	PORT_PHY_CFG_REQ_ENABLES_FORCE_LINK_SPEEDS2
#define BNGE_PHY_FLAGS_ENA_FORCE_PM4_SPEED	\
	PORT_PHY_CFG_REQ_ENABLES_FORCE_PAM4_LINK_SPEED

struct bnge_link_info {
	u8			phy_type;
	u8			media_type;
	u8			transceiver;
	u8			phy_addr;
	u8			phy_link_status;
#define BNGE_LINK_LINK		PORT_PHY_QCFG_RESP_LINK_LINK
	u8			phy_state;
#define BNGE_PHY_STATE_ENABLED		0
#define BNGE_PHY_STATE_DISABLED		1

	u8			link_state;
#define BNGE_LINK_STATE_UNKNOWN	0
#define BNGE_LINK_STATE_DOWN	1
#define BNGE_LINK_STATE_UP	2
#define BNGE_LINK_IS_UP(bd)		\
	((bd)->link_info.link_state == BNGE_LINK_STATE_UP)
	u8			active_lanes;
	u8			duplex;
#define BNGE_LINK_DUPLEX_FULL	PORT_PHY_QCFG_RESP_DUPLEX_STATE_FULL
	u8			pause;
#define BNGE_LINK_PAUSE_TX	PORT_PHY_QCFG_RESP_PAUSE_TX
#define BNGE_LINK_PAUSE_RX	PORT_PHY_QCFG_RESP_PAUSE_RX
#define BNGE_LINK_PAUSE_BOTH	(PORT_PHY_QCFG_RESP_PAUSE_RX | \
				 PORT_PHY_QCFG_RESP_PAUSE_TX)
	u8			lp_pause;
	u8			auto_pause_setting;
	u8			force_pause_setting;
	u8			duplex_setting;
	u8			auto_mode;
#define BNGE_AUTO_MODE(mode)	((mode) > BNGE_LINK_AUTO_NONE && \
				 (mode) <= BNGE_LINK_AUTO_MSK)
#define BNGE_LINK_AUTO_NONE     PORT_PHY_QCFG_RESP_AUTO_MODE_NONE
#define BNGE_LINK_AUTO_MSK	PORT_PHY_QCFG_RESP_AUTO_MODE_SPEED_MASK
#define PHY_VER_LEN		3
	u8			phy_ver[PHY_VER_LEN];
	u16			link_speed;
#define BNGE_LINK_SPEED_50GB	PORT_PHY_QCFG_RESP_LINK_SPEED_50GB
#define BNGE_LINK_SPEED_100GB	PORT_PHY_QCFG_RESP_LINK_SPEED_100GB
#define BNGE_LINK_SPEED_200GB	PORT_PHY_QCFG_RESP_LINK_SPEED_200GB
#define BNGE_LINK_SPEED_400GB	PORT_PHY_QCFG_RESP_LINK_SPEED_400GB
#define BNGE_LINK_SPEED_800GB	PORT_PHY_QCFG_RESP_LINK_SPEED_800GB
	u16			support_speeds;
	u16			support_pam4_speeds;
	u16			support_speeds2;

	u16			auto_link_speeds;	/* fw adv setting */
#define BNGE_LINK_SPEED_MSK_50GB PORT_PHY_QCFG_RESP_SUPPORT_SPEEDS_50GB
#define BNGE_LINK_SPEED_MSK_100GB PORT_PHY_QCFG_RESP_SUPPORT_SPEEDS_100GB
	u16			auto_pam4_link_speeds;
#define BNGE_LINK_PAM4_SPEED_MSK_50GB PORT_PHY_QCFG_RESP_SUPPORT_PAM4_SPEEDS_50G
#define BNGE_LINK_PAM4_SPEED_MSK_100GB		\
	PORT_PHY_QCFG_RESP_SUPPORT_PAM4_SPEEDS_100G
#define BNGE_LINK_PAM4_SPEED_MSK_200GB		\
	PORT_PHY_QCFG_RESP_SUPPORT_PAM4_SPEEDS_200G
	u16			auto_link_speeds2;
#define BNGE_LINK_SPEEDS2_MSK_50GB PORT_PHY_QCFG_RESP_SUPPORT_SPEEDS2_50GB
#define BNGE_LINK_SPEEDS2_MSK_100GB PORT_PHY_QCFG_RESP_SUPPORT_SPEEDS2_100GB
#define BNGE_LINK_SPEEDS2_MSK_50GB_PAM4	\
	PORT_PHY_QCFG_RESP_SUPPORT_SPEEDS2_50GB_PAM4_56
#define BNGE_LINK_SPEEDS2_MSK_100GB_PAM4	\
	PORT_PHY_QCFG_RESP_SUPPORT_SPEEDS2_100GB_PAM4_56
#define BNGE_LINK_SPEEDS2_MSK_200GB_PAM4	\
	PORT_PHY_QCFG_RESP_SUPPORT_SPEEDS2_200GB_PAM4_56
#define BNGE_LINK_SPEEDS2_MSK_400GB_PAM4	\
	PORT_PHY_QCFG_RESP_SUPPORT_SPEEDS2_400GB_PAM4_56
#define BNGE_LINK_SPEEDS2_MSK_100GB_PAM4_112	\
	PORT_PHY_QCFG_RESP_SUPPORT_SPEEDS2_100GB_PAM4_112
#define BNGE_LINK_SPEEDS2_MSK_200GB_PAM4_112	\
	PORT_PHY_QCFG_RESP_SUPPORT_SPEEDS2_200GB_PAM4_112
#define BNGE_LINK_SPEEDS2_MSK_400GB_PAM4_112	\
	PORT_PHY_QCFG_RESP_SUPPORT_SPEEDS2_400GB_PAM4_112
#define BNGE_LINK_SPEEDS2_MSK_800GB_PAM4_112	\
	PORT_PHY_QCFG_RESP_SUPPORT_SPEEDS2_800GB_PAM4_112

	u16			support_auto_speeds;
	u16			support_pam4_auto_speeds;
	u16			support_auto_speeds2;

	u16			lp_auto_link_speeds;
	u16			lp_auto_pam4_link_speeds;
	u16			force_link_speed;
	u16			force_pam4_link_speed;
	u16			force_link_speed2;
#define BNGE_LINK_SPEED_50GB_PAM4	\
	PORT_PHY_CFG_REQ_FORCE_LINK_SPEEDS2_50GB_PAM4_56
#define BNGE_LINK_SPEED_100GB_PAM4	\
	PORT_PHY_CFG_REQ_FORCE_LINK_SPEEDS2_100GB_PAM4_56
#define BNGE_LINK_SPEED_200GB_PAM4	\
	PORT_PHY_CFG_REQ_FORCE_LINK_SPEEDS2_200GB_PAM4_56
#define BNGE_LINK_SPEED_400GB_PAM4	\
	PORT_PHY_CFG_REQ_FORCE_LINK_SPEEDS2_400GB_PAM4_56
#define BNGE_LINK_SPEED_100GB_PAM4_112	\
	PORT_PHY_CFG_REQ_FORCE_LINK_SPEEDS2_100GB_PAM4_112
#define BNGE_LINK_SPEED_200GB_PAM4_112	\
	PORT_PHY_CFG_REQ_FORCE_LINK_SPEEDS2_200GB_PAM4_112
#define BNGE_LINK_SPEED_400GB_PAM4_112	\
	PORT_PHY_CFG_REQ_FORCE_LINK_SPEEDS2_400GB_PAM4_112
#define BNGE_LINK_SPEED_800GB_PAM4_112	\
	PORT_PHY_CFG_REQ_FORCE_LINK_SPEEDS2_800GB_PAM4_112

	u32			preemphasis;
	u8			module_status;
	u8			active_fec_sig_mode;
	u16			fec_cfg;
#define BNGE_FEC_NONE		PORT_PHY_QCFG_RESP_FEC_CFG_FEC_NONE_SUPPORTED
#define BNGE_FEC_AUTONEG_CAP	PORT_PHY_QCFG_RESP_FEC_CFG_FEC_AUTONEG_SUPPORTED
#define BNGE_FEC_AUTONEG	PORT_PHY_QCFG_RESP_FEC_CFG_FEC_AUTONEG_ENABLED
#define BNGE_FEC_ENC_BASE_R_CAP	\
	PORT_PHY_QCFG_RESP_FEC_CFG_FEC_CLAUSE74_SUPPORTED
#define BNGE_FEC_ENC_BASE_R	PORT_PHY_QCFG_RESP_FEC_CFG_FEC_CLAUSE74_ENABLED
#define BNGE_FEC_ENC_RS_CAP	\
	PORT_PHY_QCFG_RESP_FEC_CFG_FEC_CLAUSE91_SUPPORTED
#define BNGE_FEC_ENC_LLRS_CAP	\
	(PORT_PHY_QCFG_RESP_FEC_CFG_FEC_RS272_1XN_SUPPORTED |	\
	 PORT_PHY_QCFG_RESP_FEC_CFG_FEC_RS272_IEEE_SUPPORTED)
#define BNGE_FEC_ENC_RS		\
	(PORT_PHY_QCFG_RESP_FEC_CFG_FEC_CLAUSE91_ENABLED |	\
	 PORT_PHY_QCFG_RESP_FEC_CFG_FEC_RS544_1XN_ENABLED |	\
	 PORT_PHY_QCFG_RESP_FEC_CFG_FEC_RS544_IEEE_ENABLED)
#define BNGE_FEC_ENC_LLRS	\
	(PORT_PHY_QCFG_RESP_FEC_CFG_FEC_RS272_1XN_ENABLED |	\
	 PORT_PHY_QCFG_RESP_FEC_CFG_FEC_RS272_IEEE_ENABLED)

	bool			phy_retry;
	unsigned long		phy_retry_expires;

	/* a copy of phy_qcfg output used to report link
	 * info to VF
	 */
	struct hwrm_port_phy_qcfg_output phy_qcfg_resp;
};

struct bnge_ethtool_link_info {
	/* copy of requested setting from ethtool cmd */
	u8			autoneg;
#define BNGE_AUTONEG_SPEED		1
#define BNGE_AUTONEG_FLOW_CTRL		2
	u8			req_signal_mode;
#define BNGE_SIG_MODE_NRZ	PORT_PHY_QCFG_RESP_SIGNAL_MODE_NRZ
#define BNGE_SIG_MODE_PAM4	PORT_PHY_QCFG_RESP_SIGNAL_MODE_PAM4
#define BNGE_SIG_MODE_PAM4_112	PORT_PHY_QCFG_RESP_SIGNAL_MODE_PAM4_112
#define BNGE_SIG_MODE_MAX	(PORT_PHY_QCFG_RESP_SIGNAL_MODE_LAST + 1)
	u8			req_duplex;
	u8			req_flow_ctrl;
	u16			req_link_speed;
	u16			advertising;	/* user adv setting */
	u16			advertising_pam4;
	bool			force_link_chng;
};

void bnge_hwrm_set_eee(struct bnge_dev *bd,
		       struct hwrm_port_phy_cfg_input *req);
void bnge_hwrm_set_link_common(struct bnge_net *bn,
			       struct hwrm_port_phy_cfg_input *req);
void bnge_hwrm_set_pause_common(struct bnge_net *bn,
				struct hwrm_port_phy_cfg_input *req);
int bnge_update_phy_setting(struct bnge_net *bn);
void bnge_get_port_module_status(struct bnge_net *bn);
void bnge_report_link(struct bnge_dev *bd);
bool bnge_support_speed_dropped(struct bnge_net *bn);
void bnge_init_ethtool_link_settings(struct bnge_net *bn);
u32 bnge_fw_to_ethtool_speed(u16 fw_link_speed);
int bnge_probe_phy(struct bnge_net *bd, bool fw_dflt);
bool bnge_phy_qcaps_no_speed(struct hwrm_port_phy_qcaps_output *resp);
int bnge_set_link_ksettings(struct net_device *dev,
			    const struct ethtool_link_ksettings *lk_ksettings);
int bnge_get_link_ksettings(struct net_device *dev,
			    struct ethtool_link_ksettings *lk_ksettings);
u32 bnge_get_link(struct net_device *dev);
u16 bnge_get_force_speed(struct bnge_link_info *link_info);
#endif /* _BNGE_LINK_H_ */
