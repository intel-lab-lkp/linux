/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Register definitions for PIXPAPER e-ink panel
 *
 * Author: LiangCheng Wang <zaq14760@gmail.com>
 */

#ifndef __PIXPAPER_REGS_H__
#define __PIXPAPER_REGS_H__

/*
 * Note on Undocumented Commands/Registers:
 *
 * Several commands and register parameters defined in this header are not
 * documented in the datasheet. Their values and usage have been derived
 * through analysis of existing userspace example programs.
 *
 * These 'unknown' definitions are crucial for the proper initialization
 * and stable operation of the panel. Modifying these values without
 * thorough understanding may lead to display anomalies, panel damage,
 * or unexpected behavior.
 */

/* Command definitions */
#define PIXPAPER_CMD_PANEL_SETTING 0x00	/* R00H: Panel settings */
#define PIXPAPER_CMD_POWER_SETTING 0x01	/* R01H: Power settings */
#define PIXPAPER_CMD_POWER_OFF 0x02		/* R02H: Power off */
#define PIXPAPER_CMD_POWER_OFF_SEQUENCE 0x03	/* R03H: Power off sequence */
#define PIXPAPER_CMD_POWER_ON 0x04		/* R04H: Power on */
#define PIXPAPER_CMD_BOOSTER_SOFT_START 0x06	/* R06H: Booster soft start */
#define PIXPAPER_CMD_DEEP_SLEEP 0x07		/* R07H: Deep sleep */
#define PIXPAPER_CMD_DATA_START_TRANSMISSION 0x10
/* R10H: Data transmission start */
#define PIXPAPER_CMD_DISPLAY_REFRESH 0x12	/* R12H: Display refresh */
#define PIXPAPER_CMD_PLL_CONTROL 0x30		/* R30H: PLL control */
#define PIXPAPER_CMD_TEMP_SENSOR_CALIB 0x41
/* R41H: Temperature sensor calibration */
#define PIXPAPER_CMD_UNKNOWN_4D 0x4D		/* R4DH: Unknown command */
#define PIXPAPER_CMD_VCOM_INTERVAL 0x50	/* R50H: VCOM interval */
#define PIXPAPER_CMD_UNKNOWN_60 0x60		/* R60H: Unknown command */
#define PIXPAPER_CMD_RESOLUTION_SETTING 0x61	/* R61H: Resolution settings */
#define PIXPAPER_CMD_GATE_SOURCE_START 0x65	/* R65H: Gate/source start */
#define PIXPAPER_CMD_UNKNOWN_B4 0xB4		/* RB4H: Unknown command */
#define PIXPAPER_CMD_UNKNOWN_B5 0xB5		/* RB5H: Unknown command */
#define PIXPAPER_CMD_UNKNOWN_E0 0xE0		/* RE0H: Unknown command */
#define PIXPAPER_CMD_POWER_SAVING 0xE3		/* RE3H: Power saving */
#define PIXPAPER_CMD_UNKNOWN_E7 0xE7		/* RE7H: Unknown command */
#define PIXPAPER_CMD_UNKNOWN_E9 0xE9		/* RE9H: Unknown command */

/* R00H PSR - First Parameter */
#define PIXPAPER_PSR_RST_N BIT(0)
/* Bit 0: RST_N, 1=no effect (default), 0=reset with booster OFF */
#define PIXPAPER_PSR_SHD_N BIT(1)
/* Bit 1: SHD_N, 1=booster ON (default), 0=booster OFF */
#define PIXPAPER_PSR_SHL BIT(2)
/* Bit 2: SHL, 1=shift right (default), 0=shift left */
#define PIXPAPER_PSR_UD BIT(3)
/* Bit 3: UD, 1=scan up (default), 0=scan down */
#define PIXPAPER_PSR_PST_MODE BIT(5)
/* Bit 5: PST_MODE, 0=frame scanning (default), 1=external */
#define PIXPAPER_PSR_RES_MASK (3 << 6)
/* Bits 7-6: RES[1:0], resolution setting */
#define PIXPAPER_PSR_RES_176x296 (0x0 << 6)	/* 00: 176x296 */
#define PIXPAPER_PSR_RES_128x296 (0x1 << 6)	/* 01: 128x296 */
#define PIXPAPER_PSR_RES_128x250 (0x2 << 6)	/* 10: 128x250 */
#define PIXPAPER_PSR_RES_112x204 (0x3 << 6)	/* 11: 112x204 */
#define PIXPAPER_PSR_CONFIG                                           \
	(PIXPAPER_PSR_RST_N | PIXPAPER_PSR_SHD_N | PIXPAPER_PSR_SHL | \
	 PIXPAPER_PSR_UD)
/* 0x0F: Default settings, resolution set by R61H */

/* R00H PSR - Second Parameter */
#define PIXPAPER_PSR2_VC_LUTZ \
	(1 << 0) /* Bit 0: VC_LUTZ, 1=VCOM float after refresh (default), 0=no effect */
#define PIXPAPER_PSR2_NORG \
	(1 << 1) /* Bit 1: NORG, 1=VCOM to GND before power off, 0=no effect (default) */
#define PIXPAPER_PSR2_TIEG \
	(1 << 2) /* Bit 2: TIEG, 1=VGN to GND on power off, 0=no effect (default) */
#define PIXPAPER_PSR2_TS_AUTO \
	(1 << 3) /* Bit 3: TS_AUTO, 1=sensor on RST_N low to high (default), 0=on booster */
#define PIXPAPER_PSR2_VCMZ \
	(1 << 4) /* Bit 4: VCMZ, 1=VCOM always floating, 0=no effect (default) */
#define PIXPAPER_PSR2_FOPT \
	(1 << 5) /* Bit 5: FOPT, 0=scan 1 frame (default), 1=no scan, HiZ */
#define PIXPAPER_PSR_CONFIG2     \
	(PIXPAPER_PSR2_VC_LUTZ | \
	 PIXPAPER_PSR2_TS_AUTO) /* 0x09: Default VCOM and temp sensor settings */

/* R01H PWR - Power Setting Register */
/* First Parameter */
#define PIXPAPER_PWR_VDG_EN \
	(1 << 0) /* Bit 0: VDG_EN, 1=internal DCDC for VGP/VGN (default), 0=external */
#define PIXPAPER_PWR_VDS_EN \
	(1 << 1) /* Bit 1: VDS_EN, 1=internal regulator for VSP/VSN (default), 0=external */
#define PIXPAPER_PWR_VSC_EN \
	(1 << 2) /* Bit 2: VSC_EN, 1=internal regulator for VSPL (default), 0=external */
#define PIXPAPER_PWR_V_MODE \
	(1 << 3) /* Bit 3: V_MODE, 0=Mode0 (default), 1=Mode1 */
#define PIXPAPER_PWR_CONFIG1                         \
	(PIXPAPER_PWR_VDG_EN | PIXPAPER_PWR_VDS_EN | \
	 PIXPAPER_PWR_VSC_EN) /* 0x07: Internal power for VGP/VGN, VSP/VSN, VSPL */

/* Second Parameter */
#define PIXPAPER_PWR_VGPN_MASK \
	(3 << 0) /* Bits 1-0: VGPN, VGP/VGN voltage levels */
#define PIXPAPER_PWR_VGPN_20V (0x0 << 0) /* 00: VGP=20V, VGN=-20V (default) */
#define PIXPAPER_PWR_VGPN_17V (0x1 << 0) /* 01: VGP=17V, VGN=-17V */
#define PIXPAPER_PWR_VGPN_15V (0x2 << 0) /* 10: VGP=15V, VGN=-15V */
#define PIXPAPER_PWR_VGPN_10V (0x3 << 0) /* 11: VGP=10V, VGN=-10V */
#define PIXPAPER_PWR_CONFIG2 PIXPAPER_PWR_VGPN_20V /* 0x00: VGP=20V, VGN=-20V */

/* Third, Fourth, Sixth Parameters (VSP_1, VSPL_0, VSPL_1) */
#define PIXPAPER_PWR_VSP_8_2V 0x22 /* VSP_1/VSPL_1: 8.2V (34 decimal) */
#define PIXPAPER_PWR_VSPL_15V 0x78 /* VSPL_0: 15V (120 decimal) */

/* Fifth Parameter (VSN_1) */
#define PIXPAPER_PWR_VSN_4V 0x0A /* VSN_1: -4V (10 decimal) */

/* R03H PFS - Power Off Sequence Setting Register */
/* First Parameter */
#define PIXPAPER_PFS_T_VDS_OFF_MASK \
	(3 << 0) /* Bits 1-0: T_VDS_OFF, VSP/VSN power-off sequence */
#define PIXPAPER_PFS_T_VDS_OFF_20MS (0x0 << 0) /* 00: 20 ms (default) */
#define PIXPAPER_PFS_T_VDS_OFF_40MS (0x1 << 0) /* 01: 40 ms */
#define PIXPAPER_PFS_T_VDS_OFF_60MS (0x2 << 0) /* 10: 60 ms */
#define PIXPAPER_PFS_T_VDS_OFF_80MS (0x3 << 0) /* 11: 80 ms */
#define PIXPAPER_PFS_T_VDPG_OFF_MASK \
	(3 << 4) /* Bits 5-4: T_VDPG_OFF, VGP/VGN power-off sequence */
#define PIXPAPER_PFS_T_VDPG_OFF_20MS (0x0 << 4) /* 00: 20 ms (default) */
#define PIXPAPER_PFS_T_VDPG_OFF_40MS (0x1 << 4) /* 01: 40 ms */
#define PIXPAPER_PFS_T_VDPG_OFF_60MS (0x2 << 4) /* 10: 60 ms */
#define PIXPAPER_PFS_T_VDPG_OFF_80MS (0x3 << 4) /* 11: 80 ms */
#define PIXPAPER_PFS_CONFIG1           \
	(PIXPAPER_PFS_T_VDS_OFF_20MS | \
	 PIXPAPER_PFS_T_VDPG_OFF_20MS) /* 0x10: Default 20 ms for VSP/VSN and VGP/VGN */

/* Second Parameter */
#define PIXPAPER_PFS_VGP_EXT_MASK \
	(0xF << 0) /* Bits 3-0: VGP_EXT, VGP extension time */
#define PIXPAPER_PFS_VGP_EXT_0MS (0x0 << 0) /* 0000: 0 ms */
#define PIXPAPER_PFS_VGP_EXT_500MS (0x1 << 0) /* 0001: 500 ms */
#define PIXPAPER_PFS_VGP_EXT_1000MS (0x2 << 0) /* 0010: 1000 ms */
#define PIXPAPER_PFS_VGP_EXT_1500MS (0x3 << 0) /* 0011: 1500 ms */
#define PIXPAPER_PFS_VGP_EXT_2000MS (0x4 << 0) /* 0100: 2000 ms (default) */
#define PIXPAPER_PFS_VGP_EXT_2500MS (0x5 << 0) /* 0101: 2500 ms */
#define PIXPAPER_PFS_VGP_EXT_3000MS (0x6 << 0) /* 0110: 3000 ms */
#define PIXPAPER_PFS_VGP_EXT_3500MS (0x7 << 0) /* 0111: 3500 ms */
#define PIXPAPER_PFS_VGP_EXT_4000MS (0x8 << 0) /* 1000: 4000 ms */
#define PIXPAPER_PFS_VGP_EXT_4500MS (0x9 << 0) /* 1001: 4500 ms */
#define PIXPAPER_PFS_VGP_EXT_5000MS (0xA << 0) /* 1010: 5000 ms */
#define PIXPAPER_PFS_VGP_EXT_5500MS (0xB << 0) /* 1011: 5500 ms */
#define PIXPAPER_PFS_VGP_EXT_6000MS (0xC << 0) /* 1100: 6000 ms */
#define PIXPAPER_PFS_VGP_EXT_6500MS (0xD << 0) /* 1101: 6500 ms */
#define PIXPAPER_PFS_VGP_LEN_MASK \
	(0xF << 4) /* Bits 7-4: VGP_LEN, VGP at 10V during power-off */
#define PIXPAPER_PFS_VGP_LEN_0MS (0x0 << 4) /* 0000: 0 ms */
#define PIXPAPER_PFS_VGP_LEN_500MS (0x1 << 4) /* 0001: 500 ms */
#define PIXPAPER_PFS_VGP_LEN_1000MS (0x2 << 4) /* 0010: 1000 ms */
#define PIXPAPER_PFS_VGP_LEN_1500MS (0x3 << 4) /* 0011: 1500 ms */
#define PIXPAPER_PFS_VGP_LEN_2000MS (0x4 << 4) /* 0100: 2000 ms */
#define PIXPAPER_PFS_VGP_LEN_2500MS (0x5 << 4) /* 0101: 2500 ms (default) */
#define PIXPAPER_PFS_VGP_LEN_3000MS (0x6 << 4) /* 0110: 3000 ms */
#define PIXPAPER_PFS_VGP_LEN_3500MS (0x7 << 4) /* 0111: 3500 ms */
#define PIXPAPER_PFS_VGP_LEN_4000MS (0x8 << 4) /* 1000: 4000 ms */
#define PIXPAPER_PFS_VGP_LEN_4500MS (0x9 << 4) /* 1001: 4500 ms */
#define PIXPAPER_PFS_VGP_LEN_5000MS (0xA << 4) /* 1010: 5000 ms */
#define PIXPAPER_PFS_VGP_LEN_5500MS (0xB << 4) /* 1011: 5500 ms */
#define PIXPAPER_PFS_VGP_LEN_6000MS (0xC << 4) /* 1100: 6000 ms */
#define PIXPAPER_PFS_VGP_LEN_6500MS (0xD << 4) /* 1101: 6500 ms */
#define PIXPAPER_PFS_CONFIG2           \
	(PIXPAPER_PFS_VGP_EXT_1000MS | \
	 PIXPAPER_PFS_VGP_LEN_2500MS) /* 0x54: VGP extension 1000 ms, VGP at 10V for 2500 ms */

/* Third Parameter */
#define PIXPAPER_PFS_XON_LEN_MASK \
	(0xF << 0) /* Bits 3-0: XON_LEN, XON enable time */
#define PIXPAPER_PFS_XON_LEN_0MS (0x0 << 0) /* 0000: 0 ms */
#define PIXPAPER_PFS_XON_LEN_500MS (0x1 << 0) /* 0001: 500 ms */
#define PIXPAPER_PFS_XON_LEN_1000MS (0x2 << 0) /* 0010: 1000 ms */
#define PIXPAPER_PFS_XON_LEN_1500MS (0x3 << 0) /* 0011: 1500 ms */
#define PIXPAPER_PFS_XON_LEN_2000MS (0x4 << 0) /* 0100: 2000 ms (default) */
#define PIXPAPER_PFS_XON_LEN_2500MS (0x5 << 0) /* 0101: 2500 ms */
#define PIXPAPER_PFS_XON_LEN_3000MS (0x6 << 0) /* 0110: 3000 ms */
#define PIXPAPER_PFS_XON_LEN_3500MS (0x7 << 0) /* 0111: 3500 ms */
#define PIXPAPER_PFS_XON_LEN_4000MS (0x8 << 0) /* 1000: 4000 ms */
#define PIXPAPER_PFS_XON_LEN_4500MS (0x9 << 0) /* 1001: 4500 ms */
#define PIXPAPER_PFS_XON_LEN_5000MS (0xA << 0) /* 1010: 5000 ms */
#define PIXPAPER_PFS_XON_LEN_5500MS (0xB << 0) /* 1011: 5500 ms */
#define PIXPAPER_PFS_XON_LEN_6000MS (0xC << 0) /* 1100: 6000 ms */
#define PIXPAPER_PFS_XON_DLY_MASK \
	(0xF << 4) /* Bits 7-4: XON_DLY, XON delay time */
#define PIXPAPER_PFS_XON_DLY_0MS (0x0 << 4) /* 0000: 0 ms */
#define PIXPAPER_PFS_XON_DLY_500MS (0x1 << 4) /* 0001: 500 ms */
#define PIXPAPER_PFS_XON_DLY_1000MS (0x2 << 4) /* 0010: 1000 ms */
#define PIXPAPER_PFS_XON_DLY_1500MS (0x3 << 4) /* 0011: 1500 ms */
#define PIXPAPER_PFS_XON_DLY_2000MS (0x4 << 4) /* 0100: 2000 ms (default) */
#define PIXPAPER_PFS_XON_DLY_2500MS (0x5 << 4) /* 0101: 2500 ms */
#define PIXPAPER_PFS_XON_DLY_3000MS (0x6 << 4) /* 0110: 3000 ms */
#define PIXPAPER_PFS_XON_DLY_3500MS (0x7 << 4) /* 0111: 3500 ms */
#define PIXPAPER_PFS_XON_DLY_4000MS (0x8 << 4) /* 1000: 4000 ms */
#define PIXPAPER_PFS_XON_DLY_4500MS (0x9 << 4) /* 1001: 4500 ms */
#define PIXPAPER_PFS_XON_DLY_5000MS (0xA << 4) /* 1010: 5000 ms */
#define PIXPAPER_PFS_XON_DLY_5500MS (0xB << 4) /* 1011: 5500 ms */
#define PIXPAPER_PFS_XON_DLY_6000MS (0xC << 4) /* 1100: 6000 ms */
#define PIXPAPER_PFS_CONFIG3           \
	(PIXPAPER_PFS_XON_LEN_2000MS | \
	 PIXPAPER_PFS_XON_DLY_2000MS) /* 0x44: XON enable and delay at 2000 ms */

/* R06H BTST - Booster Soft Start Command */
/* First Parameter */
#define PIXPAPER_BTST_PHA_SFT_MASK \
	(3 << 0) /* Bits 1-0: PHA_SFT, soft start period for phase A */
#define PIXPAPER_BTST_PHA_SFT_10MS (0x0 << 0) /* 00: 10 ms (default) */
#define PIXPAPER_BTST_PHA_SFT_20MS (0x1 << 0) /* 01: 20 ms */
#define PIXPAPER_BTST_PHA_SFT_30MS (0x2 << 0) /* 10: 30 ms */
#define PIXPAPER_BTST_PHA_SFT_40MS (0x3 << 0) /* 11: 40 ms */
#define PIXPAPER_BTST_PHB_SFT_MASK \
	(3 << 2) /* Bits 3-2: PHB_SFT, soft start period for phase B */
#define PIXPAPER_BTST_PHB_SFT_10MS (0x0 << 2) /* 00: 10 ms (default) */
#define PIXPAPER_BTST_PHB_SFT_20MS (0x1 << 2) /* 01: 20 ms */
#define PIXPAPER_BTST_PHB_SFT_30MS (0x2 << 2) /* 10: 30 ms */
#define PIXPAPER_BTST_PHB_SFT_40MS (0x3 << 2) /* 11: 40 ms */
#define PIXPAPER_BTST_CONFIG1         \
	(PIXPAPER_BTST_PHA_SFT_40MS | \
	 PIXPAPER_BTST_PHB_SFT_40MS) /* 0x0F: 40 ms for phase A and B */

/* Second to Seventh Parameters (Driving Strength or Minimum OFF Time) */
#define PIXPAPER_BTST_CONFIG2 0x0A /* Strength11 */
#define PIXPAPER_BTST_CONFIG3 0x2F /* Period48 */
#define PIXPAPER_BTST_CONFIG4 0x25 /* Strength38 */
#define PIXPAPER_BTST_CONFIG5 0x22 /* Period35 */
#define PIXPAPER_BTST_CONFIG6 0x2E /* Strength47 */
#define PIXPAPER_BTST_CONFIG7 0x21 /* Period34 */

/* R12H: DRF (Display Refresh) */
#define PIXPAPER_DRF_VCOM_AC 0x00 /* AC VCOM: VCOM follows LUTC (default) */
#define PIXPAPER_DRF_VCOM_DC 0x01 /* DC VCOM: VCOM fixed to VCOMDC */

/* R30H PLL - PLL Control Register */
/* First Parameter */
#define PIXPAPER_PLL_FR_MASK (0x7 << 0) /* Bits 2-0: FR, frame rate */
#define PIXPAPER_PLL_FR_12_5HZ (0x0 << 0) /* 000: 12.5 Hz */
#define PIXPAPER_PLL_FR_25HZ (0x1 << 0)	/* 001: 25 Hz */
#define PIXPAPER_PLL_FR_50HZ (0x2 << 0) /* 010: 50 Hz (default) */
#define PIXPAPER_PLL_FR_65HZ (0x3 << 0) /* 011: 65 Hz */
#define PIXPAPER_PLL_FR_75HZ (0x4 << 0) /* 100: 75 Hz */
#define PIXPAPER_PLL_FR_85HZ (0x5 << 0) /* 101: 85 Hz */
#define PIXPAPER_PLL_FR_100HZ (0x6 << 0) /* 110: 100 Hz */
#define PIXPAPER_PLL_FR_120HZ (0x7 << 0) /* 111: 120 Hz */
#define PIXPAPER_PLL_DFR \
	(1 << 3) /* Bit 3: Dynamic frame rate, 0=disabled (default), 1=enabled */
#define PIXPAPER_PLL_CONFIG \
	(PIXPAPER_PLL_FR_50HZ) /* 0x02: 50 Hz, dynamic frame rate disabled */

/* R41H TSE - Temperature Sensor Calibration Register */
/* First Parameter */
#define PIXPAPER_TSE_TO_MASK \
	(0xF << 0) /* Bits 3-0: TO[3:0], temperature offset */
#define PIXPAPER_TSE_TO_POS_0C (0x0 << 0) /* 0000: +0°C (default) */
#define PIXPAPER_TSE_TO_POS_0_5C (0x1 << 0) /* 0001: +0.5°C */
#define PIXPAPER_TSE_TO_POS_1C (0x2 << 0) /* 0010: +1°C */
#define PIXPAPER_TSE_TO_POS_1_5C (0x3 << 0) /* 0011: +1.5°C */
#define PIXPAPER_TSE_TO_POS_2C (0x4 << 0) /* 0100: +2°C */
#define PIXPAPER_TSE_TO_POS_2_5C (0x5 << 0) /* 0101: +2.5°C */
#define PIXPAPER_TSE_TO_POS_3C (0x6 << 0) /* 0110: +3°C */
#define PIXPAPER_TSE_TO_POS_3_5C (0x7 << 0) /* 0111: +3.5°C */
#define PIXPAPER_TSE_TO_NEG_4C (0x8 << 0) /* 1000: -4°C */
#define PIXPAPER_TSE_TO_NEG_3_5C (0x9 << 0) /* 1001: -3.5°C */
#define PIXPAPER_TSE_TO_NEG_3C (0xA << 0) /* 1010: -3°C */
#define PIXPAPER_TSE_TO_NEG_2_5C (0xB << 0) /* 1011: -2.5°C */
#define PIXPAPER_TSE_TO_NEG_2C (0xC << 0) /* 1100: -2°C */
#define PIXPAPER_TSE_TO_NEG_1_5C (0xD << 0) /* 1101: -1.5°C */
#define PIXPAPER_TSE_TO_NEG_1C (0xE << 0) /* 1110: -1°C */
#define PIXPAPER_TSE_TO_NEG_0_5C (0xF << 0) /* 1111: -0.5°C */
#define PIXPAPER_TSE_TO_FINE_MASK \
	(0x3 << 4) /* Bits 5-4: TO[5:4], fine adjustment for positive offsets */
#define PIXPAPER_TSE_TO_FINE_0C (0x0 << 4) /* 00: +0.0°C (default) */
#define PIXPAPER_TSE_TO_FINE_0_25C (0x1 << 4) /* 01: +0.25°C */
#define PIXPAPER_TSE_ENABLE \
	(0 << 7) /* Bit 7: TSE, 0=internal sensor enabled (default), 1=disabled (external) */
#define PIXPAPER_TSE_DISABLE \
	(1 << 7) /* Bit 7: TSE, 1=internal sensor disabled, use external */
#define PIXPAPER_TSE_CONFIG                                 \
	(PIXPAPER_TSE_TO_POS_0C | PIXPAPER_TSE_TO_FINE_0C | \
	 PIXPAPER_TSE_ENABLE) /* 0x00: Internal sensor enabled, +0°C offset */

/* R4DH */
#define PIXPAPER_UNKNOWN_4D_CONFIG \
	0x78 /* This value is essential for initialization, derived from userspace examples. */

/* R50H CDI - VCOM and DATA Interval Setting Register */
/* First Parameter */
#define PIXPAPER_CDI_INTERVAL_MASK \
	(0xF << 0) /* Bits 3-0: CDI[3:0], VCOM and data interval (hsync) */
#define PIXPAPER_CDI_17_HSYNC (0x0 << 0) /* 0000: 17 hsync */
#define PIXPAPER_CDI_16_HSYNC (0x1 << 0) /* 0001: 16 hsync */
#define PIXPAPER_CDI_15_HSYNC (0x2 << 0) /* 0010: 15 hsync */
#define PIXPAPER_CDI_14_HSYNC (0x3 << 0) /* 0011: 14 hsync */
#define PIXPAPER_CDI_13_HSYNC (0x4 << 0) /* 0100: 13 hsync */
#define PIXPAPER_CDI_12_HSYNC (0x5 << 0) /* 0101: 12 hsync */
#define PIXPAPER_CDI_11_HSYNC (0x6 << 0) /* 0110: 11 hsync */
#define PIXPAPER_CDI_10_HSYNC (0x7 << 0) /* 0111: 10 hsync (default) */
#define PIXPAPER_CDI_9_HSYNC (0x8 << 0) /* 1000: 9 hsync */
#define PIXPAPER_CDI_8_HSYNC (0x9 << 0) /* 1001: 8 hsync */
#define PIXPAPER_CDI_7_HSYNC (0xA << 0) /* 1010: 7 hsync */
#define PIXPAPER_CDI_6_HSYNC (0xB << 0) /* 1011: 6 hsync */
#define PIXPAPER_CDI_5_HSYNC (0xC << 0) /* 1100: 5 hsync */
#define PIXPAPER_CDI_4_HSYNC (0xD << 0) /* 1101: 4 hsync */
#define PIXPAPER_CDI_3_HSYNC (0xE << 0) /* 1110: 3 hsync */
#define PIXPAPER_CDI_2_HSYNC (0xF << 0) /* 1111: 2 hsync */
#define PIXPAPER_CDI_DDX \
	(1 << 4) /* Bit 4: DDX, 0=grayscale mapping 0, 1=grayscale mapping 1 (default) */
#define PIXPAPER_CDI_VBD_MASK \
	(0x7 << 5) /* Bits 7-5: VBD[2:0], border data selection */
#define PIXPAPER_CDI_VBD_FLOAT (0x0 << 5) /* 000: Floating (DDX=0 or 1) */
#define PIXPAPER_CDI_VBD_GRAY3_DDX0 \
	(0x1 << 5) /* 001: Gray3 (border_buf=011) when DDX=0 */
#define PIXPAPER_CDI_VBD_GRAY2_DDX0 \
	(0x2 << 5) /* 010: Gray2 (border_buf=010) when DDX=0 */
#define PIXPAPER_CDI_VBD_GRAY1_DDX0 \
	(0x3 << 5) /* 011: Gray1 (border_buf=001) when DDX=0 */
#define PIXPAPER_CDI_VBD_GRAY0_DDX0 \
	(0x4 << 5) /* 100: Gray0 (border_buf=000) when DDX=0 */
#define PIXPAPER_CDI_VBD_GRAY0_DDX1 \
	(0x0 << 5) /* 000: Gray0 (border_buf=000) when DDX=1 */
#define PIXPAPER_CDI_VBD_GRAY1_DDX1 \
	(0x1 << 5) /* 001: Gray1 (border_buf=001) when DDX=1 */
#define PIXPAPER_CDI_VBD_GRAY2_DDX1 \
	(0x2 << 5) /* 010: Gray2 (border_buf=010) when DDX=1 */
#define PIXPAPER_CDI_VBD_GRAY3_DDX1 \
	(0x3 << 5) /* 011: Gray3 (border_buf=011) when DDX=1 */
#define PIXPAPER_CDI_VBD_FLOAT_DDX1 (0x4 << 5) /* 100: Floating when DDX=1 */
#define PIXPAPER_CDI_CONFIG                         \
	(PIXPAPER_CDI_10_HSYNC | PIXPAPER_CDI_DDX | \
	 PIXPAPER_CDI_VBD_GRAY1_DDX1) /* 0x37: 10 hsync, DDX=1, border Gray1 */

/* R60H */
#define PIXPAPER_UNKNOWN_60_CONFIG1 \
	0x02 /* This value is essential for initialization, derived from userspace examples. */
#define PIXPAPER_UNKNOWN_60_CONFIG2 \
	0x02 /* This value is essential for initialization, derived from userspace examples. */

/* R61H TRES - Resolution Setting Register */
#define PIXPAPER_TRES_HRES_H                  \
	((PIXPAPER_PANEL_BUFFER_WIDTH >> 8) & \
	 0xFF) /* HRES[9:8]: High byte of horizontal resolution (128) */
#define PIXPAPER_TRES_HRES_L           \
	(PIXPAPER_PANEL_BUFFER_WIDTH & \
	 0xFF) /* HRES[7:0]: Low byte of horizontal resolution (128 = 0x80) */
#define PIXPAPER_TRES_VRES_H      \
	((PIXPAPER_HEIGHT >> 8) & \
	 0xFF) /* VRES[9:8]: High byte of vertical resolution (250) */
#define PIXPAPER_TRES_VRES_L \
	(PIXPAPER_HEIGHT &   \
	 0xFF) /* VRES[7:0]: Low byte of vertical resolution (250 = 0xFA) */

/* R65H GSST - Gate/Source Start Setting Register */
#define PIXPAPER_GSST_S_START 0x00 /* S_Start[7:0]: First source line (S0) */
#define PIXPAPER_GSST_RESERVED 0x00 /* Reserved byte */
#define PIXPAPER_GSST_G_START_H \
	0x00 /* G_Start[8]: High bit of first gate line (G0) */
#define PIXPAPER_GSST_G_START_L \
	0x00 /* G_Start[7:0]: Low byte of first gate line (G0) */

/* RB4H */
#define PIXPAPER_UNKNOWN_B4_CONFIG \
	0xD0 /* This value is essential for initialization, derived from userspace examples. */

/* RB5H */
#define PIXPAPER_UNKNOWN_B5_CONFIG \
	0x03 /* This value is essential for initialization, derived from userspace examples. */

/* RE0H */
#define PIXPAPER_UNKNOWN_E0_CONFIG \
	0x00 /* This value is essential for initialization, derived from userspace examples. */

/* RE3H PWS - Power Saving Register */
/* First Parameter */
#define PIXPAPER_PWS_VCOM_W_MASK \
	(0xF                     \
	 << 4) /* Bits 7-4: VCOM_W[3:0], VCOM power-saving width (line periods) */
#define PIXPAPER_PWS_VCOM_W_0 (0x0 << 4) /* 0000: 0 line periods */
#define PIXPAPER_PWS_VCOM_W_1 (0x1 << 4) /* 0001: 1 line period */
#define PIXPAPER_PWS_VCOM_W_2 (0x2 << 4) /* 0010: 2 line periods */
#define PIXPAPER_PWS_VCOM_W_3 (0x3 << 4) /* 0011: 3 line periods */
#define PIXPAPER_PWS_VCOM_W_4 (0x4 << 4) /* 0100: 4 line periods */
#define PIXPAPER_PWS_VCOM_W_5 (0x5 << 4) /* 0101: 5 line periods */
#define PIXPAPER_PWS_VCOM_W_6 (0x6 << 4) /* 0110: 6 line periods */
#define PIXPAPER_PWS_VCOM_W_7 (0x7 << 4) /* 0111: 7 line periods */
#define PIXPAPER_PWS_VCOM_W_8 (0x8 << 4) /* 1000: 8 line periods */
#define PIXPAPER_PWS_VCOM_W_9 (0x9 << 4) /* 1001: 9 line periods */
#define PIXPAPER_PWS_VCOM_W_10 (0xA << 4) /* 1010: 10 line periods */
#define PIXPAPER_PWS_VCOM_W_11 (0xB << 4) /* 1011: 11 line periods */
#define PIXPAPER_PWS_VCOM_W_12 (0xC << 4) /* 1100: 12 line periods */
#define PIXPAPER_PWS_VCOM_W_13 (0xD << 4) /* 1101: 13 line periods */
#define PIXPAPER_PWS_VCOM_W_14 (0xE << 4) /* 1110: 14 line periods */
#define PIXPAPER_PWS_VCOM_W_15 (0xF << 4) /* 1111: 15 line periods */
#define PIXPAPER_PWS_SD_W_MASK \
	(0xF << 0) /* Bits 3-0: SD_W[3:0], source power-saving width (660 ns units) */
#define PIXPAPER_PWS_SD_W_0 (0x0 << 0) /* 0000: 0 ns */
#define PIXPAPER_PWS_SD_W_1 (0x1 << 0) /* 0001: 660 ns */
#define PIXPAPER_PWS_SD_W_2 (0x2 << 0) /* 0010: 1320 ns */
#define PIXPAPER_PWS_SD_W_3 (0x3 << 0) /* 0011: 1980 ns */
#define PIXPAPER_PWS_SD_W_4 (0x4 << 0) /* 0100: 2640 ns */
#define PIXPAPER_PWS_SD_W_5 (0x5 << 0) /* 0101: 3300 ns */
#define PIXPAPER_PWS_SD_W_6 (0x6 << 0) /* 0110: 3960 ns */
#define PIXPAPER_PWS_SD_W_7 (0x7 << 0) /* 0111: 4620 ns */
#define PIXPAPER_PWS_SD_W_8 (0x8 << 0) /* 1000: 5280 ns */
#define PIXPAPER_PWS_SD_W_9 (0x9 << 0) /* 1001: 5940 ns */
#define PIXPAPER_PWS_SD_W_10 (0xA << 0) /* 1010: 6600 ns */
#define PIXPAPER_PWS_SD_W_11 (0xB << 0) /* 1011: 7260 ns */
#define PIXPAPER_PWS_SD_W_12 (0xC << 0) /* 1100: 7920 ns */
#define PIXPAPER_PWS_SD_W_13 (0xD << 0) /* 1101: 8580 ns */
#define PIXPAPER_PWS_SD_W_14 (0xE << 0) /* 1110: 9240 ns */
#define PIXPAPER_PWS_SD_W_15 (0xF << 0) /* 1111: 9900 ns */
#define PIXPAPER_PWS_CONFIG      \
	(PIXPAPER_PWS_VCOM_W_2 | \
	 PIXPAPER_PWS_SD_W_2) /* 0x22: VCOM 2 line periods (160 µs), source 1320 ns */

/* RE7H */
#define PIXPAPER_UNKNOWN_E7_CONFIG \
	0x1C /* This value is essential for initialization, derived from userspace examples. */

/* RE9H */
#define PIXPAPER_UNKNOWN_E9_CONFIG \
	0x01 /* This value is essential for initialization, derived from userspace examples. */

#endif /* __PIXPAPER_REGS_H__ */
