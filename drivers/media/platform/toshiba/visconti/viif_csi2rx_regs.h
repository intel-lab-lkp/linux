/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/* Toshiba Visconti Video Capture register definitions
 *
 * (C) Copyright 2023 TOSHIBA CORPORATION
 * (C) Copyright 2023 Toshiba Electronic Devices & Storage Corporation
 */

#ifndef VIIF_CSI2RX_REGS_H
#define VIIF_CSI2RX_REGS_H

/*=============================================*/
/* CSI2HOST registers */
/*=============================================*/
#define REG_CSI2RX_NLANES	 0x4
#define REG_CSI2RX_PHY_SHUTDOWNZ 0x40
#define REG_CSI2RX_PHY_RSTZ	 0x44

/* access to dphy external registers */
#define REG_CSI2RX_PHY_TESTCTRL0 0x50
#define BIT_TESTCTRL0_CLK_0	 0
#define BIT_TESTCTRL0_CLK_1	 BIT(1)

#define REG_CSI2RX_PHY_TESTCTRL1 0x54
#define BIT_TESTCTRL1_ADDR	 BIT(16)
#define MASK_TESTCTRL1_DIN	 0xFF
#define MASK_TESTCTRL1_DOUT	 0xFF00

#define REG_CSI2RX_INT_ST_PHY_FATAL  0xE0
#define REG_CSI2RX_INT_MSK_PHY_FATAL 0xE4
#define MASK_PHY_FATAL_ALL	     0x0000000F

#define REG_CSI2RX_INT_ST_PKT_FATAL  0xF0
#define REG_CSI2RX_INT_MSK_PKT_FATAL 0xF4
#define MASK_PKT_FATAL_ALL	     0x0001000F

#define REG_CSI2RX_INT_ST_FRAME_FATAL  0x100
#define REG_CSI2RX_INT_MSK_FRAME_FATAL 0x104
#define MASK_FRAME_FATAL_ALL	       0x000F0F0F

#define REG_CSI2RX_INT_ST_PHY  0x110
#define REG_CSI2RX_INT_MSK_PHY 0x114
#define MASK_PHY_ERROR_ALL     0x000F000F

#define REG_CSI2RX_INT_ST_PKT  0x120
#define REG_CSI2RX_INT_MSK_PKT 0x124
#define MASK_PKT_ERROR_ALL     0x000F000F

#define REG_CSI2RX_INT_ST_LINE	0x130
#define REG_CSI2RX_INT_MSK_LINE 0x134
#define MASK_LINE_ERROR_ALL	0x00FF00FF

/*=============================================*/
/* DPHY register space */
/*=============================================*/
enum dphy_testcode {
	DIG_TESTCODE_EXT = 0,
	DIG_SYS_0 = 0x001,
	DIG_SYS_1 = 0x002,
	DIG_SYS_3 = 0x004,
	DIG_SYS_7 = 0x008,
	DIG_RX_STARTUP_OVR_2 = 0x0E2,
	DIG_RX_STARTUP_OVR_3 = 0x0E3,
	DIG_RX_STARTUP_OVR_4 = 0x0E4,
	DIG_RX_STARTUP_OVR_5 = 0x0E5,
	DIG_CB_2 = 0x1AC,
	DIG_TERM_CAL_0 = 0x220,
	DIG_TERM_CAL_1 = 0x221,
	DIG_TERM_CAL_2 = 0x222,
	DIG_CLKLANE_LANE_6 = 0x307,
	DIG_CLKLANE_OFFSET_CAL_0 = 0x39D,
	DIG_LANE0_OFFSET_CAL_0 = 0x59F,
	DIG_LANE0_DDL_0 = 0x5E0,
	DIG_LANE1_OFFSET_CAL_0 = 0x79F,
	DIG_LANE1_DDL_0 = 0x7E0,
	DIG_LANE2_OFFSET_CAL_0 = 0x99F,
	DIG_LANE2_DDL_0 = 0x9E0,
	DIG_LANE3_OFFSET_CAL_0 = 0xB9F,
	DIG_LANE3_DDL_0 = 0xBE0,
};

#define SYS_0_HSFREQRANGE_OVR  BIT(5)
#define SYS_3_NO_REXT	       BIT(4)
#define SYS_7_RESERVED	       FIELD_PREP(0x1F, 0x0C)
#define SYS_7_DESKEW_POL       BIT(5)
#define STARTUP_OVR_4_CNTVAL   FIELD_PREP(0x70, 0x01)
#define STARTUP_OVR_4_DDL_EN   BIT(0)
#define STARTUP_OVR_5_BYPASS   BIT(0)
#define CB_2_LPRX_BIAS	       BIT(6)
#define CB_2_RESERVED	       FIELD_PREP(0x3F, 0x0B)
#define CLKLANE_RXHS_PULL_LONG BIT(7)

/* bit mask for calibration result registers */
#define MASK_TERM_CAL_ERR  0
#define MASK_TERM_CAL_DONE BIT(7)
#define MASK_CLK_CAL_ERR   BIT(4)
#define MASK_CLK_CAL_DONE  BIT(0)
#define MASK_CAL_ERR	   BIT(2)
#define MASK_CAL_DONE	   BIT(1)
#define MASK_DDL_ERR	   BIT(1)
#define MASK_DDL_DONE	   BIT(2)

#endif /* VIIF_CSI2RX_REGS_H */
