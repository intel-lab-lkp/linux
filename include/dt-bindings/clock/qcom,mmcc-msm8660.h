/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/*
 * Copyright (c) 2013, The Linux Foundation. All rights reserved.
 *
 * Clock and power-domain bindings for the MSM8x60 family (MSM8260/MSM8660/APQ8060)
 * Multimedia Clock Controller (MMCC).
 *
 * MSM8260, MSM8660 and APQ8060 are the same Scorpion-class MSM8x60 SoC
 * with different bin/feature labels. MSM8960 is a newer generation (Krait)
 * — its bindings live in
 * <dt-bindings/clock/qcom,mmcc-msm8960.h> and must not be reused here.
 *
 * IDs below intentionally match the numeric values used by the original
 * shared mmcc-msm8960.h so the driver's clk array indexing is preserved;
 * only the clocks actually implemented by mmcc-msm8660.c are defined.
 */

#ifndef _DT_BINDINGS_CLK_MSM_MMCC_8660_H
#define _DT_BINDINGS_CLK_MSM_MMCC_8660_H

#define TV_ENC_AHB_CLK					3
#define AMP_AHB_CLK					4
#define JPEGD_AHB_CLK					6
#define GFX2D0_AHB_CLK					7
#define DSI_S_AHB_CLK					8
#define VPE_AHB_CLK					10
#define SMMU_AHB_CLK					11
#define HDMI_M_AHB_CLK					12
#define VFE_AHB_CLK					13
#define ROT_AHB_CLK					14
#define VCODEC_AHB_CLK					15
#define MDP_AHB_CLK					16
#define DSI_M_AHB_CLK					17
#define CSI0_AHB_CLK					18
#define MMSS_IMEM_AHB_CLK				19
#define IJPEG_AHB_CLK					20
#define HDMI_S_AHB_CLK					21
#define GFX3D_AHB_CLK					22
#define GFX2D1_AHB_CLK					23
#define JPEGD_AXI_CLK					28
#define GMEM_AXI_CLK					29
#define MDP_AXI_CLK					30
#define MMSS_IMEM_AXI_CLK				31
#define IJPEG_AXI_CLK					32
#define GFX3D_AXI_CLK					33
#define VCODEC_AXI_CLK					34
#define VFE_AXI_CLK					35
#define VPE_AXI_CLK					36
#define ROT_AXI_CLK					37
#define VCODEC_AXI_A_CLK				38
#define VCODEC_AXI_B_CLK				39
#define CSI0_SRC					47
#define CSI0_CLK					48
#define CSI0_PHY_CLK					49
#define CSI1_SRC					50
#define CSI1_CLK					51
#define CSI1_PHY_CLK					52
#define DSI_SRC						56
#define DSI_CLK						57
#define CSI_PIX_CLK					58
#define CSI_RDI_CLK					59
#define MDP_VSYNC_CLK					60
#define HDMI_APP_CLK					62
#define GFX2D0_SRC					66
#define GFX2D0_CLK					67
#define GFX2D1_SRC					68
#define GFX2D1_CLK					69
#define GFX3D_SRC					70
#define GFX3D_CLK					71
#define IJPEG_SRC					72
#define IJPEG_CLK					73
#define JPEGD_SRC					74
#define JPEGD_CLK					75
#define MDP_SRC						76
#define MDP_CLK						77
#define MDP_LUT_CLK					78
#define DSI1_BYTE_SRC					83
#define DSI1_BYTE_CLK					84
#define DSI1_ESC_SRC					87
#define DSI1_ESC_CLK					88
#define ROT_SRC						91
#define ROT_CLK						92
#define TV_ENC_CLK					93
#define TV_DAC_CLK					94
#define HDMI_TV_CLK					95
#define MDP_TV_CLK					96
#define TV_SRC						97
#define VCODEC_SRC					98
#define VCODEC_CLK					99
#define VFE_SRC						100
#define VFE_CLK						101
#define VFE_CSI0_CLK					102
#define VPE_SRC						103
#define VPE_CLK						104
#define DSI_PIXEL_SRC					105
#define DSI_PIXEL_CLK					106
#define CAMCLK0_SRC					107
#define CAMCLK0_CLK					108
#define CAMCLK1_SRC					109
#define CAMCLK1_CLK					110
#define CSIPHYTIMER_SRC					113
#define CSIPHY1_TIMER_CLK				115
#define CSIPHY0_TIMER_CLK				116
#define PLL2						118
#define MDP_PIXEL_SRC					129
#define MDP_PIXEL_CLK					130
#define MDP_LCDC_CLK					131
#define VFE_CSI1_CLK					132
#define CSI1_AHB_CLK					133

/*
 * MSM8x60 legacy footswitch power domains.
 * Used with the MMCC power-domain provider (#power-domain-cells = <1>).
 * Numbering is independent of the clock ID space above.
 */
#define GFX2D0_GDSC					0
#define GFX2D1_GDSC					1
#define GFX3D_GDSC					2
#define IJPEG_GDSC					3
#define MDP_GDSC					4
#define ROT_GDSC					5
#define VED_GDSC					6
#define VFE_GDSC					7
#define VPE_GDSC					8

#endif
