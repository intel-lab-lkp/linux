/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/*
 * Copyright (c) 2013, The Linux Foundation. All rights reserved.
 *
 * Reset bindings for the MSM8x60 family (MSM8260/MSM8660/APQ8060) Multimedia Clock
 * Controller (MMCC).
 *
 * MSM8260, MSM8660 and APQ8060 are the same Scorpion-class MSM8x60 SoC
 * with different bin/feature labels. MSM8960 is a newer generation (Krait)
 * — its reset bindings live in
 * <dt-bindings/reset/qcom,mmcc-msm8960.h> and must not be reused here.
 *
 * IDs intentionally match the numeric values used by the original shared
 * mmcc-msm8960.h so the driver's qcom_reset_map array indexing is preserved;
 * only the resets actually implemented by mmcc-msm8660.c are defined.
 */

#ifndef _DT_BINDINGS_RESET_MSM_MMCC_8660_H
#define _DT_BINDINGS_RESET_MSM_MMCC_8660_H

#define VPE_AXI_RESET					0
#define IJPEG_AXI_RESET					1
#define MPD_AXI_RESET					2
#define VFE_AXI_RESET					3
#define SP_AXI_RESET					4
#define VCODEC_AXI_RESET				5
#define ROT_AXI_RESET					6
#define VCODEC_AXI_A_RESET				7
#define VCODEC_AXI_B_RESET				8
#define FAB_S3_AXI_RESET				9
#define FAB_S2_AXI_RESET				10
#define FAB_S1_AXI_RESET				11
#define FAB_S0_AXI_RESET				12
#define SMMU_GFX3D_ABH_RESET				13
#define SMMU_VPE_AHB_RESET				14
#define SMMU_VFE_AHB_RESET				15
#define SMMU_ROT_AHB_RESET				16
#define SMMU_VCODEC_B_AHB_RESET				17
#define SMMU_VCODEC_A_AHB_RESET				18
#define SMMU_MDP1_AHB_RESET				19
#define SMMU_MDP0_AHB_RESET				20
#define SMMU_JPEGD_AHB_RESET				21
#define SMMU_IJPEG_AHB_RESET				22
#define APU_AHB_RESET					25
#define CSI_AHB_RESET					26
#define TV_ENC_AHB_RESET				27
#define VPE_AHB_RESET					28
#define FABRIC_AHB_RESET				29
#define GFX2D0_AHB_RESET				30
#define GFX2D1_AHB_RESET				31
#define GFX3D_AHB_RESET					32
#define HDMI_AHB_RESET					33
#define MSSS_IMEM_AHB_RESET				34
#define IJPEG_AHB_RESET					35
#define DSI_M_AHB_RESET					36
#define DSI_S_AHB_RESET					37
#define JPEGD_AHB_RESET					38
#define MDP_AHB_RESET					39
#define ROT_AHB_RESET					40
#define VCODEC_AHB_RESET				41
#define VFE_AHB_RESET					42
#define CSIPHY0_RESET					47
#define CSIPHY1_RESET					48
#define VFE_CSI_RESET					50
#define MDP_RESET					51
#define AMP_RESET					52
#define JPEGD_RESET					53
#define CSI1_RESET					54
#define VPE_RESET					55
#define MMSS_FABRIC_RESET				56
#define VFE_RESET					57
#define GFX3D_RESET					60
#define HDMI_RESET					61
#define MMSS_IMEM_RESET					62
#define IJPEG_RESET					63
#define CSI0_RESET					64
#define DSI_RESET					65
#define VCODEC_RESET					66
#define MDP_TV_RESET					67
#define MDP_VSYNC_RESET					68
#define ROT_RESET					69
#define TV_HDMI_RESET					70
#define TV_ENC_RESET					71
#define GFX3D_AXI_RESET					75
#define CSI_RDI_RESET					79
#define CSI_PIX_RESET					80

#endif
