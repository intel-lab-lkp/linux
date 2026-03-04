// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Qualcomm Technologies, Inc.
 *
 * Author:
 *	Can Guo <can.guo@oss.qualcomm.com>
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <ufs/ufshcd.h>
#include <ufs/unipro.h>
#include "ufshcd-priv.h"

static bool use_adaptive_txeq;
module_param(use_adaptive_txeq, bool, 0644);
MODULE_PARM_DESC(use_adaptive_txeq, "Find and apply optimal TX Equalization settings before power mode change (default: false)");

static int txeq_gear_set(const char *val, const struct kernel_param *kp)
{
	return param_set_uint_minmax(val, kp, UFS_HS_G1, UFS_HS_G6);
}

static const struct kernel_param_ops txeq_gear_ops = {
	.set = txeq_gear_set,
	.get = param_get_uint,
};

static unsigned int adaptive_txeq_gear = UFS_HS_G6;
module_param_cb(adaptive_txeq_gear, &txeq_gear_ops, &adaptive_txeq_gear, 0644);
MODULE_PARM_DESC(adaptive_txeq_gear, "For the HS-Gear[n] and above, adaptive txeq shall be used");

static bool use_txeq_presets = true;
module_param(use_txeq_presets, bool, 0644);
MODULE_PARM_DESC(use_txeq_presets, "Use only the 8 TX Equalization Presets (pre-defined Pre-Shoot & De-Emphasis combinations) for TX EQTR (default: true)");

static bool txeq_presets_selected[UFS_TX_EQ_PRESET_MAX] = {[0 ... (UFS_TX_EQ_PRESET_MAX - 1)] = 1};
module_param_array(txeq_presets_selected, bool, NULL, 0644);
MODULE_PARM_DESC(txeq_presets_selected, "Use only the selected Presets out of the 8 TX Equalization Presets for TX EQTR");

/**
 * ufs_tx_eq_preset - Table of minimum required list of presets.
 *
 * A HS-G6 capable M-TX shall support the presets defined in M-PHY v6.0 spec.
 * Preset	Pre-Shoot(dB)	De-Emphasis(dB)
 * P0		0.0		0.0
 * P1		0.0		0.8
 * P2		0.0		1.6
 * P3		0.8		0.0
 * P4		1.6		0.0
 * P5		0.8		0.8
 * P6		0.8		1.6
 * P7		1.6		0.8
 */
static const struct __ufs_tx_eq_preset {
	unsigned int preshoot;
	unsigned int deemphasis;
} ufs_tx_eq_preset[UFS_TX_EQ_PRESET_MAX] = {
	[UFS_TX_EQ_PRESET_P0] = {UFS_TX_HS_PRESHOOT_DB_0P0, UFS_TX_HS_DEEMPHASIS_DB_0P0},
	[UFS_TX_EQ_PRESET_P1] = {UFS_TX_HS_PRESHOOT_DB_0P0, UFS_TX_HS_DEEMPHASIS_DB_0P8},
	[UFS_TX_EQ_PRESET_P2] = {UFS_TX_HS_PRESHOOT_DB_0P0, UFS_TX_HS_DEEMPHASIS_DB_1P6},
	[UFS_TX_EQ_PRESET_P3] = {UFS_TX_HS_PRESHOOT_DB_0P8, UFS_TX_HS_DEEMPHASIS_DB_0P0},
	[UFS_TX_EQ_PRESET_P4] = {UFS_TX_HS_PRESHOOT_DB_1P6, UFS_TX_HS_DEEMPHASIS_DB_0P0},
	[UFS_TX_EQ_PRESET_P5] = {UFS_TX_HS_PRESHOOT_DB_0P8, UFS_TX_HS_DEEMPHASIS_DB_0P8},
	[UFS_TX_EQ_PRESET_P6] = {UFS_TX_HS_PRESHOOT_DB_0P8, UFS_TX_HS_DEEMPHASIS_DB_1P6},
	[UFS_TX_EQ_PRESET_P7] = {UFS_TX_HS_PRESHOOT_DB_1P6, UFS_TX_HS_DEEMPHASIS_DB_0P8},
};

/**
 * pa_peer_rx_adapt_initial - Table of UniPro PA_PeerRxHSGnAdaptInitial
 * attribute IDs for High Speed (HS) Gears.
 *
 * This table maps HS Gears to their respective UniPro PA_PeerRxHSGnAdaptInitial
 * attribute IDs. Entries for Gears 1-3 are 0 (unsupported).
 */
static const u32 pa_peer_rx_adapt_initial[UFS_HS_GEAR_MAX] = {
	[UFS_HS_G4] = PA_PEERRXHSG4ADAPTINITIAL,
	[UFS_HS_G5] = PA_PEERRXHSG5ADAPTINITIAL,
	[UFS_HS_G6] = PA_PEERRXHSG6ADAPTINITIALL0L3
};

/**
 * rx_adapt_initial_cap - Table of M-PHY RX_HS_Gn_ADAPT_INITIAL_Capability
 * attribute IDs for High Speed (HS) Gears.
 *
 * This table maps HS Gears to their respective M-PHY
 * RX_HS_Gn_ADAPT_INITIAL_Capability attribute IDs. Entries for Gears 1-3 are 0
 * (unsupported).
 */
static const u32 rx_adapt_initial_cap[UFS_HS_GEAR_MAX] = {
	[UFS_HS_G4] = RX_HS_G4_ADAPT_INITIAL_CAP,
	[UFS_HS_G5] = RX_HS_G5_ADAPT_INITIAL_CAP,
	[UFS_HS_G6] = RX_HS_G6_ADAPT_INITIAL_CAP
};

/**
 * pa_tx_eq_setting - Table of UniPro PA_TxEQGnSetting attribute IDs for High
 * Speed (HS) Gears.
 *
 * This table maps HS Gears to their respective UniPro PA_TxEQGnSetting
 * attribute IDs.
 */
static const u32 pa_tx_eq_setting[UFS_HS_GEAR_MAX] = {
	0,
	PA_TXEQG1SETTING,
	PA_TXEQG2SETTING,
	PA_TXEQG3SETTING,
	PA_TXEQG4SETTING,
	PA_TXEQG5SETTING,
	PA_TXEQG6SETTING
};

/**
 * ufshcd_configure_precoding - Configure Pre-Coding for all active lanes
 * @hba: per adapter instance
 * @params: TX EQ parameters data structure
 *
 * Bit[7] in RX_FOM indicates that the receiver needs to enable Pre-Coding when
 * set. Pre-Coding must be enabled on both the transmitter and receiver to
 * ensure proper operation.
 *
 * Returns 0 on success, negative error code otherwise
 */
static int ufshcd_configure_precoding(struct ufs_hba *hba,
				      struct ufshcd_tx_eq_params *params)
{
	u32 local_precode_en = 0;
	u32 peer_precode_en = 0;
	int lane, ret = 0;

	/* Enable Pre-Coding for Host's TX & Device's RX pair */
	for (lane = 0; lane < params->tx_lanes; lane++) {
		if (params->host[lane].precode_en) {
			local_precode_en |= PRECODEEN_TX_BIT(lane);
			peer_precode_en |= PRECODEEN_RX_BIT(lane);
		}
	}

	/* Enable Pre-Coding for Device's TX & Host's RX pair */
	for (lane = 0; lane < params->rx_lanes; lane++) {
		if (params->device[lane].precode_en) {
			peer_precode_en |= PRECODEEN_TX_BIT(lane);
			local_precode_en |= PRECODEEN_RX_BIT(lane);
		}
	}

	if (!local_precode_en && !peer_precode_en) {
		dev_dbg(hba->dev, "Pre-Coding is not required for either side\n");
		return ret;
	}

	/* Set local PA_PreCodeEn */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB(PA_PRECODEEN), local_precode_en);
	if (ret) {
		dev_err(hba->dev, "Failed to set local PA_PreCodeEn: %d\n",
			ret);
		return ret;
	}

	/* Set peer PA_PreCodeEn */
	ret = ufshcd_dme_peer_set(hba, UIC_ARG_MIB(PA_PRECODEEN), peer_precode_en);
	if (ret) {
		dev_err(hba->dev, "Failed to set peer PA_PreCodeEn: %d\n",
			ret);
		return ret;
	}

	dev_dbg(hba->dev, "Pre-Coding configured - Local PA_PreCodeEn: 0x%02x, Peer PA_PreCodeEn: 0x%02x\n",
		local_precode_en, peer_precode_en);

	return ret;
}

void ufshcd_print_tx_eq_params(struct ufs_hba *hba)
{
	struct ufshcd_tx_eq_params *params;
	u32 gear = hba->pwr_info.gear_tx;
	int lane;

	if (!ufshcd_is_tx_eq_supported(hba))
		return;

	if (gear < UFS_HS_G1 || gear >= UFS_HS_GEAR_MAX)
		return;

	params = &hba->tx_eq_params[gear - 1];
	if (!params->is_valid || !params->is_applied)
		return;

	for (lane = 0; lane < params->tx_lanes; lane++)
		dev_dbg(hba->dev, "Host TX Equalization params: Lane %u - PreShoot %u, DeEmphasis %u, FOM value %u, PreCodeEn %d\n",
			lane, params->host[lane].preshoot,
			params->host[lane].deemphasis,
			params->host[lane].fom_val,
			params->host[lane].precode_en);

	for (lane = 0; lane < params->rx_lanes; lane++)
		dev_dbg(hba->dev, "Device TX Equalization params: Lane %u - PreShoot %u, DeEmphasis %u, FOM value %u, PreCodeEn %d\n",
			lane, params->device[lane].preshoot,
			params->device[lane].deemphasis,
			params->device[lane].fom_val,
			params->device[lane].precode_en);
}

static inline u32
ufshcd_compose_tx_eq_setting(struct ufshcd_tx_eq_settings *settings,
			     int num_lanes)
{
	u32 setting = 0;
	int lane;

	for (lane = 0; lane < num_lanes; lane++, settings++) {
		setting |= TX_HS_PRESHOOT_BITS(lane, settings->preshoot);
		setting |= TX_HS_DEEMPHASIS_BITS(lane, settings->deemphasis);
	}

	return setting;
}

/**
 * ufshcd_apply_tx_eq_settings - Apply TX Equalization settings for target gear
 * @hba: per adapter instance
 * @params: TX EQ parameters data structure
 * @gear: target gear
 *
 * Returns 0 on success, negative error code otherwise
 */
static int ufshcd_apply_tx_eq_settings(struct ufs_hba *hba,
				       struct ufshcd_tx_eq_params *params,
				       u32 gear)
{
	u32 setting;
	int ret;

	/* Compose settings for Host's TX Lanes */
	setting = ufshcd_compose_tx_eq_setting(params->host, params->tx_lanes);
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB(pa_tx_eq_setting[gear]), setting);
	if (ret)
		return ret;

	/* Compose settings for Device's TX Lanes */
	setting = ufshcd_compose_tx_eq_setting(params->device, params->rx_lanes);
	ret = ufshcd_dme_peer_set(hba, UIC_ARG_MIB(pa_tx_eq_setting[gear]),
				  setting);
	if (ret)
		return ret;

	/* Configure Pre-Coding */
	if (gear >= UFS_HS_G6) {
		ret = ufshcd_configure_precoding(hba, params);
		if (ret) {
			dev_err(hba->dev, "Failed to configure pre-coding: %d\n",
				ret);
			return ret;
		}
	}

	return ret;
}

/**
 * ufshcd_evaluate_fom - Update TX EQ params based on FOM results
 * @hba: per adapter instance
 * @params: TX EQ parameters data structure
 * @h_iter: host TX EQTR iterator data structure
 * @d_iter: device TX EQTR iterator data structure
 *
 * Evaluate FOM results, update host and device TX EQ params if FOM results are
 * improved, and record TX EQTR results.
 */
static void ufshcd_evaluate_fom(struct ufs_hba *hba,
				struct ufshcd_tx_eq_params *params,
				struct tx_eqtr_iter *h_iter,
				struct tx_eqtr_iter *d_iter)
{
	u32 preshoot, deemphasis, fom_value;
	bool precode_en;
	int lane;

	for (lane = 0; h_iter->is_new && lane < h_iter->num_lanes; lane++) {
		preshoot = h_iter->preshoot;
		deemphasis = h_iter->deemphasis;
		fom_value = h_iter->fom[lane] & RX_FOM_VALUE_MASK;
		precode_en = h_iter->fom[lane] & RX_FOM_PRECODING_EN_MASK;

		/* Record host TX EQTR result */
		params->host_eqtr_record[lane][preshoot][deemphasis] = h_iter->fom[lane];

		/* Check if FOM is improved for host's TX Lanes */
		if (fom_value > params->host[lane].fom_val) {
			params->host[lane].preshoot = preshoot;
			params->host[lane].deemphasis = deemphasis;
			params->host[lane].fom_val = fom_value;
			params->host[lane].precode_en = precode_en;
		}

		dev_dbg(hba->dev, "Host TX EQTR: Lane %u - PreShoot %u, DeEmphasis %u, FOM value %u, PreCodeEn %d\n",
			lane, preshoot, deemphasis, fom_value, precode_en);
	}

	for (lane = 0; d_iter->is_new && lane < d_iter->num_lanes; lane++) {
		preshoot = d_iter->preshoot;
		deemphasis = d_iter->deemphasis;
		fom_value = d_iter->fom[lane] & RX_FOM_VALUE_MASK;
		precode_en = d_iter->fom[lane] & RX_FOM_PRECODING_EN_MASK;

		/* Record device TX EQTR result */
		params->device_eqtr_record[lane][preshoot][deemphasis] = d_iter->fom[lane];

		/* Check if FOM is improved for Device's TX Lanes */
		if (fom_value > params->device[lane].fom_val) {
			params->device[lane].preshoot = preshoot;
			params->device[lane].deemphasis = deemphasis;
			params->device[lane].fom_val = fom_value;
			params->device[lane].precode_en = precode_en;
		}

		dev_dbg(hba->dev, "Device TX EQTR: Lane %u - PreShoot %u, DeEmphasis %u, FOM value %u, PreCodeEn %d\n",
			lane, preshoot, deemphasis, fom_value, precode_en);
	}
}

/**
 * ufshcd_get_rx_fom - Get Figure of Merit (FOM) for both sides
 * @hba: per adapter instance
 * @pwr_mode: target power mode containing gear and rate information
 * @h_iter: host TX EQTR iterator data structure
 * @d_iter: device TX EQTR iterator data structure
 *
 * Returns 0 on success, negative error code otherwise
 */
static int ufshcd_get_rx_fom(struct ufs_hba *hba,
			     struct ufs_pa_layer_attr *pwr_mode,
			     struct tx_eqtr_iter *h_iter,
			     struct tx_eqtr_iter *d_iter)
{
	int lane, ret;

	/* Get FOM of host's TX lanes from device's RX_FOM. */
	for (lane = 0; lane < h_iter->num_lanes; lane++) {
		ret = ufshcd_dme_peer_get(hba, UIC_ARG_MIB_SEL(RX_FOM,
					  UIC_ARG_MPHY_RX_GEN_SEL_INDEX(lane)),
					  &h_iter->fom[lane]);
		if (ret)
			return ret;
	}

	/* Get FOM of device's TX lanes from host's RX_FOM. */
	for (lane = 0; lane < d_iter->num_lanes; lane++) {
		ret = ufshcd_dme_get(hba, UIC_ARG_MIB_SEL(RX_FOM,
				     UIC_ARG_MPHY_RX_GEN_SEL_INDEX(lane)),
				     &d_iter->fom[lane]);
		if (ret)
			return ret;
	}

	ret = ufshcd_vops_get_rx_fom(hba, pwr_mode, h_iter, d_iter);
	if (ret)
		dev_err(hba->dev, "Failed to get FOM via vops: %d\n", ret);

	return ret;
}

/**
 * tx_eqtr_iter_set - Set TX EQTR iterator with supported settings
 * @iter: TX EQTR iterator data structure
 * @preshoot: PreShoot value
 * @deemphasis: DeEmphasis value
 *
 * This function validates whether the requested PreShoot and DeEmphasis
 * combination is supported or not.
 */
static inline void tx_eqtr_iter_set(struct tx_eqtr_iter *iter,
				    unsigned int preshoot,
				    unsigned int deemphasis)
{
	if (!test_bit(preshoot, &iter->preshoot_bitmap) ||
	    !test_bit(deemphasis, &iter->deemphasis_bitmap)) {
		iter->is_new = false;
		return;
	}

	if (use_txeq_presets) {
		bool is_preset = false;

		for (int i = 0; i < UFS_TX_EQ_PRESET_MAX; i++) {
			if (!txeq_presets_selected[i])
				continue;

			if (preshoot == ufs_tx_eq_preset[i].preshoot &&
			    deemphasis == ufs_tx_eq_preset[i].deemphasis) {
				is_preset = true;
				break;
			}
		}

		if (!is_preset) {
			iter->is_new = false;
			return;
		}
	}

	iter->preshoot = preshoot;
	iter->deemphasis = deemphasis;
	iter->is_new = true;
}

/**
 * tx_eqtr_iter_update() - Update TX Equalization training iterators
 * @preshoot: PreShoot value
 * @deemphasis: DeEmphasis value
 * @h_iter: Host TX EQTR iterator data structure
 * @d_iter: Device TX EQTR iterator data structure
 *
 * Updates host and device TX Equalization training iterators with the
 * provided PreShoot and DeEmphasis.
 *
 * Return: true if host and/or device TX Equalization training iterator has
 * been updated to the provided PreShoot and DeEmphasis, false otherwise.
 */
static inline bool tx_eqtr_iter_update(unsigned int preshoot,
				       unsigned int deemphasis,
				       struct tx_eqtr_iter *h_iter,
				       struct tx_eqtr_iter *d_iter)
{
	tx_eqtr_iter_set(h_iter, preshoot, deemphasis);
	tx_eqtr_iter_set(d_iter, preshoot, deemphasis);

	return h_iter->is_new || d_iter->is_new;
}

/**
 * ufshcd_tx_eqtr_data_init - Initialize TX EQTR iteration data and TX EQ params
 * @hba: per adapter instance
 * @params: TX EQ parameters data structure
 * @h_iter: host TX EQTR iterator data structure
 * @d_iter: device TX EQTR iterator data structure
 *
 * This function initializes the TX EQTR iterator structures for both host and
 * device by reading their TX equalization capabilities and setting up the
 * iteration state. The capabilities are cached in the hba structure to avoid
 * redundant DME operations in subsequent calls. The iterator structures are
 * used by tx_eqtr_iter_load() to systematically iterate through all possible TX
 * Equalization setting combinations during the TX EQTR procedure. This function
 * also clears the host and device parameter arrays in the TX EQ params data
 * structure, so that the TX EQTR procedure does not compare FOM values to stale
 * FOM values.
 *
 * Returns 0 on success, negative error code otherwise
 */
static int ufshcd_tx_eqtr_data_init(struct ufs_hba *hba,
				    struct ufshcd_tx_eq_params *params,
				    struct tx_eqtr_iter *h_iter,
				    struct tx_eqtr_iter *d_iter)
{
	u32 cap;
	int ret;

	if (!hba->host_preshoot_cap) {
		ret = ufshcd_dme_get(hba, UIC_ARG_MIB(TX_HS_PRESHOOT_SETTING_CAP), &cap);
		if (ret)
			return ret;

		hba->host_preshoot_cap = cap & TX_EQTR_CAP_MASK;
	}

	if (!hba->host_deemphasis_cap) {
		ret = ufshcd_dme_get(hba, UIC_ARG_MIB(TX_HS_DEEMPHASIS_SETTING_CAP), &cap);
		if (ret)
			return ret;

		hba->host_deemphasis_cap = cap & TX_EQTR_CAP_MASK;
	}

	if (!hba->device_preshoot_cap) {
		ret = ufshcd_dme_peer_get(hba, UIC_ARG_MIB(TX_HS_PRESHOOT_SETTING_CAP), &cap);
		if (ret)
			return ret;

		hba->device_preshoot_cap = cap & TX_EQTR_CAP_MASK;
	}

	if (!hba->device_deemphasis_cap) {
		ret = ufshcd_dme_peer_get(hba, UIC_ARG_MIB(TX_HS_DEEMPHASIS_SETTING_CAP), &cap);
		if (ret)
			return ret;

		hba->device_deemphasis_cap = cap & TX_EQTR_CAP_MASK;
	}

	memset(params->host, 0, sizeof(params->host));
	memset(params->device, 0, sizeof(params->device));
	memset(params->host_eqtr_record, 0xFF, sizeof(params->host_eqtr_record));
	memset(params->device_eqtr_record, 0xFF, sizeof(params->device_eqtr_record));

	memset(h_iter, 0, sizeof(struct tx_eqtr_iter));
	memset(d_iter, 0, sizeof(struct tx_eqtr_iter));

	h_iter->num_lanes = params->tx_lanes;
	d_iter->num_lanes = params->rx_lanes;

	/*
	 * Support PreShoot & DeEmphasis of value 0 is mandatory, hence they are
	 * not reflected in PreShoot/DeEmphasis capabilities. Left shift the
	 * capability bitmap by 1 and set bit[0] to reflect value 0 is
	 * supported, such that test_bit() can be used later for convenience.
	 */
	h_iter->preshoot_bitmap = (hba->host_preshoot_cap << 0x1) | 0x1;
	h_iter->deemphasis_bitmap = (hba->host_deemphasis_cap << 0x1) | 0x1;
	d_iter->preshoot_bitmap = (hba->device_preshoot_cap << 0x1) | 0x1;
	d_iter->deemphasis_bitmap = (hba->device_deemphasis_cap << 0x1) | 0x1;

	return ret;
}

/**
 * adapt_cap_to_t_adapt - Calculate TAdapt from adapt capability
 * @adapt_cap: Adapt capability
 *
 * For NRZ:
 *   IF (ADAPT_range = FINE)
 *     TADAPT = 650 x (ADAPT_length + 1)
 *   ELSE (IF ADAPT_range = COARSE)
 *     TADAPT = 650 x 2^ADAPT_length
 *
 * Returns calculated TAdapt value in term of Unit Intervals (UI)
 */
static inline u64 adapt_cap_to_t_adapt(u32 adapt_cap)
{
	u64 tadapt;
	u8 adapt_range = adapt_cap & ADAPT_RANGE_MASK;
	u8 adapt_length = adapt_cap & ADAPT_LENGTH_MASK;

	if (!adapt_range)
		tadapt = TADAPT_FACTOR * (adapt_length + 1);
	else
		tadapt = TADAPT_FACTOR * (1 << adapt_length);

	return tadapt;
}

/**
 * adapt_cap_to_t_adapt_l0l3 - Calculate TAdapt_L0_L3 from adapt capability
 * @adapt_cap: Adapt capability
 *
 * For PAM-4:
 *   IF (ADAPT_range = FINE)
 *     TADAPT_L0_L3 = 2^9 x ADAPT_length
 *   ELSE IF (ADAPT_range = COARSE)
 *     TADAPT_L0_L3 = 2^9 x (2^ADAPT_length)
 *
 * Returns calculated TAdapt value in term of Unit Intervals (UI)
 */
static inline u64 adapt_cap_to_t_adapt_l0l3(u32 adapt_cap)
{
	u64 tadapt;
	u8 adapt_range = adapt_cap & ADAPT_RANGE_MASK;
	u8 adapt_length = adapt_cap & ADAPT_LENGTH_MASK;

	if (!adapt_range)
		tadapt = TADAPT_L0L3_FACTOR * adapt_length;
	else
		tadapt = TADAPT_L0L3_FACTOR * (1 << adapt_length);

	return tadapt;
}

/**
 * adapt_cap_to_t_adapt_l0l1l2l3 - Calculate TAdapt_L0_L1_L2_L3 from adapt capability
 * @adapt_cap: Adapt capability
 *
 * For PAM-4:
 *   IF (ADAPT_range_L0_L1_L2_L3 = FINE)
 *     TADAPT_L0_L1_L2_L3 = 2^15 x (ADAPT_length_L0_L1_L2_L3 + 1)
 *   ELSE IF (ADAPT_range_L0_L1_L2_L3 = COARSE)
 *     TADAPT_L0_L1_L2_L3 = 2^15 x 2^ADAPT_length_L0_L1_L2_L3
 *
 * Returns calculated TAdapt value in term of Unit Intervals (UI)
 */
static inline u64 adapt_cap_to_t_adapt_l0l1l2l3(u32 adapt_cap)
{
	u64 tadapt;
	u8 adapt_range = adapt_cap & ADAPT_RANGE_MASK;
	u8 adapt_length = adapt_cap & ADAPT_LENGTH_MASK;

	if (!adapt_range)
		tadapt = TADAPT_L0L1L2L3_FACTOR * (adapt_length + 1);
	else
		tadapt = TADAPT_L0L1L2L3_FACTOR * (1 << adapt_length);

	return tadapt;
}

/**
 * ufshcd_setup_tx_eqtr_adapt_length - Setup TX adapt length for EQTR
 * @hba: per adapter instance
 * @gear: target gear for EQTR
 *
 * This function determines and configures the proper TX adapt length (TAdapt)
 * for the TX EQTR procedure based on the target gear and RX adapt capabilities
 * of both host and device.
 *
 * Guidelines from MIPI UniPro v3.0 spec - select the minimum Adapt Length for
 * the Equalization Training procedure based on the following conditions:
 *
 * If the target High-Speed Gear n is HS-G4 or HS-G5:
 *  PA_TxAdaptLength_EQTR[7:0] >= Max (10us, RX_HS_Gn_ADAPT_INITIAL_Capability,
 *					PA_PeerRxHsGnAdaptInitial)
 *  PA_TxAdaptLength_EQTR[7:0] shall be shorter than PACP_REQUEST_TIMER (10ms)
 *  PA_TxAdaptLength_EQTR[15:8] is not relevant for HS-G4 and HS-G5. This field
 *  is set to 255 (reserved value).
 *
 * If the target High-Speed Gear n is HS-G6:
 *  PA_TxAdapthLength_EQTR >= 10us
 *  PA_TxAdapthLength_EQTR[7:0] >= Max (RX_HS_G6_ADAPT_INITIAL_Capability,
 *					PA_PeerRxHsG6AdaptInitialL0L3)
 *  PA_TxAdapthLength_EQTR[15:8] >= Max (RX_HS_G6_ADAPT_INITIAL_L0_L1_L2_L3_Capability,
 *					PA_PeerRxHsG6AdaptInitialL0L1L2L3)
 * PA_TxAdaptLength_EQTR shall be shorter than PACP_REQUEST_TIMER value of 10ms.
 *
 * Since adapt capabilities encode both range (fine/coarse) and length values,
 * direct comparison is not possible. This function converts adapt capabilities
 * to actual time durations in Unit Intervals (UI) using the Adapt time
 * calculation formular in M-PHY v6.0 spec (Table 8), then selects the maximum
 * to ensure both host and device use adequate TX adapt length.
 *
 * Returns 0 on success, negative error code otherwise
 */
static int ufshcd_setup_tx_eqtr_adapt_length(struct ufs_hba *hba, u32 gear)
{
	struct ufshcd_tx_eq_params *params = &hba->tx_eq_params[gear - 1];
	u32 adapt_eqtr;
	int ret;

	if (params->saved_adapt_eqtr)
		goto set_adapt_eqtr;

	if (gear == UFS_HS_G4 || gear == UFS_HS_G5) {
		u64 t_adapt, t_adapt_local, t_adapt_peer;
		u32 adapt_cap_local, adapt_cap_peer, adapt_length;

		ret = ufshcd_dme_get(hba, UIC_ARG_MIB_SEL(rx_adapt_initial_cap[gear],
				     UIC_ARG_MPHY_RX_GEN_SEL_INDEX(0)),
				     &adapt_cap_local);
		if (ret)
			return ret;

		if (adapt_cap_local > ADAPT_LENGTH_MAX) {
			dev_err(hba->dev, "local RX_HS_G%u_ADAPT_INITIAL_CAP (0x%x) exceeds max\n",
				gear, adapt_cap_local);
			return -EINVAL;
		}

		ret = ufshcd_dme_get(hba, UIC_ARG_MIB(pa_peer_rx_adapt_initial[gear]),
				     &adapt_cap_peer);
		if (ret)
			return ret;

		if (adapt_cap_peer > ADAPT_LENGTH_MAX) {
			dev_err(hba->dev, "local RX_HS_G%u_ADAPT_INITIAL_CAP (0x%x) exceeds max\n",
				gear, adapt_cap_peer);
			return -EINVAL;
		}

		t_adapt_local = adapt_cap_to_t_adapt(adapt_cap_local);
		t_adapt_peer = adapt_cap_to_t_adapt(adapt_cap_peer);

		dev_dbg(hba->dev, "local RX_HS_G%u_ADAPT_INITIAL_CAP = 0x%x\n",
			gear, adapt_cap_local);
		dev_dbg(hba->dev, "peer RX_HS_G%u_ADAPT_INITIAL_CAP = 0x%x\n",
			gear, adapt_cap_peer);
		dev_dbg(hba->dev, "t_adapt_local = %llu UI, t_adapt_peer = %llu UI\n",
			t_adapt_local, t_adapt_peer);

		t_adapt = max(t_adapt_local, t_adapt_peer);
		adapt_length = (t_adapt == t_adapt_local) ?
			       adapt_cap_local : adapt_cap_peer;

		dev_dbg(hba->dev, "TAdapt %llu UI selected for TX EQTR\n",
			t_adapt);

		if (gear == UFS_HS_G4 && t_adapt < TX_EQTR_HS_G4_MIN_T_ADAPT) {
			dev_dbg(hba->dev, "TAdapt %llu UI is too short for TX EQTR for HS-G%u, use default Adapt 0x%x\n",
				t_adapt, gear, TX_EQTR_HS_G4_ADAPT_DEFAULT);
			adapt_length = TX_EQTR_HS_G4_ADAPT_DEFAULT;
		} else if (gear == UFS_HS_G5 && t_adapt < TX_EQTR_HS_G5_MIN_T_ADAPT) {
			dev_dbg(hba->dev, "TAdapt %llu UI is too short for TX EQTR for HS-G%u, use default Adapt 0x%x\n",
				t_adapt, gear, TX_EQTR_HS_G5_ADAPT_DEFAULT);
			adapt_length = TX_EQTR_HS_G5_ADAPT_DEFAULT;
		}

		adapt_eqtr = (adapt_length << TX_EQTR_ADAPT_LENGTH_SHIFT) |
			     (TX_EQTR_ADAPT_RESERVED << TX_EQTR_ADAPT_LENGTH_L0L1L2L3_SHIFT);
	} else if (gear == UFS_HS_G6) {
		u64 t_adapt, t_adapt_l0l3, t_adapt_l0l3_local, t_adapt_l0l3_peer;
		u64 t_adapt_l0l1l2l3, t_adapt_l0l1l2l3_local, t_adapt_l0l1l2l3_peer;
		u32 adapt_l0l3_cap_local, adapt_l0l3_cap_peer, adapt_length_l0l3;
		u32 adapt_l0l1l2l3_cap_local, adapt_l0l1l2l3_cap_peer, adapt_length_l0l1l2l3;

		ret = ufshcd_dme_get(hba, UIC_ARG_MIB_SEL(rx_adapt_initial_cap[gear],
				     UIC_ARG_MPHY_RX_GEN_SEL_INDEX(0)),
				     &adapt_l0l3_cap_local);
		if (ret)
			return ret;

		if (adapt_l0l3_cap_local > ADAPT_L0L3_LENGTH_MAX) {
			dev_err(hba->dev, "local RX_HS_G%u_ADAPT_INITIAL_CAP (0x%x) exceeds max\n",
				gear, adapt_l0l3_cap_local);
			return -EINVAL;
		}

		ret = ufshcd_dme_get(hba, UIC_ARG_MIB(pa_peer_rx_adapt_initial[gear]),
				     &adapt_l0l3_cap_peer);
		if (ret)
			return ret;

		if (adapt_l0l3_cap_peer > ADAPT_L0L3_LENGTH_MAX) {
			dev_err(hba->dev, "peer RX_HS_G%u_ADAPT_INITIAL_CAP (0x%x) exceeds max\n",
				gear, adapt_l0l3_cap_peer);
			return -EINVAL;
		}

		t_adapt_l0l3_local = adapt_cap_to_t_adapt_l0l3(adapt_l0l3_cap_local);
		t_adapt_l0l3_peer = adapt_cap_to_t_adapt_l0l3(adapt_l0l3_cap_peer);

		dev_dbg(hba->dev, "local RX_HS_G%u_ADAPT_INITIAL_CAP = 0x%x\n",
			gear, adapt_l0l3_cap_local);
		dev_dbg(hba->dev, "peer RX_HS_G%u_ADAPT_INITIAL_CAP = 0x%x\n",
			gear, adapt_l0l3_cap_peer);
		dev_dbg(hba->dev, "t_adapt_l0l3_local = %llu UI, t_adapt_l0l3_peer = %llu UI\n",
			t_adapt_l0l3_local, t_adapt_l0l3_peer);

		ret = ufshcd_dme_get(hba, UIC_ARG_MIB_SEL(RX_HS_G6_ADAPT_INITIAL_L0L1L2L3_CAP,
				     UIC_ARG_MPHY_RX_GEN_SEL_INDEX(0)),
				     &adapt_l0l1l2l3_cap_local);
		if (ret)
			return ret;

		if (adapt_l0l1l2l3_cap_local > ADAPT_L0L1L2L3_LENGTH_MAX) {
			dev_err(hba->dev, "local RX_HS_G%u_ADAPT_INITIAL_L0L1L2L3_CAP (0x%x) exceeds max\n",
				gear, adapt_l0l1l2l3_cap_local);
			return -EINVAL;
		}

		ret = ufshcd_dme_get(hba, UIC_ARG_MIB(PA_PEERRXHSG6ADAPTINITIALL0L1L2L3),
				     &adapt_l0l1l2l3_cap_peer);
		if (ret)
			return ret;

		if (adapt_l0l1l2l3_cap_peer > ADAPT_L0L1L2L3_LENGTH_MAX) {
			dev_err(hba->dev, "peer RX_HS_G%u_ADAPT_INITIAL_L0L1L2L3_CAP (0x%x) exceeds max\n",
				gear, adapt_l0l1l2l3_cap_peer);
			return -EINVAL;
		}

		t_adapt_l0l1l2l3_local = adapt_cap_to_t_adapt_l0l1l2l3(adapt_l0l1l2l3_cap_local);
		t_adapt_l0l1l2l3_peer = adapt_cap_to_t_adapt_l0l1l2l3(adapt_l0l1l2l3_cap_peer);

		dev_dbg(hba->dev, "local RX_HS_G%u_ADAPT_INITIAL_L0L1L2L3_CAP = 0x%x\n",
			gear, adapt_l0l1l2l3_cap_local);
		dev_dbg(hba->dev, "peer RX_HS_G%u_ADAPT_INITIAL_L0L1L2L3_CAP = 0x%x\n",
			gear, adapt_l0l1l2l3_cap_peer);
		dev_dbg(hba->dev, "t_adapt_l0l1l2l3_local = %llu UI, t_adapt_l0l1l2l3_peer = %llu UI\n",
			t_adapt_l0l1l2l3_local, t_adapt_l0l1l2l3_peer);

		t_adapt_l0l3 = max(t_adapt_l0l3_local, t_adapt_l0l3_peer);
		adapt_length_l0l3 = (t_adapt_l0l3 == t_adapt_l0l3_local) ?
				    adapt_l0l3_cap_local : adapt_l0l3_cap_peer;

		t_adapt_l0l1l2l3 = max(t_adapt_l0l1l2l3_local, t_adapt_l0l1l2l3_peer);
		adapt_length_l0l1l2l3 = (t_adapt_l0l1l2l3 == t_adapt_l0l1l2l3_local) ?
					adapt_l0l1l2l3_cap_local : adapt_l0l1l2l3_cap_peer;

		t_adapt = t_adapt_l0l3 + t_adapt_l0l1l2l3;

		dev_dbg(hba->dev, "TAdapt %llu PAM-4 UI selected for TX EQTR\n",
			t_adapt);

		if (t_adapt < TX_EQTR_HS_G6_MIN_T_ADAPT) {
			dev_dbg(hba->dev, "TAdapt %llu UI is too short for TX EQTR for HS-G%u, use default Adapt 0x%x\n",
				t_adapt, gear, TX_EQTR_HS_G6_ADAPT_DEFAULT);
			adapt_length_l0l3 = TX_EQTR_HS_G6_ADAPT_DEFAULT;
		}

		adapt_eqtr = (adapt_length_l0l3 << TX_EQTR_ADAPT_LENGTH_SHIFT) |
			     (adapt_length_l0l1l2l3 << TX_EQTR_ADAPT_LENGTH_L0L1L2L3_SHIFT);
	} else {
		return -EINVAL;
	}

	params->saved_adapt_eqtr = adapt_eqtr;

set_adapt_eqtr:
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB(PA_TXADAPTLENGTH_EQTR),
			     params->saved_adapt_eqtr);
	if (ret)
		dev_err(hba->dev, "Failed to set adapt length for TX EQTR: %d\n",
			ret);
	else
		dev_dbg(hba->dev, "PA_TXADAPTLENGTH_EQTR configured to 0x%08x\n",
			params->saved_adapt_eqtr);

	return ret;
}

/**
 * ufshcd_compose_tx_eqtr_setting - Compose TX EQTR setting
 * @iter: TX EQTR iterator data structure
 *
 * Returns composed TX EQTR setting, same setting is used for all active lanes
 */
static inline u32 ufshcd_compose_tx_eqtr_setting(struct tx_eqtr_iter *iter)
{
	u32 setting = 0;
	int lane;

	for (lane = 0; lane < iter->num_lanes; lane++) {
		setting |= TX_HS_PRESHOOT_BITS(lane, iter->preshoot);
		setting |= TX_HS_DEEMPHASIS_BITS(lane, iter->deemphasis);
	}

	return setting;
}

/**
 * ufshcd_apply_tx_eqtr_settings - Apply TX EQTR setting
 * @hba: per adapter instance
 * @pwr_mode: target power mode containing gear and rate information
 * @h_iter: host TX EQTR iterator data structure
 * @d_iter: device TX EQTR iterator data structure
 *
 * Returns 0 on success, negative error code otherwise
 */
static int ufshcd_apply_tx_eqtr_settings(struct ufs_hba *hba,
					 struct ufs_pa_layer_attr *pwr_mode,
					 struct tx_eqtr_iter *h_iter,
					 struct tx_eqtr_iter *d_iter)
{
	u32 setting;
	int ret;

	setting = ufshcd_compose_tx_eqtr_setting(h_iter);
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB(PA_TXEQTRSETTING), setting);
	if (ret)
		return ret;

	setting = ufshcd_compose_tx_eqtr_setting(d_iter);
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB(PA_PEERTXEQTRSETTING), setting);
	if (ret)
		return ret;

	ret = ufshcd_vops_apply_tx_eqtr_settings(hba, pwr_mode, h_iter, d_iter);

	return ret;
}

/**
 * ufshcd_tx_eqtr_result_examine() - Examine TX Equalization training results
 * @old_params: Pointer to the previously known TX Equalization parameters
 * @new_params: Pointer to the newly trained TX Equalization parameters
 *
 * Checks the Figure of Merit (FOM) for each lane in the newly trained TX
 * Equalization parameters. If a lane in the newly trained TX Equalization
 * parameters has an FOM of 0, it restores the TX Equalization settings from
 * the old_params for that specific lane.
 */
static inline void
ufshcd_tx_eqtr_result_examine(struct ufshcd_tx_eq_params *old_params,
			      struct ufshcd_tx_eq_params *new_params)
{
	int lane;

	if (!old_params->is_valid)
		return;

	for (lane = 0; lane < new_params->tx_lanes; lane++)
		if (new_params->host[lane].fom_val == 0)
			new_params->host[lane] = old_params->host[lane];

	for (lane = 0; lane < new_params->rx_lanes; lane++)
		if (new_params->device[lane].fom_val == 0)
			new_params->device[lane] = old_params->device[lane];
}

/**
 * __ufshcd_tx_eqtr - TX Equalization Training (EQTR) procedure
 * @hba: per adapter instance
 * @params: TX EQ parameters data structure
 * @pwr_mode: target power mode containing gear and rate information
 *
 * This function implements the complete TX EQTR procedure as defined in UFSHCI
 * v5.0 specification. It iterates through all possible combinations of PreShoot
 * and DeEmphasis settings to find the optimal TX Equalization settings for all
 * active lanes.
 *
 * Returns 0 on success, negative error code otherwise
 */
static int __ufshcd_tx_eqtr(struct ufs_hba *hba,
			    struct ufshcd_tx_eq_params *params,
			    struct ufs_pa_layer_attr *pwr_mode)
{
	struct ufshcd_tx_eq_params *new_params __free(kfree) =
		kzalloc(sizeof(*new_params), GFP_KERNEL);
	struct tx_eqtr_iter h_iter, d_iter;
	unsigned int preshoot, deemphasis;
	u32 gear = pwr_mode->gear_tx;
	ktime_t start;
	int ret;

	if (!new_params)
		return -ENOMEM;

	dev_info(hba->dev, "Starting TX EQTR procedure for HS-G%u, Rate-%s, active RX lanes: %u, active TX Lanes: %u\n",
		 gear, UFS_HS_RATE_STRING(pwr_mode->hs_rate),
		 params->rx_lanes, params->tx_lanes);

	start = ktime_get();

	/* Step 1 - Determine the TX Adapt Length for EQTR */
	ret = ufshcd_setup_tx_eqtr_adapt_length(hba, gear);
	if (ret) {
		dev_err(hba->dev, "Failed to setup adapt length: %d\n", ret);
		return ret;
	}

	/* Step 2 - Determine TX Equalization setting capabilities */
	memcpy(new_params, params, sizeof(struct ufshcd_tx_eq_params));
	ret = ufshcd_tx_eqtr_data_init(hba, new_params, &h_iter, &d_iter);
	if (ret) {
		dev_err(hba->dev, "Failed to init TX EQTR data: %d\n", ret);
		return ret;
	}

	/* TX EQTR main loop */
	for (preshoot = 0; preshoot < TX_HS_NUM_PRESHOOT; preshoot++) {
		for (deemphasis = 0; deemphasis < TX_HS_NUM_DEEMPHASIS; deemphasis++) {
			if (!tx_eqtr_iter_update(preshoot, deemphasis, &h_iter, &d_iter))
				continue;

			/* Step 3 - Apply TX EQTR settings */
			ret = ufshcd_apply_tx_eqtr_settings(hba, pwr_mode, &h_iter, &d_iter);
			if (ret) {
				dev_err(hba->dev, "Failed to apply TX EQTR settings: %d\n",
					ret);
				return ret;
			}

			/* Step 4 - Perform UIC TX EQTR */
			ret = ufshcd_uic_tx_eqtr(hba, gear);
			if (ret) {
				dev_err(hba->dev, "Failed to start TX EQTR procedure for target gear %u: %d\n",
					gear, ret);
				return ret;
			}

			/* Step 5 - Get FOM */
			ret = ufshcd_get_rx_fom(hba, pwr_mode, &h_iter, &d_iter);
			if (ret) {
				dev_err(hba->dev, "Failed to get RX_FOM: %d\n",
					ret);
				return ret;
			}

			ufshcd_evaluate_fom(hba, new_params, &h_iter, &d_iter);
		}
	}

	dev_info(hba->dev, "TX EQTR procedure completed successfully! Time elapsed: %llu ms\n",
		 ktime_to_ms(ktime_sub(ktime_get(), start)));

	ufshcd_tx_eqtr_result_examine(params, new_params);

	memcpy(params, new_params, sizeof(struct ufshcd_tx_eq_params));
	params->last_eqtr_ts = ktime_get();
	params->num_eqtr_records++;

	return ret;
}

/**
 * ufshcd_tx_eqtr_prepare - Prepare UFS link for TX EQTR procedure
 * @hba: per adapter instance
 * @pwr_mode: target power mode containing gear and rate
 *
 * This function prepares the UFS link for TX Equalization Training (EQTR) by
 * establishing the proper initial conditions required by the EQTR procedure.
 * It ensures that EQTR starts from the most reliable Power Mode (HS-G1) with
 * all connected lanes activated and sets host TX HS Adapt Type to INITIAL.
 *
 * Returns 0 on successful preparation, negative error code on failure
 */
static int ufshcd_tx_eqtr_prepare(struct ufs_hba *hba,
				  struct ufs_pa_layer_attr *pwr_mode)
{
	struct ufs_pa_layer_attr pwr_mode_hs_g1 = {
		/* TX EQTR shall be initiated from the most reliable HS-G1 */
		.gear_rx = UFS_HS_G1,
		.gear_tx = UFS_HS_G1,
		.lane_rx = pwr_mode->lane_rx,
		.lane_tx = pwr_mode->lane_tx,
		.pwr_rx = FAST_MODE,
		.pwr_tx = FAST_MODE,
		/* Use the target power mode's HS rate */
		.hs_rate = pwr_mode->hs_rate,
	};
	u32 rate = pwr_mode->hs_rate;
	int ret;

	/* Change power mode to HS-G1, activate all connected lanes. */
	ret = ufshcd_change_power_mode(hba, &pwr_mode_hs_g1,
				       UFSHCD_PMC_POLICY_DONT_FORCE);
	if (ret) {
		dev_err(hba->dev, "TX EQTR: Failed to change power mode to HS-G1, Rate-%s: %d\n",
			UFS_HS_RATE_STRING(rate), ret);
		return ret;
	}

	ret = ufshcd_dme_set(hba, UIC_ARG_MIB(PA_TXHSADAPTTYPE),
			     PA_INITIAL_ADAPT);
	if (ret)
		dev_err(hba->dev, "TX EQTR: Failed to set Host Adapt type to INITIAL: %d\n",
			ret);

	return ret;
}

static void ufshcd_tx_eqtr_unprepare(struct ufs_hba *hba,
				     struct ufs_pa_layer_attr *pwr_mode)
{
	int err;

	if (pwr_mode->pwr_rx == SLOWAUTO_MODE || pwr_mode->hs_rate == 0)
		return;

	err = ufshcd_change_power_mode(hba, pwr_mode,
				       UFSHCD_PMC_POLICY_DONT_FORCE);
	if (err)
		dev_err(hba->dev, "%s: Failed to restore Power Mode: %d\n",
			__func__, err);
}

/**
 * ufshcd_tx_eqtr - Perform TX EQTR procedures with vops callbacks
 * @hba: per adapter instance
 * @params: TX EQ parameters data structure to populate
 * @pwr_mode: target power mode containing gear and rate information
 *
 * This is the main entry point for performing TX Equalization Training (EQTR)
 * procedure as defined in UFSCHI v5.0 specification. It serves as a wrapper
 * around __ufshcd_tx_eqtr() to provide vops support through the variant
 * operations framework.
 *
 * Returns 0 on success, negative error code on failure
 */
static int ufshcd_tx_eqtr(struct ufs_hba *hba,
			  struct ufshcd_tx_eq_params *params,
			  struct ufs_pa_layer_attr *pwr_mode)
{
	struct ufs_pa_layer_attr old_pwr_info;
	u32 gear = pwr_mode->gear_tx;
	int ret;

	if (gear < UFS_HS_G4 || gear > UFS_HS_G6) {
		dev_err(hba->dev, "TX EQTR is not implemented for HS-G%u\n",
			gear);
		return -EINVAL;
	}

	params->rx_lanes = pwr_mode->lane_rx;
	params->tx_lanes = pwr_mode->lane_tx;

	memcpy(&old_pwr_info, &hba->pwr_info, sizeof(struct ufs_pa_layer_attr));

	ret = ufshcd_tx_eqtr_prepare(hba, pwr_mode);
	if (ret) {
		dev_err(hba->dev, "Failed to prepare TX EQTR: %d\n", ret);
		goto out;
	}

	ret = ufshcd_vops_tx_eqtr_notify(hba, PRE_CHANGE, pwr_mode);
	if (ret)
		goto out;

	ret = __ufshcd_tx_eqtr(hba, params, pwr_mode);
	if (ret)
		goto out;

	ret = ufshcd_vops_tx_eqtr_notify(hba, POST_CHANGE, pwr_mode);
	if (ret)
		goto out;

out:
	if (ret)
		ufshcd_tx_eqtr_unprepare(hba, &old_pwr_info);

	return ret;
}

/**
 * ufshcd_config_tx_eq_settings - Configure TX Equalization settings
 * @hba: per adapter instance
 * @pwr_mode: target power mode containing gear and rate information
 *
 * This function finds and sets the TX Equalization settings for the given
 * target power mode.
 *
 * Returns 0 on success, error code otherwise
 */
int ufshcd_config_tx_eq_settings(struct ufs_hba *hba,
				 struct ufs_pa_layer_attr *pwr_mode)
{
	struct ufshcd_tx_eq_params *params;
	u32 gear, rate;

	if (!ufshcd_is_tx_eq_supported(hba) || !use_adaptive_txeq)
		return 0;

	if (!pwr_mode) {
		dev_err(hba->dev, "%s: Target power mode is NULL\n", __func__);
		return -EINVAL;
	}

	gear = pwr_mode->gear_tx;
	rate = pwr_mode->hs_rate;
	params = &hba->tx_eq_params[gear - 1];

	if (gear < UFS_HS_G1 || gear >= UFS_HS_GEAR_MAX) {
		dev_err(hba->dev, "Invalid HS-Gear (%u) for TX Equalization\n",
			gear);
		return -EINVAL;
	} else if (gear < adaptive_txeq_gear) {
		return 0;
	}

	if (rate != PA_HS_MODE_A && rate != PA_HS_MODE_B) {
		dev_err(hba->dev, "Invalid HS-Rate (%u) for TX Equalization\n",
			rate);
		return -EINVAL;
	}

	/* TX EQTR is supported for HS-G4 and higher Gears */
	if (gear < UFS_HS_G4)
		goto apply_tx_eq_settings;

	if (!params->is_valid) {
		int ret;

		ret = ufshcd_tx_eqtr(hba, params, pwr_mode);
		if (ret) {
			dev_err(hba->dev, "Failed to train TX Equalization for HS-G%u, Rate-%s: %d\n",
				gear, UFS_HS_RATE_STRING(rate), ret);
			return ret;
		}

		/* Mark TX Equalization settings as valid */
		params->is_valid = true;
		params->is_applied = false;
	}

apply_tx_eq_settings:
	if (params->is_valid && !params->is_applied) {
		int ret;

		ret = ufshcd_apply_tx_eq_settings(hba, params, gear);
		if (ret) {
			dev_err(hba->dev, "Failed to apply TX Equalization settings for HS-G%u, Rate-%s: %d\n",
				gear, UFS_HS_RATE_STRING(rate), ret);
			return ret;
		}

		params->is_applied = true;
	}

	return 0;
}

void ufshcd_apply_valid_tx_eq_settings(struct ufs_hba *hba)
{
	struct ufs_pa_layer_attr *pwr_info = &hba->max_pwr_info.info;
	struct ufshcd_tx_eq_params *params;
	int gear, ret;

	if (!ufshcd_is_tx_eq_supported(hba))
		return;

	if (!hba->max_pwr_info.is_valid) {
		dev_err(hba->dev, "Max power info invalid, cannot apply TX Equalization settings\n");
		return;
	}

	for (gear = UFS_HS_G1; gear < UFS_HS_GEAR_MAX; gear++) {
		params = &hba->tx_eq_params[gear - 1];

		if (params->is_valid) {
			params->tx_lanes = pwr_info->lane_tx;
			params->rx_lanes = pwr_info->lane_rx;

			ret = ufshcd_apply_tx_eq_settings(hba, params, gear);
			if (ret) {
				params->is_applied = false;
				dev_err(hba->dev, "Failed to apply TX Equalization settings for HS-G%u: %d\n",
					gear, ret);
			} else {
				params->is_applied = true;
			}
		}
	}
}
