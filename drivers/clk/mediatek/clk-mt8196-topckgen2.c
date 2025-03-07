// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 MediaTek Inc.
 * Author: Guangjie Song <guangjie.song@mediatek.com>
 */
#include <dt-bindings/clock/mt8196-clk.h>
#include <linux/clk.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>

#include "clk-mtk.h"
#include "clk-mux.h"

/* MUX SEL REG */
#define CKSYS2_CLK_CFG_UPDATE		0x0004
#define CKSYS2_CLK_CFG_0		0x0010
#define CKSYS2_CLK_CFG_0_SET		0x0014
#define CKSYS2_CLK_CFG_0_CLR		0x0018
#define CKSYS2_CLK_CFG_1		0x0020
#define CKSYS2_CLK_CFG_1_SET		0x0024
#define CKSYS2_CLK_CFG_1_CLR		0x0028
#define CKSYS2_CLK_CFG_2		0x0030
#define CKSYS2_CLK_CFG_2_SET		0x0034
#define CKSYS2_CLK_CFG_2_CLR		0x0038
#define CKSYS2_CLK_CFG_3		0x0040
#define CKSYS2_CLK_CFG_3_SET		0x0044
#define CKSYS2_CLK_CFG_3_CLR		0x0048
#define CKSYS2_CLK_CFG_4		0x0050
#define CKSYS2_CLK_CFG_4_SET		0x0054
#define CKSYS2_CLK_CFG_4_CLR		0x0058
#define CKSYS2_CLK_CFG_5		0x0060
#define CKSYS2_CLK_CFG_5_SET		0x0064
#define CKSYS2_CLK_CFG_5_CLR		0x0068
#define CKSYS2_CLK_CFG_6		0x0070
#define CKSYS2_CLK_CFG_6_SET		0x0074
#define CKSYS2_CLK_CFG_6_CLR		0x0078
#define CKSYS2_CLK_FENC_STATUS_MON_0	0x0174

/* MUX SHIFT */
#define TOP_MUX_SENINF0_SHIFT		0
#define TOP_MUX_SENINF1_SHIFT		1
#define TOP_MUX_SENINF2_SHIFT		2
#define TOP_MUX_SENINF3_SHIFT		3
#define TOP_MUX_SENINF4_SHIFT		4
#define TOP_MUX_SENINF5_SHIFT		5
#define TOP_MUX_IMG1_SHIFT		6
#define TOP_MUX_IPE_SHIFT		7
#define TOP_MUX_CAM_SHIFT		8
#define TOP_MUX_CAMTM_SHIFT		9
#define TOP_MUX_DPE_SHIFT		10
#define TOP_MUX_VDEC_SHIFT		11
#define TOP_MUX_CCUSYS_SHIFT		12
#define TOP_MUX_CCUTM_SHIFT		13
#define TOP_MUX_VENC_SHIFT		14
#define TOP_MUX_DVO_SHIFT		15
#define TOP_MUX_DVO_FAVT_SHIFT		16
#define TOP_MUX_DP1_SHIFT		17
#define TOP_MUX_DP0_SHIFT		18
#define TOP_MUX_DISP_SHIFT		19
#define TOP_MUX_MDP_SHIFT		20
#define TOP_MUX_MMINFRA_SHIFT		21
#define TOP_MUX_MMINFRA_SNOC_SHIFT	22
#define TOP_MUX_MMUP_SHIFT		23
#define TOP_MUX_MMINFRA_AO_SHIFT	26

/* HW Voter REG */
#define VOTE_CG_30_SET		0x0058
#define VOTE_CG_30_CLR		0x005c
#define VOTE_CG_30_DONE		0x2c2c

#define MM_VOTE_CG_30_SET	0x00f0
#define MM_VOTE_CG_30_CLR	0x00f4
#define MM_VOTE_CG_30_DONE	0x2c78
#define MM_VOTE_CG_31_SET	0x00f8
#define MM_VOTE_CG_31_CLR	0x00fc
#define MM_VOTE_CG_31_DONE	0x2c7c
#define MM_VOTE_CG_32_SET	0x0100
#define MM_VOTE_CG_32_CLR	0x0104
#define MM_VOTE_CG_32_DONE	0x2c80
#define MM_VOTE_CG_33_SET	0x0108
#define MM_VOTE_CG_33_CLR	0x010c
#define MM_VOTE_CG_33_DONE	0x2c84
#define MM_VOTE_CG_34_SET	0x0110
#define MM_VOTE_CG_34_CLR	0x0114
#define MM_VOTE_CG_34_DONE	0x2c88
#define MM_VOTE_CG_35_SET	0x0118
#define MM_VOTE_CG_35_CLR	0x011c
#define MM_VOTE_CG_35_DONE	0x2c8c
#define MM_VOTE_CG_36_SET	0x0120
#define MM_VOTE_CG_36_CLR	0x0124
#define MM_VOTE_CG_36_DONE	0x2c90
#define MM_VOTE_MUX_UPDATE_31_0	0x0240

static DEFINE_SPINLOCK(mt8196_clk_ck2_lock);

static const struct mtk_fixed_factor ck2_divs[] = {
	FACTOR(CLK_CK2_MAINPLL2_D2, "ck2_mainpll2_d2", "mainpll2", 1, 2),
	FACTOR(CLK_CK2_MAINPLL2_D3, "ck2_mainpll2_d3", "mainpll2", 1, 3),
	FACTOR(CLK_CK2_MAINPLL2_D4, "ck2_mainpll2_d4", "mainpll2", 1, 4),
	FACTOR(CLK_CK2_MAINPLL2_D4_D2, "ck2_mainpll2_d4_d2", "mainpll2", 1, 8),
	FACTOR(CLK_CK2_MAINPLL2_D4_D4, "ck2_mainpll2_d4_d4", "mainpll2", 1, 16),
	FACTOR(CLK_CK2_MAINPLL2_D5, "ck2_mainpll2_d5", "mainpll2", 1, 5),
	FACTOR(CLK_CK2_MAINPLL2_D5_D2, "ck2_mainpll2_d5_d2", "mainpll2", 1, 10),
	FACTOR(CLK_CK2_MAINPLL2_D6, "ck2_mainpll2_d6", "mainpll2", 1, 6),
	FACTOR(CLK_CK2_MAINPLL2_D6_D2, "ck2_mainpll2_d6_d2", "mainpll2", 1, 12),
	FACTOR(CLK_CK2_MAINPLL2_D7, "ck2_mainpll2_d7", "mainpll2", 1, 7),
	FACTOR(CLK_CK2_MAINPLL2_D7_D2, "ck2_mainpll2_d7_d2", "mainpll2", 1, 14),
	FACTOR(CLK_CK2_MAINPLL2_D9, "ck2_mainpll2_d9", "mainpll2", 1, 9),
	FACTOR(CLK_CK2_UNIVPLL2_D3, "ck2_univpll2_d3", "univpll2", 1, 3),
	FACTOR(CLK_CK2_UNIVPLL2_D4, "ck2_univpll2_d4", "univpll2", 1, 4),
	FACTOR(CLK_CK2_UNIVPLL2_D4_D2, "ck2_univpll2_d4_d2", "univpll2", 1, 8),
	FACTOR(CLK_CK2_UNIVPLL2_D5, "ck2_univpll2_d5", "univpll2", 1, 5),
	FACTOR(CLK_CK2_UNIVPLL2_D5_D2, "ck2_univpll2_d5_d2", "univpll2", 1, 10),
	FACTOR(CLK_CK2_UNIVPLL2_D6, "ck2_univpll2_d6", "univpll2", 1, 6),
	FACTOR(CLK_CK2_UNIVPLL2_D6_D2, "ck2_univpll2_d6_d2", "univpll2", 1, 12),
	FACTOR(CLK_CK2_UNIVPLL2_D6_D4, "ck2_univpll2_d6_d4", "univpll2", 1, 24),
	FACTOR(CLK_CK2_UNIVPLL2_D7, "ck2_univpll2_d7", "univpll2", 1, 7),
	FACTOR(CLK_CK2_IMGPLL_D2, "ck2_imgpll_d2", "imgpll", 1, 2),
	FACTOR(CLK_CK2_IMGPLL_D4, "ck2_imgpll_d4", "imgpll", 1, 4),
	FACTOR(CLK_CK2_IMGPLL_D5, "ck2_imgpll_d5", "imgpll", 1, 5),
	FACTOR(CLK_CK2_IMGPLL_D5_D2, "ck2_imgpll_d5_d2", "imgpll", 1, 10),
	FACTOR(CLK_CK2_MMPLL2_D3, "ck2_mmpll2_d3", "mmpll2", 1, 3),
	FACTOR(CLK_CK2_MMPLL2_D4, "ck2_mmpll2_d4", "mmpll2", 1, 4),
	FACTOR(CLK_CK2_MMPLL2_D4_D2, "ck2_mmpll2_d4_d2", "mmpll2", 1, 8),
	FACTOR(CLK_CK2_MMPLL2_D5, "ck2_mmpll2_d5", "mmpll2", 1, 5),
	FACTOR(CLK_CK2_MMPLL2_D5_D2, "ck2_mmpll2_d5_d2", "mmpll2", 1, 10),
	FACTOR(CLK_CK2_MMPLL2_D6, "ck2_mmpll2_d6", "mmpll2", 1, 6),
	FACTOR(CLK_CK2_MMPLL2_D6_D2, "ck2_mmpll2_d6_d2", "mmpll2", 1, 12),
	FACTOR(CLK_CK2_MMPLL2_D7, "ck2_mmpll2_d7", "mmpll2", 1, 7),
	FACTOR(CLK_CK2_MMPLL2_D9, "ck2_mmpll2_d9", "mmpll2", 1, 9),
	FACTOR(CLK_CK2_TVDPLL1_D4, "ck2_tvdpll1_d4", "tvdpll1", 1, 4),
	FACTOR(CLK_CK2_TVDPLL1_D8, "ck2_tvdpll1_d8", "tvdpll1", 1, 8),
	FACTOR(CLK_CK2_TVDPLL1_D16, "ck2_tvdpll1_d16", "tvdpll1", 1, 16),
	FACTOR(CLK_CK2_TVDPLL2_D2, "ck2_tvdpll2_d2", "tvdpll2", 1, 2),
	FACTOR(CLK_CK2_TVDPLL2_D4, "ck2_tvdpll2_d4", "tvdpll2", 1, 4),
	FACTOR(CLK_CK2_TVDPLL2_D8, "ck2_tvdpll2_d8", "tvdpll2", 1, 8),
	FACTOR(CLK_CK2_TVDPLL2_D16, "ck2_tvdpll2_d16", "tvdpll2", 92, 1473),
	FACTOR(CLK_CK2_CCUSYS, "ck2_ccusys_ck", "ck2_ccusys_sel", 1, 1),
	FACTOR(CLK_CK2_VENC, "ck2_venc_ck", "ck2_venc_sel", 1, 1),
	FACTOR(CLK_CK2_MMINFRA, "ck2_mminfra_ck", "ck2_mminfra_sel", 1, 1),
	FACTOR(CLK_CK2_IMG1, "ck2_img1_ck", "ck2_img1_sel", 1, 1),
	FACTOR(CLK_CK2_IPE, "ck2_ipe_ck", "ck2_ipe_sel", 1, 1),
	FACTOR(CLK_CK2_CAM, "ck2_cam_ck", "ck2_cam_sel", 1, 1),
	FACTOR(CLK_CK2_CAMTM, "ck2_camtm_ck", "ck2_camtm_sel", 1, 1),
	FACTOR(CLK_CK2_DPE, "ck2_dpe_ck", "ck2_dpe_sel", 1, 1),
	FACTOR(CLK_CK2_VDEC, "ck2_vdec_ck", "ck2_vdec_sel", 1, 1),
	FACTOR(CLK_CK2_DP1, "ck2_dp1_ck", "ck2_dp1_sel", 1, 1),
	FACTOR(CLK_CK2_DP0, "ck2_dp0_ck", "ck2_dp0_sel", 1, 1),
	FACTOR(CLK_CK2_DISP, "ck2_disp_ck", "ck2_disp_sel", 1, 1),
	FACTOR(CLK_CK2_MDP, "ck2_mdp_ck", "ck2_mdp_sel", 1, 1),
	FACTOR(CLK_CK2_AVS_IMG, "ck2_avs_img_ck", "ck_tck_26m_mx9_ck", 1, 1),
	FACTOR(CLK_CK2_AVS_VDEC, "ck2_avs_vdec_ck", "ck_tck_26m_mx9_ck", 1, 1),
	FACTOR(CLK_CK2_TVDPLL3_D2, "ck2_tvdpll3_d2", "tvdpll3", 1, 2),
	FACTOR(CLK_CK2_TVDPLL3_D4, "ck2_tvdpll3_d4", "tvdpll3", 1, 4),
	FACTOR(CLK_CK2_TVDPLL3_D8, "ck2_tvdpll3_d8", "tvdpll3", 1, 8),
	FACTOR(CLK_CK2_TVDPLL3_D16, "ck2_tvdpll3_d16", "tvdpll3", 92, 1473),
};

static const char * const ck2_seninf0_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_osc_d10",
	"ck_osc_d8",
	"ck_osc_d5",
	"ck_osc_d4",
	"ck2_univpll2_d6_d2",
	"ck2_mainpll2_d9",
	"ck_osc_d2",
	"ck2_mainpll2_d4_d2",
	"ck2_univpll2_d4_d2",
	"ck2_mmpll2_d4_d2",
	"ck2_univpll2_d7",
	"ck2_mainpll2_d6",
	"ck2_mmpll2_d7",
	"ck2_univpll2_d6",
	"ck2_univpll2_d5"
};

static const char * const ck2_seninf1_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_osc_d10",
	"ck_osc_d8",
	"ck_osc_d5",
	"ck_osc_d4",
	"ck2_univpll2_d6_d2",
	"ck2_mainpll2_d9",
	"ck_osc_d2",
	"ck2_mainpll2_d4_d2",
	"ck2_univpll2_d4_d2",
	"ck2_mmpll2_d4_d2",
	"ck2_univpll2_d7",
	"ck2_mainpll2_d6",
	"ck2_mmpll2_d7",
	"ck2_univpll2_d6",
	"ck2_univpll2_d5"
};

static const char * const ck2_seninf2_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_osc_d10",
	"ck_osc_d8",
	"ck_osc_d5",
	"ck_osc_d4",
	"ck2_univpll2_d6_d2",
	"ck2_mainpll2_d9",
	"ck_osc_d2",
	"ck2_mainpll2_d4_d2",
	"ck2_univpll2_d4_d2",
	"ck2_mmpll2_d4_d2",
	"ck2_univpll2_d7",
	"ck2_mainpll2_d6",
	"ck2_mmpll2_d7",
	"ck2_univpll2_d6",
	"ck2_univpll2_d5"
};

static const char * const ck2_seninf3_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_osc_d10",
	"ck_osc_d8",
	"ck_osc_d5",
	"ck_osc_d4",
	"ck2_univpll2_d6_d2",
	"ck2_mainpll2_d9",
	"ck_osc_d2",
	"ck2_mainpll2_d4_d2",
	"ck2_univpll2_d4_d2",
	"ck2_mmpll2_d4_d2",
	"ck2_univpll2_d7",
	"ck2_mainpll2_d6",
	"ck2_mmpll2_d7",
	"ck2_univpll2_d6",
	"ck2_univpll2_d5"
};

static const char * const ck2_seninf4_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_osc_d10",
	"ck_osc_d8",
	"ck_osc_d5",
	"ck_osc_d4",
	"ck2_univpll2_d6_d2",
	"ck2_mainpll2_d9",
	"ck_osc_d2",
	"ck2_mainpll2_d4_d2",
	"ck2_univpll2_d4_d2",
	"ck2_mmpll2_d4_d2",
	"ck2_univpll2_d7",
	"ck2_mainpll2_d6",
	"ck2_mmpll2_d7",
	"ck2_univpll2_d6",
	"ck2_univpll2_d5"
};

static const char * const ck2_seninf5_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_osc_d10",
	"ck_osc_d8",
	"ck_osc_d5",
	"ck_osc_d4",
	"ck2_univpll2_d6_d2",
	"ck2_mainpll2_d9",
	"ck_osc_d2",
	"ck2_mainpll2_d4_d2",
	"ck2_univpll2_d4_d2",
	"ck2_mmpll2_d4_d2",
	"ck2_univpll2_d7",
	"ck2_mainpll2_d6",
	"ck2_mmpll2_d7",
	"ck2_univpll2_d6",
	"ck2_univpll2_d5"
};

static const char * const ck2_img1_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_osc_d4",
	"ck_osc_d3",
	"ck2_mmpll2_d6_d2",
	"ck_osc_d2",
	"ck2_imgpll_d5_d2",
	"ck2_mmpll2_d5_d2",
	"ck2_univpll2_d4_d2",
	"ck2_mmpll2_d4_d2",
	"ck2_mmpll2_d7",
	"ck2_univpll2_d6",
	"ck2_mmpll2_d6",
	"ck2_univpll2_d5",
	"ck2_mmpll2_d5",
	"ck2_univpll2_d4",
	"ck2_imgpll_d4"
};

static const char * const ck2_ipe_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_osc_d4",
	"ck_osc_d3",
	"ck_osc_d2",
	"ck2_univpll2_d6",
	"ck2_mmpll2_d6",
	"ck2_univpll2_d5",
	"ck2_imgpll_d5",
	"ck_mainpll_d4",
	"ck2_mmpll2_d5",
	"ck2_imgpll_d4"
};

static const char * const ck2_cam_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_osc_d10",
	"ck_osc_d4",
	"ck_osc_d3",
	"ck_osc_d2",
	"ck2_mmpll2_d5_d2",
	"ck2_univpll2_d4_d2",
	"ck2_univpll2_d7",
	"ck2_mmpll2_d7",
	"ck2_univpll2_d6",
	"ck2_mmpll2_d6",
	"ck2_univpll2_d5",
	"ck2_mmpll2_d5",
	"ck2_univpll2_d4",
	"ck2_imgpll_d4",
	"ck2_mmpll2_d4"
};

static const char * const ck2_camtm_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck2_univpll2_d6_d4",
	"ck_osc_d4",
	"ck_osc_d3",
	"ck2_univpll2_d6_d2"
};

static const char * const ck2_dpe_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck2_mmpll2_d5_d2",
	"ck2_univpll2_d4_d2",
	"ck2_mmpll2_d7",
	"ck2_univpll2_d6",
	"ck2_mmpll2_d6",
	"ck2_univpll2_d5",
	"ck2_mmpll2_d5",
	"ck2_imgpll_d4",
	"ck2_mmpll2_d4"
};

static const char * const ck2_vdec_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_mainpll_d5_d2",
	"ck2_mainpll2_d4_d4",
	"ck2_mainpll2_d7_d2",
	"ck2_mainpll2_d6_d2",
	"ck2_mainpll2_d5_d2",
	"ck2_mainpll2_d9",
	"ck2_mainpll2_d4_d2",
	"ck2_mainpll2_d7",
	"ck2_mainpll2_d6",
	"ck2_univpll2_d6",
	"ck2_mainpll2_d5",
	"ck2_mainpll2_d4",
	"ck2_imgpll_d2"
};

static const char * const ck2_ccusys_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_osc_d4",
	"ck_osc_d3",
	"ck_osc_d2",
	"ck2_mmpll2_d5_d2",
	"ck2_univpll2_d4_d2",
	"ck2_mmpll2_d7",
	"ck2_univpll2_d6",
	"ck2_mmpll2_d6",
	"ck2_univpll2_d5",
	"ck2_mainpll2_d4",
	"ck2_mainpll2_d3",
	"ck2_univpll2_d3"
};

static const char * const ck2_ccutm_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck2_univpll2_d6_d4",
	"ck_osc_d4",
	"ck_osc_d3",
	"ck2_univpll2_d6_d2"
};

static const char * const ck2_venc_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck2_mainpll2_d5_d2",
	"ck2_univpll2_d5_d2",
	"ck2_mainpll2_d4_d2",
	"ck2_mmpll2_d9",
	"ck2_univpll2_d4_d2",
	"ck2_mmpll2_d4_d2",
	"ck2_mainpll2_d6",
	"ck2_univpll2_d6",
	"ck2_mainpll2_d5",
	"ck2_mmpll2_d6",
	"ck2_univpll2_d5",
	"ck2_mainpll2_d4",
	"ck2_univpll2_d4",
	"ck2_univpll2_d3"
};

static const char * const ck2_dp1_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck2_tvdpll2_d16",
	"ck2_tvdpll2_d8",
	"ck2_tvdpll2_d4",
	"ck2_tvdpll2_d2"
};

static const char * const ck2_dp0_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck2_tvdpll1_d16",
	"ck2_tvdpll1_d8",
	"ck2_tvdpll1_d4",
	"ck_tvdpll1_d2"
};

static const char * const ck2_disp_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_mainpll_d5_d2",
	"ck_mainpll_d4_d2",
	"ck_mainpll_d6",
	"ck2_mainpll2_d5",
	"ck2_mmpll2_d6",
	"ck2_mainpll2_d4",
	"ck2_univpll2_d4",
	"ck2_mainpll2_d3"
};

static const char * const ck2_mdp_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_mainpll_d5_d2",
	"ck2_mainpll2_d5_d2",
	"ck2_mmpll2_d6_d2",
	"ck2_mainpll2_d9",
	"ck2_mainpll2_d4_d2",
	"ck2_mainpll2_d7",
	"ck2_mainpll2_d6",
	"ck2_mainpll2_d5",
	"ck2_mmpll2_d6",
	"ck2_mainpll2_d4",
	"ck2_univpll2_d4",
	"ck2_mainpll2_d3"
};

static const char * const ck2_mminfra_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_osc_d4",
	"ck_mainpll_d7_d2",
	"ck_mainpll_d5_d2",
	"ck_mainpll_d9",
	"ck2_mmpll2_d6_d2",
	"ck2_mainpll2_d4_d2",
	"ck_mainpll_d6",
	"ck2_univpll2_d6",
	"ck2_mainpll2_d5",
	"ck2_mmpll2_d6",
	"ck2_univpll2_d5",
	"ck2_mainpll2_d4",
	"ck2_univpll2_d4",
	"ck2_mainpll2_d3",
	"ck2_univpll2_d3"
};

static const char * const ck2_mminfra_snoc_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_osc_d4",
	"ck_mainpll_d7_d2",
	"ck_mainpll_d9",
	"ck_mainpll_d7",
	"ck_mainpll_d6",
	"ck2_mmpll2_d4_d2",
	"ck_mainpll_d5",
	"ck_mainpll_d4",
	"ck2_univpll2_d4",
	"ck2_mmpll2_d4",
	"ck2_mainpll2_d3",
	"ck2_univpll2_d3",
	"ck2_mmpll2_d3",
	"ck2_mainpll2_d2"
};

static const char * const ck2_mmup_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck2_mainpll2_d6",
	"ck2_mainpll2_d5",
	"ck_osc_d2",
	"ck_osc",
	"ck_mainpll_d4",
	"ck2_univpll2_d4",
	"ck2_mainpll2_d3"
};

static const char * const ck2_mminfra_ao_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_osc_d4",
	"ck_mainpll_d3"
};

static const char * const ck2_dvo_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck2_tvdpll3_d16",
	"ck2_tvdpll3_d8",
	"ck2_tvdpll3_d4",
	"ck2_tvdpll3_d2"
};

static const char * const ck2_dvo_favt_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck2_tvdpll3_d16",
	"ck2_tvdpll3_d8",
	"ck2_tvdpll3_d4",
	"ck_apll1_ck",
	"ck_apll2_ck",
	"ck2_tvdpll3_d2"
};

static const struct mtk_mux ck2_muxes[] = {
	/* CKSYS2_CLK_CFG_0 */
	MUX_MULT_VOTE_FENC(CLK_CK2_SENINF0_SEL, "ck2_seninf0_sel", ck2_seninf0_parents,
		CKSYS2_CLK_CFG_0, CKSYS2_CLK_CFG_0_SET, CKSYS2_CLK_CFG_0_CLR, "mm-vote-regmap",
		MM_VOTE_CG_30_DONE, MM_VOTE_CG_30_SET, MM_VOTE_CG_30_CLR,
		0, 4, 7, CKSYS2_CLK_CFG_UPDATE, TOP_MUX_SENINF0_SHIFT,
		CKSYS2_CLK_FENC_STATUS_MON_0, 31),
	MUX_MULT_VOTE_FENC(CLK_CK2_SENINF1_SEL, "ck2_seninf1_sel", ck2_seninf1_parents,
		CKSYS2_CLK_CFG_0, CKSYS2_CLK_CFG_0_SET, CKSYS2_CLK_CFG_0_CLR, "mm-vote-regmap",
		MM_VOTE_CG_30_DONE, MM_VOTE_CG_30_SET, MM_VOTE_CG_30_CLR,
		8, 4, 15, CKSYS2_CLK_CFG_UPDATE, TOP_MUX_SENINF1_SHIFT,
		CKSYS2_CLK_FENC_STATUS_MON_0, 30),
	MUX_MULT_VOTE_FENC(CLK_CK2_SENINF2_SEL, "ck2_seninf2_sel", ck2_seninf2_parents,
		CKSYS2_CLK_CFG_0, CKSYS2_CLK_CFG_0_SET, CKSYS2_CLK_CFG_0_CLR, "mm-vote-regmap",
		MM_VOTE_CG_30_DONE, MM_VOTE_CG_30_SET, MM_VOTE_CG_30_CLR,
		16, 4, 23, CKSYS2_CLK_CFG_UPDATE, TOP_MUX_SENINF2_SHIFT,
		CKSYS2_CLK_FENC_STATUS_MON_0, 29),
	MUX_MULT_VOTE_FENC(CLK_CK2_SENINF3_SEL, "ck2_seninf3_sel", ck2_seninf3_parents,
		CKSYS2_CLK_CFG_0, CKSYS2_CLK_CFG_0_SET, CKSYS2_CLK_CFG_0_CLR, "mm-vote-regmap",
		MM_VOTE_CG_30_DONE, MM_VOTE_CG_30_SET, MM_VOTE_CG_30_CLR,
		24, 4, 31, CKSYS2_CLK_CFG_UPDATE, TOP_MUX_SENINF3_SHIFT,
		CKSYS2_CLK_FENC_STATUS_MON_0, 28),
	/* CKSYS2_CLK_CFG_1 */
	MUX_MULT_VOTE_FENC(CLK_CK2_SENINF4_SEL, "ck2_seninf4_sel", ck2_seninf4_parents,
		CKSYS2_CLK_CFG_1, CKSYS2_CLK_CFG_1_SET, CKSYS2_CLK_CFG_1_CLR, "mm-vote-regmap",
		MM_VOTE_CG_31_DONE, MM_VOTE_CG_31_SET, MM_VOTE_CG_31_CLR,
		0, 4, 7, CKSYS2_CLK_CFG_UPDATE, TOP_MUX_SENINF4_SHIFT,
		CKSYS2_CLK_FENC_STATUS_MON_0, 27),
	MUX_MULT_VOTE_FENC(CLK_CK2_SENINF5_SEL, "ck2_seninf5_sel", ck2_seninf5_parents,
		CKSYS2_CLK_CFG_1, CKSYS2_CLK_CFG_1_SET, CKSYS2_CLK_CFG_1_CLR, "mm-vote-regmap",
		MM_VOTE_CG_31_DONE, MM_VOTE_CG_31_SET, MM_VOTE_CG_31_CLR,
		8, 4, 15, CKSYS2_CLK_CFG_UPDATE, TOP_MUX_SENINF5_SHIFT,
		CKSYS2_CLK_FENC_STATUS_MON_0, 26),
	MUX_MULT_VOTE_FENC(CLK_CK2_IMG1_SEL, "ck2_img1_sel", ck2_img1_parents,
		CKSYS2_CLK_CFG_1, CKSYS2_CLK_CFG_1_SET, CKSYS2_CLK_CFG_1_CLR, "mm-vote-regmap",
		MM_VOTE_CG_31_DONE, MM_VOTE_CG_31_SET, MM_VOTE_CG_31_CLR,
		16, 4, 23, CKSYS2_CLK_CFG_UPDATE, TOP_MUX_IMG1_SHIFT,
		CKSYS2_CLK_FENC_STATUS_MON_0, 25),
	MUX_MULT_VOTE_FENC(CLK_CK2_IPE_SEL, "ck2_ipe_sel", ck2_ipe_parents,
		CKSYS2_CLK_CFG_1, CKSYS2_CLK_CFG_1_SET, CKSYS2_CLK_CFG_1_CLR, "mm-vote-regmap",
		MM_VOTE_CG_31_DONE, MM_VOTE_CG_31_SET, MM_VOTE_CG_31_CLR,
		24, 4, 31, CKSYS2_CLK_CFG_UPDATE, TOP_MUX_IPE_SHIFT,
		CKSYS2_CLK_FENC_STATUS_MON_0, 24),
	/* CKSYS2_CLK_CFG_2 */
	MUX_MULT_VOTE_FENC(CLK_CK2_CAM_SEL, "ck2_cam_sel", ck2_cam_parents,
		CKSYS2_CLK_CFG_2, CKSYS2_CLK_CFG_2_SET, CKSYS2_CLK_CFG_2_CLR, "mm-vote-regmap",
		MM_VOTE_CG_32_DONE, MM_VOTE_CG_32_SET, MM_VOTE_CG_32_CLR,
		0, 4, 7, CKSYS2_CLK_CFG_UPDATE, TOP_MUX_CAM_SHIFT,
		CKSYS2_CLK_FENC_STATUS_MON_0, 23),
	MUX_MULT_VOTE_FENC(CLK_CK2_CAMTM_SEL, "ck2_camtm_sel", ck2_camtm_parents,
		CKSYS2_CLK_CFG_2, CKSYS2_CLK_CFG_2_SET, CKSYS2_CLK_CFG_2_CLR, "mm-vote-regmap",
		MM_VOTE_CG_32_DONE, MM_VOTE_CG_32_SET, MM_VOTE_CG_32_CLR,
		8, 3, 15, CKSYS2_CLK_CFG_UPDATE, TOP_MUX_CAMTM_SHIFT,
		CKSYS2_CLK_FENC_STATUS_MON_0, 22),
	MUX_MULT_VOTE_FENC(CLK_CK2_DPE_SEL, "ck2_dpe_sel", ck2_dpe_parents,
		CKSYS2_CLK_CFG_2, CKSYS2_CLK_CFG_2_SET, CKSYS2_CLK_CFG_2_CLR, "mm-vote-regmap",
		MM_VOTE_CG_32_DONE, MM_VOTE_CG_32_SET, MM_VOTE_CG_32_CLR,
		16, 4, 23, CKSYS2_CLK_CFG_UPDATE, TOP_MUX_DPE_SHIFT,
		CKSYS2_CLK_FENC_STATUS_MON_0, 21),
	MUX_MULT_VOTE_FENC(CLK_CK2_VDEC_SEL, "ck2_vdec_sel", ck2_vdec_parents,
		CKSYS2_CLK_CFG_2, CKSYS2_CLK_CFG_2_SET, CKSYS2_CLK_CFG_2_CLR, "mm-vote-regmap",
		MM_VOTE_CG_32_DONE, MM_VOTE_CG_32_SET, MM_VOTE_CG_32_CLR,
		24, 4, 31, CKSYS2_CLK_CFG_UPDATE, TOP_MUX_VDEC_SHIFT,
		CKSYS2_CLK_FENC_STATUS_MON_0, 20),
	/* CKSYS2_CLK_CFG_3 */
	MUX_MULT_VOTE_FENC(CLK_CK2_CCUSYS_SEL, "ck2_ccusys_sel", ck2_ccusys_parents,
		CKSYS2_CLK_CFG_3, CKSYS2_CLK_CFG_3_SET, CKSYS2_CLK_CFG_3_CLR, "mm-vote-regmap",
		MM_VOTE_CG_33_DONE, MM_VOTE_CG_33_SET, MM_VOTE_CG_33_CLR,
		0, 4, 7, CKSYS2_CLK_CFG_UPDATE, TOP_MUX_CCUSYS_SHIFT,
		CKSYS2_CLK_FENC_STATUS_MON_0, 19),
	MUX_MULT_VOTE_FENC(CLK_CK2_CCUTM_SEL, "ck2_ccutm_sel", ck2_ccutm_parents,
		CKSYS2_CLK_CFG_3, CKSYS2_CLK_CFG_3_SET, CKSYS2_CLK_CFG_3_CLR, "mm-vote-regmap",
		MM_VOTE_CG_33_DONE, MM_VOTE_CG_33_SET, MM_VOTE_CG_33_CLR,
		8, 3, 15, CKSYS2_CLK_CFG_UPDATE, TOP_MUX_CCUTM_SHIFT,
		CKSYS2_CLK_FENC_STATUS_MON_0, 18),
	MUX_MULT_VOTE_FENC(CLK_CK2_VENC_SEL, "ck2_venc_sel", ck2_venc_parents,
		CKSYS2_CLK_CFG_3, CKSYS2_CLK_CFG_3_SET, CKSYS2_CLK_CFG_3_CLR, "mm-vote-regmap",
		MM_VOTE_CG_33_DONE, MM_VOTE_CG_33_SET, MM_VOTE_CG_33_CLR,
		16, 4, 23, CKSYS2_CLK_CFG_UPDATE, TOP_MUX_VENC_SHIFT,
		CKSYS2_CLK_FENC_STATUS_MON_0, 17),
	MUX_GATE_FENC_CLR_SET_UPD(CLK_CK2_DVO_SEL, "ck2_dvo_sel", ck2_dvo_parents,
		CKSYS2_CLK_CFG_3, CKSYS2_CLK_CFG_3_SET, CKSYS2_CLK_CFG_3_CLR,
		24, 3, 31, CKSYS2_CLK_CFG_UPDATE, TOP_MUX_DVO_SHIFT,
		CKSYS2_CLK_FENC_STATUS_MON_0, 16),
	MUX_GATE_FENC_CLR_SET_UPD(CLK_CK2_DVO_FAVT_SEL, "ck2_dvo_favt_sel", ck2_dvo_favt_parents,
		CKSYS2_CLK_CFG_4, CKSYS2_CLK_CFG_4_SET, CKSYS2_CLK_CFG_4_CLR,
		0, 3, 7, CKSYS2_CLK_CFG_UPDATE, TOP_MUX_DVO_FAVT_SHIFT,
		CKSYS2_CLK_FENC_STATUS_MON_0, 15),
	MUX_GATE_FENC_CLR_SET_UPD(CLK_CK2_DP1_SEL, "ck2_dp1_sel", ck2_dp1_parents,
		CKSYS2_CLK_CFG_4, CKSYS2_CLK_CFG_4_SET, CKSYS2_CLK_CFG_4_CLR,
		8, 3, 15, CKSYS2_CLK_CFG_UPDATE, TOP_MUX_DP1_SHIFT,
		CKSYS2_CLK_FENC_STATUS_MON_0, 14),
	MUX_GATE_FENC_CLR_SET_UPD(CLK_CK2_DP0_SEL, "ck2_dp0_sel", ck2_dp0_parents,
		CKSYS2_CLK_CFG_4, CKSYS2_CLK_CFG_4_SET, CKSYS2_CLK_CFG_4_CLR,
		16, 3, 23, CKSYS2_CLK_CFG_UPDATE, TOP_MUX_DP0_SHIFT,
		CKSYS2_CLK_FENC_STATUS_MON_0, 13),
	MUX_MULT_VOTE_FENC(CLK_CK2_DISP_SEL, "ck2_disp_sel", ck2_disp_parents,
		CKSYS2_CLK_CFG_4, CKSYS2_CLK_CFG_4_SET, CKSYS2_CLK_CFG_4_CLR, "mm-vote-regmap",
		MM_VOTE_CG_34_DONE, MM_VOTE_CG_34_SET, MM_VOTE_CG_34_CLR,
		24, 4, 31, CKSYS2_CLK_CFG_UPDATE, TOP_MUX_DISP_SHIFT,
		CKSYS2_CLK_FENC_STATUS_MON_0, 12),
	/* CKSYS2_CLK_CFG_5 */
	MUX_MULT_VOTE_FENC(CLK_CK2_MDP_SEL, "ck2_mdp_sel", ck2_mdp_parents,
		CKSYS2_CLK_CFG_5, CKSYS2_CLK_CFG_5_SET, CKSYS2_CLK_CFG_5_CLR, "mm-vote-regmap",
		MM_VOTE_CG_35_DONE, MM_VOTE_CG_35_SET, MM_VOTE_CG_35_CLR,
		0, 4, 7, CKSYS2_CLK_CFG_UPDATE, TOP_MUX_MDP_SHIFT,
		CKSYS2_CLK_FENC_STATUS_MON_0, 11),
	MUX_MULT_VOTE_FENC(CLK_CK2_MMINFRA_SEL, "ck2_mminfra_sel", ck2_mminfra_parents,
		CKSYS2_CLK_CFG_5, CKSYS2_CLK_CFG_5_SET, CKSYS2_CLK_CFG_5_CLR, "mm-vote-regmap",
		MM_VOTE_CG_35_DONE, MM_VOTE_CG_35_SET, MM_VOTE_CG_35_CLR,
		8, 4, 15, CKSYS2_CLK_CFG_UPDATE, TOP_MUX_MMINFRA_SHIFT,
		CKSYS2_CLK_FENC_STATUS_MON_0, 10),
	MUX_MULT_VOTE_FENC(CLK_CK2_MMINFRA_SNOC_SEL, "ck2_mminfra_snoc_sel", ck2_mminfra_snoc_parents,
		CKSYS2_CLK_CFG_5, CKSYS2_CLK_CFG_5_SET, CKSYS2_CLK_CFG_5_CLR, "mm-vote-regmap",
		MM_VOTE_CG_35_DONE, MM_VOTE_CG_35_SET, MM_VOTE_CG_35_CLR,
		16, 4, 23, CKSYS2_CLK_CFG_UPDATE, TOP_MUX_MMINFRA_SNOC_SHIFT,
		CKSYS2_CLK_FENC_STATUS_MON_0, 9),
	MUX_MULT_VOTE_FENC(CLK_CK2_MMUP_SEL, "ck2_mmup_sel", ck2_mmup_parents,
		CKSYS2_CLK_CFG_5, CKSYS2_CLK_CFG_5_SET, CKSYS2_CLK_CFG_5_CLR, "vote-regmap",
		VOTE_CG_30_DONE, VOTE_CG_30_SET, VOTE_CG_30_CLR,
		24, 3, 31, CKSYS2_CLK_CFG_UPDATE, TOP_MUX_MMUP_SHIFT,
		CKSYS2_CLK_FENC_STATUS_MON_0, 8),
	MUX_MULT_VOTE_FENC(CLK_CK2_MMINFRA_AO_SEL, "ck2_mminfra_ao_sel", ck2_mminfra_ao_parents,
		CKSYS2_CLK_CFG_6, CKSYS2_CLK_CFG_6_SET, CKSYS2_CLK_CFG_6_CLR, "mm-vote-regmap",
		MM_VOTE_CG_36_DONE, MM_VOTE_CG_36_SET, MM_VOTE_CG_36_CLR,
		16, 2, 7, CKSYS2_CLK_CFG_UPDATE, TOP_MUX_MMINFRA_AO_SHIFT,
		CKSYS2_CLK_FENC_STATUS_MON_0, 5),
};

static int clk_mt8196_ck2_probe(struct platform_device *pdev)
{
	struct clk_hw_onecell_data *clk_data;
	int r;
	struct device_node *node = pdev->dev.of_node;

	clk_data = mtk_alloc_clk_data(CLK_CK2_NR_CLK);

	mtk_clk_register_factors(ck2_divs, ARRAY_SIZE(ck2_divs), clk_data);

	mtk_clk_register_muxes(&pdev->dev, ck2_muxes, ARRAY_SIZE(ck2_muxes), node,
			       &mt8196_clk_ck2_lock, clk_data);

	r = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, clk_data);
	if (r)
		dev_err(&pdev->dev, "%s(): could not register clock provider: %d\n",
			__func__, r);

	return r;
}

static void clk_mt8196_ck2_remove(struct platform_device *pdev)
{
	struct clk_hw_onecell_data *clk_data = platform_get_drvdata(pdev);
	struct device_node *node = pdev->dev.of_node;

	of_clk_del_provider(node);
	mtk_clk_unregister_muxes(ck2_muxes, ARRAY_SIZE(ck2_muxes), clk_data);
	mtk_clk_unregister_factors(ck2_divs, ARRAY_SIZE(ck2_divs), clk_data);
	mtk_free_clk_data(clk_data);
}

static const struct of_device_id of_match_clk_mt8196_ck2[] = {
	{ .compatible = "mediatek,mt8196-cksys_gp2", },
	{ /* sentinel */ }
};

static struct platform_driver clk_mt8196_ck2_drv = {
	.probe = clk_mt8196_ck2_probe,
	.remove = clk_mt8196_ck2_remove,
	.driver = {
		.name = "clk-mt8196-ck2",
		.owner = THIS_MODULE,
		.of_match_table = of_match_clk_mt8196_ck2,
	},
};

module_platform_driver(clk_mt8196_ck2_drv);
MODULE_LICENSE("GPL");
