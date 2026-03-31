/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2020 Intel Corporation
 */

#ifndef _INTEL_DDI_BUF_TRANS_H_
#define _INTEL_DDI_BUF_TRANS_H_

#include <linux/types.h>

struct intel_encoder;
struct intel_crtc_state;

struct hsw_ddi_buf_trans {
	u32 trans1;	/* balance leg enable, de-emph level */
	u32 trans2;	/* vref sel, vswing */
	u8 i_boost;	/* SKL: I_boost; valid: 0x0, 0x1, 0x3, 0x7 */
};

struct bxt_ddi_buf_trans {
	u8 margin;	/* swing value */
	u8 scale;	/* scale value */
	u8 enable;	/* scale enable */
	u8 deemphasis;
};

struct icl_ddi_buf_trans {
	u8 dw2_swing_sel;
	u8 dw7_n_scalar;
	u8 dw4_cursor_coeff;
	u8 dw4_post_cursor_2;
	u8 dw4_post_cursor_1;
};

struct icl_mg_phy_ddi_buf_trans {
	u8 cri_txdeemph_override_11_6;
	u8 cri_txdeemph_override_5_0;
	u8 cri_txdeemph_override_17_12;
};

struct tgl_dkl_phy_ddi_buf_trans {
	u8 vswing;
	u8 preshoot;
	u8 de_emphasis;
};

struct dg2_snps_phy_buf_trans {
	u32 vswing;
	u32 pre_cursor;
	u32 post_cursor;
};

struct xe3plpd_lt_phy_buf_trans {
	u32 main_cursor;
	u32 pre_cursor;
	u32 post_cursor;
	u32 txswing;
	u32 txswing_level;
};

union intel_ddi_buf_trans_entry {
	struct hsw_ddi_buf_trans hsw;
	struct bxt_ddi_buf_trans bxt;
	struct icl_ddi_buf_trans icl;
	struct icl_mg_phy_ddi_buf_trans mg;
	struct tgl_dkl_phy_ddi_buf_trans dkl;
	struct dg2_snps_phy_buf_trans snps;
	struct xe3plpd_lt_phy_buf_trans lt;
};

struct intel_ddi_buf_trans {
	const union intel_ddi_buf_trans_entry *entries;
	u8 num_entries;
	u8 hdmi_default_entry;
};

enum lt_vswing_preemph_index {
        XE3P_VS_PE_UNSET = -1,
	XE3P_VS_PE_DEFAULT = 0,
        XE3P_VS_PE_EDP = 3,
        XE3P_VS_PE_DP14 = 4,
        XE3P_VS_PE_DP21 = 5
};

enum snps_vswing_preemph_index {
        MTL_C10_VS_PE_UNSET = -1,
        MTL_C10_VS_PE_DP14_RBR_HBR = 0,
        MTL_C10_VS_PE_DP14_HBR2_HBR3 = 1,
        MTL_C10_VS_PE_EDP_NON_HBR3 = 2,
        MTL_C10_VS_PE_EDP_HBR3 = 3,

        MTL_C20_VS_PE_DP14 = 4,
        MTL_C20_VS_PE_DP20 = 5
};

enum icl_vswing_preemph_index {
        ICL_VS_PE_UNSET = -1,
        ICL_VS_PE_DEFAULT = 0
};

union ddi_vswing_preemph_index {
        enum lt_vswing_preemph_index lt;
        enum snps_vswing_preemph_index snps;
        enum icl_vswing_preemph_index icl;
};

struct ddi_vswing_preemph {
        struct intel_ddi_buf_trans *buf_trans;
        union ddi_vswing_preemph_index index;
};

bool is_hobl_buf_trans(const struct intel_ddi_buf_trans *table);

void intel_ddi_buf_trans_init(struct intel_encoder *encoder);

#endif
