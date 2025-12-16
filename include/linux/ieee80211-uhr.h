/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * IEEE 802.11 UHR definitions
 *
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */
#ifndef LINUX_IEEE80211_UHR_H
#define LINUX_IEEE80211_UHR_H

/* UHR MAC capabilities as defined in P802.11bn_D1.0 section 9.4.2.aa2.2 */
#define IEEE80211_UHR_MAC_CAP0_DPS_SUPPORT				0x01
#define IEEE80211_UHR_MAC_CAP0_DPS_ASSISTING_SUPPORT			0x02
#define IEEE80211_UHR_MAC_CAP0_DPS_AP_STATIC_HCM_SUPPORT		0x04
#define IEEE80211_UHR_MAC_CAP0_ML_POWER_MANAGEMENT			0x08
#define IEEE80211_UHR_MAC_CAP0_NPCA_SUPPORTED				0x10
#define IEEE80211_UHR_MAC_CAP0_ENHANCED_BSR_SUPPORT			0x20
#define IEEE80211_UHR_MAC_CAP0_ADDITIONAL_MAPPED_TID_SUPPORT		0x40
#define IEEE80211_UHR_MAC_CAP0_EOTSP_SUPPORT				0x80
#define IEEE80211_UHR_MAC_CAP1_DSO_SUPPORT				0x01
#define IEEE80211_UHR_MAC_CAP1_P_EDCA_SUPPORT				0x02
#define IEEE80211_UHR_MAC_CAP1_DBE_SUPPORT				0x04
#define IEEE80211_UHR_MAC_CAP1_UL_LLI_SUPPORT				0x08
#define IEEE80211_UHR_MAC_CAP1_P2P_LLI_SUPPORT				0x10
#define IEEE80211_UHR_MAC_CAP1_PUO_SUPPORT				0x20
#define IEEE80211_UHR_MAC_CAP1_AP_PUO_SUPPORT				0x40
#define IEEE80211_UHR_MAC_CAP1_DUO_SUPPORT				0x80
#define IEEE80211_UHR_MAC_CAP2_OM_UL_MU_DATA_DIS_RX_SUPPORT		0x01
#define IEEE80211_UHR_MAC_CAP2_AOM_SUPPORT				0x02
#define IEEE80211_UHR_MAC_CAP2_IFCS_SUPPORT				0x04
#define IEEE80211_UHR_MAC_CAP2_UHR_TRS_SUPPORT				0x08
#define IEEE80211_UHR_MAC_CAP2_TXSPG_SUPPORT				0x10
#define IEEE80211_UHR_MAC_CAP2_TXOP_RETURN_SUPPORT_INTXSPG		0x20
#define IEEE80211_UHR_MAC_CAP2_UHR_OPER_MODE_PARAM_UPDATE_TIMEOUT_MASK	0xc0
#define IEEE80211_UHR_MAC_CAP3_UHR_OPER_MODE_PARAM_UPDATE_TIMEOUT_MASK	0x03
#define IEEE80211_UHR_MAC_CAP3_PARAM_UPDATE_ADV_NOTIFY_INT_MASK		0x1c
#define IEEE80211_UHR_MAC_CAP3_UPDATE_IND_IN_TIM_INT_MASK		0xe0
#define IEEE80211_UHR_MAC_CAP4_UPDATE_IND_IN_TIM_INT_MASK		0x03
#define IEEE80211_UHR_MAC_CAP4_BOUNDED_ESS				0x04
#define IEEE80211_UHR_MAC_CAP4_BTM_ASSURANCE				0x08

/* UHR PHY capabilities as defined in P802.11bn_D1.0 section 9.4.2.aa2.3 */
#define IEEE80211_UHR_PHY_CAP0_MAX_NSS_RX_NDP_SOUNDING_80MHZ		0x01
#define IEEE80211_UHR_PHY_CAP0_MAX_NSS_TOTAL_RX_DL_MUMIMO_80MHZ		0x02
#define IEEE80211_UHR_PHY_CAP0_MAX_NSS_RX_NDP_SOUNDING_160MHZ		0x04
#define IEEE80211_UHR_PHY_CAP0_MAX_NSS_TOTAL_RX_DL_MUMIMO_160MHZ	0x08
#define IEEE80211_UHR_PHY_CAP0_MAX_NSS_RX_NDP_SOUNDING_320MHZ		0x10
#define IEEE80211_UHR_PHY_CAP0_MAX_NSS_TOTAL_RX_DL_MUMIMO_320MHZ	0x20
#define IEEE80211_UHR_PHY_CAP0_ELR_RX_SUPPORT				0x40
#define IEEE80211_UHR_PHY_CAP0_ELR_TX_SUPPORT				0x80

/**
 * struct ieee80211_uhr_mcs_nss_supp_20mhz_only - UHR 20 MHz only station max
 * supported NSS for per MCS.
 *
 * As per spec P802.11bn_D1.0 38.3.11 - "UHR-MCS 0-15 are the same as
 * EHT-MCS 0-15"
 *
 * For each field below, bits 0 - 3 indicate the maximal number of spatial
 * streams for Rx, and bits 4 - 7 indicate the maximal number of spatial streams
 * for Tx.
 *
 * @rx_tx_mcs7_max_nss: indicates the maximum number of spatial streams
 *     supported for reception and the maximum number of spatial streams
 *     supported for transmission for MCS 0 - 7.
 * @rx_tx_mcs9_max_nss: indicates the maximum number of spatial streams
 *     supported for reception and the maximum number of spatial streams
 *     supported for transmission for MCS 8 - 9.
 * @rx_tx_mcs11_max_nss: indicates the maximum number of spatial streams
 *     supported for reception and the maximum number of spatial streams
 *     supported for transmission for MCS 10 - 11.
 * @rx_tx_mcs13_max_nss: indicates the maximum number of spatial streams
 *     supported for reception and the maximum number of spatial streams
 *     supported for transmission for MCS 12 - 13.
 * @rx_tx_max_nss: array of the previous fields for easier loop access
 */
struct ieee80211_uhr_mcs_nss_supp_20mhz_only {
	union {
		struct {
			u8 rx_tx_mcs7_max_nss;
			u8 rx_tx_mcs9_max_nss;
			u8 rx_tx_mcs11_max_nss;
			u8 rx_tx_mcs13_max_nss;
		};
		u8 rx_tx_max_nss[4];
	};
} __packed;

#define IEEE80211_UHR_DBE_CAP_MAX_BW			GENMASK(2, 0)
#define IEEE80211_UHR_DBE_CAP_EHT_MCS_BW_160MHZ_PRES	BIT(3)
#define IEEE80211_UHR_DBE_CAP_EHT_MCS_BW_320MHZ_PRES	BIT(4)
#define IEEE80211_UHR_DBE_CAP_RESERVED			GENMASK(7, 5)
#define IEEE80211_UHR_DBE_CAP_EHT_MCS_BW_160MHZ		GENMASK(31, 8)
#define IEEE80211_UHR_DBE_CAP_EHT_MCS_BW_320MHZ		GENMASK(55, 32)

/**
 * struct ieee80211_dbe_cap - UHR DBE Capability parameter
 *
 * As per spec P802.11bn_D1.0 Figure 9-aa8 "DBE Capability Parameters
 * field format"
 *
 * Please refer IEEE80211_UHR_DBE_CAP*
 * @dbe_cap_param:
 *      DBE Maximum Supported Bandwidth -  indicates the maximum bandwidth
 *                                         that the AP supports for
 *                                         DBE operation.
 *      EHT-MCS Map (BW=160 MHz) Present - indicates whether the EHT-MCS Map
 *                                         (BW=160 MHz) field is present in
 *                                         the DBE Capability Parameters field
 *      EHT-MCS Map (BW=320 MHz) Present - indicates whether the EHT-MCS Map
 *                                         (BW=320 MHz) field is present in
 *                                         the DBE Capability Parameters field
 *      EHT-MCS Map (BW=160 MHz) - indicates the combinations of EHT-MCS 0-13,
 *                                 and number of spatial streams NSS, that the
 *                                 AP supports for reception and the
 *                                 combinations that it supports for
 *                                 transmission for 160 MHz DBE bandwidth
 *      EHT-MCS Map (BW=320 MHz) - indicates the combinations of EHT-MCS 0-13,
 *                                 and number of spatial streams NSS, that the
 *                                 AP supports for reception and the
 *                                 combinations that it supports for
 *                                 transmission for 320 MHz DBE bandwidth
 */
struct ieee80211_dbe_cap {
	__le64 dbe_cap_param;
} __packed;

/**
 * struct ieee80211_uhr_cap_elem_fixed - UHR capabilities fixed data
 *
 * This structure is the "UHR Capabilities element" fixed fields as
 * described in P802.11bn_D1.0 section 9.4.2.aa2.
 *
 * @mac_cap_info: MAC capabilities, see IEEE80211_UHR_MAC_CAP*
 * @dbe_cap: DBE Capabilities, see IEEE80211_UHR_DBE_CAP*
 * @phy_cap_info: PHY capabilities, see IEEE80211_UHR_PHY_CAP*
 */
struct ieee80211_uhr_cap_elem_fixed {
	u8 mac_cap_info[5];
	struct ieee80211_dbe_cap dbe_cap;
	u8 phy_cap_info[2];
} __packed;

/**
 * struct ieee80211_uhr_cap_elem - UHR capabilities element
 * @fixed: fixed parts, see &ieee80211_uhr_cap_elem_fixed
 */
struct ieee80211_uhr_cap_elem {
	struct ieee80211_uhr_cap_elem_fixed fixed;
} __packed;

#define IEEE80211_UHR_OPER_DPS_ENABLED		BIT(0)
#define IEEE80211_UHR_OPER_NPCA_ENABLED		BIT(1)
#define IEEE80211_UHR_OPER_DBE_ENABLED		BIT(2)
#define IEEE80211_UHR_OPER_P_EDCA_ENABLED	BIT(3)

#define IEEE80211_UHR_DBE_BANDWIDTH			GENMASK(2, 0)
#define IEEE80211_UHR_DBE_RESERVED			GENMASK(7, 3)
#define IEEE80211_UHR_DBE_DIS_SUBCHANNEL_BITMAP		GENMASK(15, 0)
/**
 * struct ieee80211_dbe_info - dbe operation information
 *
 * This structure is the "UHR Operation Element" fields as described
 * in P802.11bn_D1.0 section 9.4.2.aa1. Refer Figure 9-aa5.
 *
 * Please refer IEEE80211_UHR_DBE*
 * @dbe_bandwidth: DBE Bandwidth field is set to indicate
 *     expanded bandwidth for DBE mode
 *     Value 0 is reserved.
 *     Set to 1 to indicate 40 MHz DBE bandwidth.
 *     Set to 2 to indicate 80 MHz DBE bandwidth.
 *     Set to 3 to indicate 160 MHz DBE bandwidth.
 *     Set to 4 to indicate 320-1 MHz DBE bandwidth.
 *     Set to 5 to indicate 320-2 MHz DBE bandwidth.
 *     Values 6 to 7 are reserved.
 * @dbe_disabled_subchannel_bitmap: DBE Disabled Subchannel
 *     Bitmap field is set to indicate disabled 20 MHz subchannels
 *     within the DBE Bandwidth.
 */
struct ieee80211_dbe_info {
	u8 dbe_bandwidth;
	__le16 dbe_disabled_subchannel_bitmap;
} __packed;

#define IEEE80211_UHR_P_EDCA_ECWMIN		GENMASK(3, 0)
#define IEEE80211_UHR_P_EDCA_ECWMAX		GENMASK(7, 4)
#define IEEE80211_UHR_P_EDCA_AIFSN		GENMASK(3, 0)
#define IEEE80211_UHR_P_EDCA_CW_DS		GENMASK(5, 4)
#define IEEE80211_UHR_P_EDCA_PSRC_THRESHOLD	GENMASK(8, 6)
#define IEEE80211_UHR_P_EDCA_QSRC_THRESHOLD	GENMASK(10, 9)
#define IEEE80211_UHR_P_EDCA_RESERVED		GENMASK(14, 11)
/**
 * struct ieee80211_p_edca_info - p_edca operation information
 *
 * This structure is the "UHR Operation Element" fields as described
 * in P802.11bn_D1.0 section 9.4.2.aa1. Refer Figure 9-aa4.
 *
 * Please refer IEEE80211_UHR_P_EDCA*
 * @p_edca_ec: The P-EDCA ECWmin, P-EDCA and ECWmax
 *     fields indicate the CWmin and CWmax
 *     value that are used by a P-EDCA STA during P-EDCA contention.
 * @p_edca_params: The AIFSN field indicate the AIFSN value that are
 *     used by a P-EDCA STA during P-EDCA contention.
 *     The CW DS field indicate the value used
 *     for the randomization of the transmission slot of the DS-CTS
 *     frame. The value 3 is reserved. The value 0 indicate that
 *     randomization not enabled.
 *     The P-EDCA PSRC threshold field indicates the maximum number
 *     of allowed consecutive DS-CTS transmissions. The value 0 and
 *     values greater than 4 are reserved
 *      The P-EDCA QSRC threshold field indicates the value of the
 *      QSRC[AC_VO] counter to be allowed to start P-EDCA contention.
 *      The value 0 is reserved
 */
struct ieee80211_p_edca_info {
	u8 p_edca_ec;
	__le16 p_edca_params;
} __packed;

#define IEEE80211_UHR_NPCA_PRIMARY_CHAN			GENMASK(3, 0)
#define IEEE80211_UHR_NPCA_MIN_DUR_THRESHOLD		GENMASK(7, 4)
#define IEEE80211_UHR_NPCA_SWITCHING_DELAY		GENMASK(13, 8)
#define IEEE80211_UHR_NPCA_SWITCH_BACK_DELAY		GENMASK(19, 14)
#define IEEE80211_UHR_NPCA_INITIAL_QSRC			GENMASK(21, 20)
#define IEEE80211_UHR_NPCA_MOPLEN			BIT(22)
#define IEEE80211_UHR_NPCA_DIS_SUBCHAN_BITMAP_PRESENT	BIT(23)
#define IEEE80211_UHR_NPCA_RESERVED			GENMASK(31, 24)

/**
 * struct ieee80211_npca_info - npca operation information
 *
 * This structure is the "UHR Operation Element" fields as described
 * in P802.11bn_D1.0 section 9.4.2.aa1. Refer Figure 9-aa3.
 *
 * Please refer IEEE80211_UHR_NPCA*
 * @npca_params:
 *     npca_primary_chan - NPCA primary channel
 *     npca_min_dur_threshold - Minimum duration of inter-BSS activity
 *     npca_switching_delay -  Time needed by an NPCA AP to switch from the
 *                             BSS primary channel to the NPCA primary channel
 *                             in the unit of 4 µs.
 *     npca_switch_back_delay - Time to switch from the NPCA primary channel
 *                              to the BSS primary channel in the unit of 4 µs.
 *     npca_initial_qsrc -  initialize the EDCAF QSRC[AC] variables
 *                          when an NPCA STA in the BSS
 *                          switches to NPCA operation.
 *     npca_moplen - indicates which conditions can be used to
 *                   initiate an NPCA operation,
 *                   1 -> both PHYLEN NPCA operation and MOPLEN
 *                        NPCA operation are
 *                        permitted in the BSS
 *                   0 -> only PHYLEN NPCA operation is allowed in the BSS.
 * @npca_disabled_subchan_bitmap: indicates whether the NPCA
 *                                Disabled Subchannel
 *                                Bitmap field is present.
 */
struct ieee80211_npca_info {
	__le32 npca_params;
	__le16 npca_disabled_subchan_bitmap;
} __packed;

#define IEEE80211_UHR_DPS_PADDING_DELAY			GENMASK(5, 0)
#define IEEE80211_UHR_DPS_RESERVED1			GENMASK(7, 6)
#define IEEE80211_UHR_DPS_TRANSITION_DELAY		GENMASK(13, 8)
#define IEEE80211_UHR_DPS_RESERVED2			GENMASK(15, 14)
#define IEEE80211_UHR_DPS_ICF_REQUIRED			BIT(16)
#define IEEE80211_UHR_DPS_PARAMETERIZED_FLAG		BIT(17)
#define IEEE80211_UHR_DPS_LC_MODE_BW			GENMASK(20, 18)
#define IEEE80211_UHR_DPS_LC_MODE_NSS			GENMASK(24, 21)
#define IEEE80211_UHR_DPS_LC_MODE_MCS			GENMASK(28, 25)
#define IEEE80211_UHR_DPS_MOBILE_AP_DPS_STATIC_HCM	BIT(29)
#define IEEE80211_UHR_DPS_RESERVED3			GENMASK(31, 30)

/**
 * struct ieee80211_dps_info - dps operation information
 *
 * This structure is the "UHR Operation Element" fields as described
 * in P802.11bn_D1.0 section 9.4.1.87. Refer Figure 9-207u.
 *
 * Please refer IEEE80211_UHR_DPS*
 * @dps_delay_params:
 *     DPS Padding Delay - indicates the minimum MAC padding
 *                         duration that is required by a DPS STA
 *                         in an ICF to cause the STA to transition
 *                         from the lower capability mode to the
 *                         higher capability mode. The DPS Padding
 *                         Delay field is in units of 4 µs.
 *     DPS Transition - indicates the amount of time required by a
 *                      DPS STA to transition from the higher
 *                      capability mode to the lower capability
 *                      mode. The DPS Transition Delay field is in
 *                      units of 4 µs.
 * @dps_params:
 *      ICF Required - indicates when the DPS assisting STA needs
 *                     to transmit an ICF frame to the peer DPS STA
 *                     before performing the frame exchanges with
 *                     the peer DPS STA in a TXOP.
 *                     1 -> indicates that the transmission of the
 *                          ICF frame to the peer DPS STA prior to
 *                          any frame exchange is needed.
 *                     0 -> ICF transmission before the frame
 *                          exchanges with the peer DPS STA is only
 *                          needed if the frame exchange is performed
 *                          in the HC mode.
 *      Parameterized Flag - 0 -> indicate that only 20 MHz, 1 SS,
 *                                non-HT PPDU format with the data
 *                                rate of 6, 12, and 24 Mb/s as the
 *                                default mode are supported by the
 *                                DPS STA in the LC mode
 *                           1 -> indicate that a bandwidth up to the
 *                                bandwidth indicated in the LC Mode
 *                                Bandwidth field, a number of spatial
 *                                streams up to the NSS indicated in
 *                                the LC Mode Nss field, and an MCS up
 *                                to the MCS indicated in the LC Mode
 *                                MCS fields are supported by the DPS
 *                                STA in the LC mode as the
 *                                parameterized mode.
 *      LC Mode Bandwidth - indicates the maximum bandwidth supported
 *                          by the STA in the LC mode.
 *      LC Mode Nss - indicates the maximum number of the spatial
 *                    streams supported by the STA in the LC mode.
 *      LC Mode MCS - indicates the highest MCS supported by the STA
 *                    in the LC mode.
 *      Mobile AP DPS Static HCM -
 *                    1 -> indicate that it will remain in the DPS high
 *                         capability mode until the next TBTT on that
 *                         link.
 *                    0 -> otherwise.
 */
struct ieee80211_dps_info {
	__le16 dps_delay_params;
	__le16 dps_params;
} __packed;

/**
 * struct ieee80211_uhr_operation_info - uhr operation information
 *
 * @dps_info: DPS operation information
 * @npca_info: NPCA operation information
 * @p_edca_info: P-EDCA operation information
 * @dbe_info: DBE operation information
 */
struct ieee80211_uhr_operation_info {
	struct ieee80211_dps_info dps_info;
	struct ieee80211_npca_info npca_info;
	struct ieee80211_p_edca_info p_edca_info;
	struct ieee80211_dbe_info dbe_info;
} __packed;

/**
 * struct ieee80211_uhr_operation - uhr operation element
 *
 * This structure is the "UHR Operation Element" fields as
 * described in P802.11bn_D1.0 section 9.4.2.aa1
 *
 * @params: UHR operation element parameters. See &IEEE80211_UHR_OPER_*
 * @basic_mcs_nss: indicates the UHR-MCSs for each number of spatial streams in
 *     UHR PPDUs that are supported by all UHR STAs in the BSS in transmit and
 *     receive.
 * @optional: optional parts
 */

struct ieee80211_uhr_operation {
	__le16 params;
	struct ieee80211_uhr_mcs_nss_supp_20mhz_only basic_mcs_nss;
	u8 optional[];
} __packed;

static inline bool
ieee80211_uhr_capa_size_ok(const u8 *data, u8 len)
{
	const struct ieee80211_uhr_cap_elem_fixed *elem = (const void *)data;
	u8 needed = sizeof(struct ieee80211_uhr_cap_elem_fixed) -
		sizeof(struct ieee80211_dbe_cap);

	if (elem->mac_cap_info[1] &
	    IEEE80211_UHR_MAC_CAP1_DBE_SUPPORT) {
		needed += 1;

		if (le64_to_cpu(elem->dbe_cap.dbe_cap_param) &
		    IEEE80211_UHR_DBE_CAP_EHT_MCS_BW_160MHZ_PRES)
			needed += 3;

		if (le64_to_cpu(elem->dbe_cap.dbe_cap_param) &
		    IEEE80211_UHR_DBE_CAP_EHT_MCS_BW_320MHZ_PRES)
			needed += 3;
	}

	return len >= needed;
}

static inline bool
ieee80211_uhr_oper_size_ok(const u8 *data, u8 len, bool is_bcn)
{
	const struct ieee80211_uhr_operation *elem = (const void *)data;
	u8 needed = sizeof(*elem);

	if (len < needed)
		return false;

	if (is_bcn)
		return len >= needed;

	if (le16_to_cpu(elem->params) & IEEE80211_UHR_OPER_DPS_ENABLED)
		needed += sizeof(struct ieee80211_dps_info);
	if (le16_to_cpu(elem->params) & IEEE80211_UHR_OPER_NPCA_ENABLED)
		needed += sizeof(struct ieee80211_npca_info);
	if (le16_to_cpu(elem->params) & IEEE80211_UHR_OPER_DBE_ENABLED)
		needed += sizeof(struct ieee80211_dbe_info);
	if (le16_to_cpu(elem->params) & IEEE80211_UHR_OPER_P_EDCA_ENABLED)
		needed += sizeof(struct ieee80211_p_edca_info);

	return len >= needed;
}

#endif /* LINUX_IEEE80211_UHR_H */
