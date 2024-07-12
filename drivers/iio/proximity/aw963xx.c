// SPDX-License-Identifier: GPL-2.0
/*
 * AWINIC sar sensor driver (aw963xx)
 *
 * Author: Shuaijie Wang<wangshuaijie@awinic.com>
 *
 * Copyright (c) 2024 awinic Technology CO., LTD
 */
#include "aw_sar.h"

#define AW963XX_I2C_NAME "aw963xx_sar"
#define AW963XX_CHANNEL_NUM_MAX				(12)
#define AW963XX_VALID_TH				(2)
#define AW963XX_DATA_PROCESS_FACTOR			(1024)
#define AW963XX_SAR_VCC_MIN_UV				(1700000)
#define AW963XX_SAR_VCC_MAX_UV				(3600000)
#define AW963XX_STEP_LEN_UNSIGNED_CAP_ROUGH_ADJ		(9900)
#define AW963XX_STEP_LEN_UNSIGNED_CAP_FINE_ADJ		(152)
#define AW963XX_STEP_LEN_UNSIGNED_CAP_ENLARGE		(10000)
#define AW963XX_CPU_OSC_CTRL_MASK			(1)
#define ONE_WORD					(0xff)
#define AW_BIT8						(8)
#define AW_BIT16					(16)
#define AW96303						("AW96303")
#define AW96305						("AW96305")
#define AW96305BFOR					("AW96305BFOR")
#define AW96308						("AW96308")
#define AW96310						("AW96310")
#define AW963XX_FILTCTRL0_CHX_REFAEN			(16)
#define AW963XX_FILTCTRL0_CHX_REFASEL			(11)
#define AW963XX_FILTCTRL0_CHX_REFBEN			(9)
#define AW963XX_FILTCTRL0_CHX_REFBSEL			(4)
#define AW963XX_REF_EN					(1)
#define REG_SA_RSTNALL					(0xFF0C)
#define AW963XX_SOFT_RST_EN				(0)
#define AW963XX_CHIP_INIT_MAX_TIME_MS			(30)

#define AFE_BASE_ADDR					(0x0000)
#define DSP_BASE_ADDR					(0x0000)
#define STAT_BASE_ADDR					(0x0000)
#define DATA_BASE_ADDR					(0x0000)
#define SFR_BASE_ADDR					(0x0000)
#define HIDDEN_BASE_ADDR				(0x0000)
#define REG_STAT0					((0x0020) + STAT_BASE_ADDR)
#define REG_STAT1					((0x0024) + STAT_BASE_ADDR)
#define REG_FWVER					((0x005C) + STAT_BASE_ADDR)
#define REG_DIFF_CH0					((0x01EC) + DATA_BASE_ADDR)
#define REG_DIFF_CH1					((0x0304) + DATA_BASE_ADDR)
#define REG_FILTCTRL0_CH0				((0x0130) + DSP_BASE_ADDR)
#define REG_FILTCTRL0_CH1				((0x0248) + DSP_BASE_ADDR)
#define REG_SCANCTRL1					((0x0004) + AFE_BASE_ADDR)
#define REG_AFESOFTCFG0_CH0				((0x010C) + AFE_BASE_ADDR)
#define REG_AFECFG1_CH0					((0x0118) + AFE_BASE_ADDR)
#define REG_AFECFG3_CH0					((0x0120) + AFE_BASE_ADDR)
#define REG_AFESOFTCFG0_CH1				((0x0224) + AFE_BASE_ADDR)
#define REG_AFECFG1_CH1					((0x0230) + AFE_BASE_ADDR)
#define REG_AFECFG3_CH1					((0x0238) + AFE_BASE_ADDR)
#define REG_AFECFG1_M_CH0				((0x10F8) + HIDDEN_BASE_ADDR)
#define REG_AFECFG1_M_CH1				((0x1184) + HIDDEN_BASE_ADDR)
#define REG_IRQSRC					((0xF080) + SFR_BASE_ADDR)
#define REG_IRQEN					((0xF084) + SFR_BASE_ADDR)
#define REG_CHIPSTAT					((0xFF00) + SFR_BASE_ADDR)
#define REG_CHIP_ID0					((0xFF10) + SFR_BASE_ADDR)
#define	REG_CMD						((0xf008) + SFR_BASE_ADDR)
#define REG_AFECFG2_CH0					((0x011c) + AFE_BASE_ADDR)
#define REG_AFECFG2_CH1					((0x0234) + AFE_BASE_ADDR)

enum aw963xx_cap_mode {
	AW963XX_UNSIGNED_CAP = 0,
	AW963XX_SIGNED_CAP = 4,
	AW963XX_MUTUAL_CAP = 5,
};

enum aw963xx_cs_2_irq {
	AW963XX_CS2_IRQ = 2,
	AW963XX_CS5_IRQ = 5,
};

enum aw963xx_operation_mode {
	AW963XX_ACTIVE_MODE = 0x01,
	AW963XX_SLEEP_MODE = 0x02,
	AW963XX_DEEPSLEEP_MODE = 0x03,
};

enum aw963xx_chip_id {
	AW96303_CHIP_ID = 0xA9630340,
	AW96305_CHIP_ID = 0xA9630520,
	AW96305BFOR_CHIP_ID = 0xA9630500,
	AW96308_CHIP_ID = 0xA9630810,
	AW96310_CHIP_ID = 0xA9631010,
};

struct aw963xx {
	unsigned int irq_mux;
	unsigned int ref_ch_en[AW963XX_CHANNEL_NUM_MAX];
	void *p_aw_sar;
	unsigned int last_blfilta[AW963XX_CHANNEL_NUM_MAX];
	unsigned int last_irq_en;
};

struct reg_data {
	unsigned char rw;
	unsigned short reg;
};
#define REG_NONE_ACCESS					(0)
#define REG_RD_ACCESS					(1 << 0)
#define REG_WR_ACCESS					(1 << 1)
static const struct reg_data g_aw963xx_reg_access[] = {
	{ .reg = REG_SCANCTRL1,			.rw = REG_RD_ACCESS | REG_WR_ACCESS, },
	{ .reg = REG_AFESOFTCFG0_CH0,		.rw = REG_RD_ACCESS | REG_WR_ACCESS, },
	{ .reg = REG_AFECFG1_CH0,		.rw = REG_RD_ACCESS | REG_WR_ACCESS, },
	{ .reg = REG_AFECFG3_CH0,		.rw = REG_RD_ACCESS | REG_WR_ACCESS, },
	{ .reg = REG_AFESOFTCFG0_CH1,		.rw = REG_RD_ACCESS | REG_WR_ACCESS, },
	{ .reg = REG_AFECFG1_CH1,		.rw = REG_RD_ACCESS | REG_WR_ACCESS, },
	{ .reg = REG_AFECFG3_CH1,		.rw = REG_RD_ACCESS | REG_WR_ACCESS, },
	{ .reg = REG_STAT0,			.rw = REG_RD_ACCESS, },
	{ .reg = REG_STAT1,			.rw = REG_RD_ACCESS, },
	{ .reg = REG_FWVER,			.rw = REG_RD_ACCESS, },
	{ .reg = REG_DIFF_CH0,			.rw = REG_RD_ACCESS, },
	{ .reg = REG_DIFF_CH1,			.rw = REG_RD_ACCESS, },
	{ .reg = REG_FILTCTRL0_CH0,		.rw = REG_RD_ACCESS | REG_WR_ACCESS, },
	{ .reg = REG_FILTCTRL0_CH1,		.rw = REG_RD_ACCESS | REG_WR_ACCESS, },
	{ .reg = REG_AFECFG1_M_CH0,		.rw = REG_RD_ACCESS | REG_WR_ACCESS, },
	{ .reg = REG_AFECFG1_M_CH1,		.rw = REG_RD_ACCESS | REG_WR_ACCESS, },
	{ .reg = REG_IRQSRC,			.rw = REG_RD_ACCESS, },
	{ .reg = REG_IRQEN,			.rw = REG_RD_ACCESS | REG_WR_ACCESS, },
	{ .reg = REG_CHIPSTAT,			.rw = REG_RD_ACCESS, },
	{ .reg = REG_CHIP_ID0,			.rw = REG_RD_ACCESS, },
};

static const unsigned int aw963xx_reg_default[] = {
	0x0000, 0x000000FF,
	0x0004, 0x00000FFF,
	0x0008, 0x00000FFF,
	0x000C, 0x00000064,
	0x0010, 0x0000705F,
	0x0014, 0x80000000,
	0x0060, 0x00000000,
	0x0064, 0x00000000,
	0x0068, 0x00000000,
	0x006C, 0x00000000,
	0x0070, 0x00000000,
	0x0074, 0x00000000,
	0x0078, 0x00000000,
	0x007C, 0x00000000,
	0x0080, 0x00000000,
	0x0084, 0x00000000,
	0x0088, 0x00000000,
	0x008C, 0x00000000,
	0x0168, 0x00000000,
	0x016C, 0x003F0000,
	0x0170, 0x00050100,
	0x0174, 0x00000000,
	0x0178, 0x341C9207,
	0x017C, 0x00008000,
	0x0180, 0x00000909,
	0x0184, 0x00000001,
	0x0188, 0x00000000,
	0x0318, 0x00000000,
	0x031C, 0x003F0000,
	0x0320, 0x00050100,
	0x0324, 0x00000000,
	0x0328, 0x341C9207,
	0x032C, 0x00008000,
	0x0330, 0x00000909,
	0x0334, 0x00000008,
	0x0338, 0x00000000,
	0x04C8, 0x00000000,
	0x04CC, 0x003F0000,
	0x04D0, 0x00050100,
	0x04D4, 0x00000000,
	0x04D8, 0x341C9207,
	0x04DC, 0x00008000,
	0x04E0, 0x00000909,
	0x04E4, 0x00000040,
	0x04E8, 0x00000000,
	0x0678, 0x00000000,
	0x067C, 0x003F0000,
	0x0680, 0x00050100,
	0x0684, 0x00000000,
	0x0688, 0x341C9207,
	0x068C, 0x00008000,
	0x0690, 0x00000909,
	0x0694, 0x00000200,
	0x0698, 0x00000000,
	0x0828, 0x00000000,
	0x082C, 0x003F0000,
	0x0830, 0x00050100,
	0x0834, 0x00000000,
	0x0838, 0x341C9207,
	0x083C, 0x00008000,
	0x0840, 0x00000909,
	0x0844, 0x00001000,
	0x0848, 0x00000000,
	0x09D8, 0x00000000,
	0x09DC, 0x003F0000,
	0x09E0, 0x00050100,
	0x09E4, 0x00000000,
	0x09E8, 0x341C9207,
	0x09EC, 0x00008000,
	0x09F0, 0x00000909,
	0x09F4, 0x00008000,
	0x09F8, 0x00000000,
	0x0B88, 0x00000000,
	0x0B8C, 0x003F0000,
	0x0B90, 0x00050100,
	0x0B94, 0x00000000,
	0x0B98, 0x341C9207,
	0x0B9C, 0x00008000,
	0x0BA0, 0x00000909,
	0x0BA4, 0x00040000,
	0x0BA8, 0x00000000,
	0x0D38, 0x00000000,
	0x0D3C, 0x003F0000,
	0x0D40, 0x00050100,
	0x0D44, 0x00000000,
	0x0D48, 0x341C9207,
	0x0D4C, 0x00008000,
	0x0D50, 0x00000909,
	0x0D54, 0x00200000,
	0x0D58, 0x00000000,
	0x0EE8, 0x00000000,
	0x0EEC, 0x003F0000,
	0x0EF0, 0x00050100,
	0x0EF4, 0x00000000,
	0x0EF8, 0x341C9207,
	0x0EFC, 0x00008000,
	0x0F00, 0x00000909,
	0x0F04, 0x00000000,
	0x0F08, 0x00000000,
	0x1098, 0x00000000,
	0x109C, 0x003F0000,
	0x10A0, 0x00050100,
	0x10A4, 0x00000000,
	0x10A8, 0x341C9207,
	0x10AC, 0x00008000,
	0x10B0, 0x00000909,
	0x10B4, 0x00000000,
	0x10B8, 0x00000000,
	0x1248, 0x00000000,
	0x124C, 0x003F0000,
	0x1250, 0x00050100,
	0x1254, 0x00000000,
	0x1258, 0x341C9207,
	0x125C, 0x00008000,
	0x1260, 0x00000909,
	0x1264, 0x00000000,
	0x1268, 0x00000000,
	0x13F8, 0x00000000,
	0x13FC, 0x003F0000,
	0x1400, 0x00050100,
	0x1404, 0x00000000,
	0x1408, 0x341C9207,
	0x140C, 0x00008000,
	0x1410, 0x00000909,
	0x1414, 0x00000000,
	0x1418, 0x00000000,
	0x018C, 0xE0400000,
	0x0190, 0x00000000,
	0x0194, 0x00000000,
	0x0198, 0x000A0000,
	0x019C, 0x000008D2,
	0x01A0, 0x00000000,
	0x01A4, 0x00000040,
	0x01A8, 0x000186A0,
	0x01AC, 0x00030D40,
	0x01B0, 0x00061A80,
	0x01B4, 0x000C3500,
	0x01B8, 0x00000000,
	0x01BC, 0x00000000,
	0x01C0, 0x00000000,
	0x01C4, 0x00000000,
	0x01C8, 0x00000000,
	0x01CC, 0x00000000,
	0x033C, 0xE0400000,
	0x0340, 0x00000000,
	0x0344, 0x00000000,
	0x0348, 0x000A0000,
	0x034C, 0x000008D2,
	0x0350, 0x00000000,
	0x0354, 0x00000040,
	0x0358, 0x000186A0,
	0x035C, 0x00030D40,
	0x0360, 0x00061A80,
	0x0364, 0x000C3500,
	0x0368, 0x00000000,
	0x036C, 0x00000000,
	0x0370, 0x00000000,
	0x0374, 0x00000000,
	0x0378, 0x00000000,
	0x037C, 0x00000000,
	0x04EC, 0xE0400000,
	0x04F0, 0x00000000,
	0x04F4, 0x00000000,
	0x04F8, 0x000A0000,
	0x04FC, 0x000008D2,
	0x0500, 0x00000000,
	0x0504, 0x00000040,
	0x0508, 0x000186A0,
	0x050C, 0x00030D40,
	0x0510, 0x00061A80,
	0x0514, 0x000C3500,
	0x0518, 0x00000000,
	0x051C, 0x00000000,
	0x0520, 0x00000000,
	0x0524, 0x00000000,
	0x0528, 0x00000000,
	0x052C, 0x00000000,
	0x069C, 0xE0400000,
	0x06A0, 0x00000000,
	0x06A4, 0x00000000,
	0x06A8, 0x000A0000,
	0x06AC, 0x000008D2,
	0x06B0, 0x00000000,
	0x06B4, 0x00000040,
	0x06B8, 0x000186A0,
	0x06BC, 0x00030D40,
	0x06C0, 0x00061A80,
	0x06C4, 0x000C3500,
	0x06C8, 0x00000000,
	0x06CC, 0x00000000,
	0x06D0, 0x00000000,
	0x06D4, 0x00000000,
	0x06D8, 0x00000000,
	0x06DC, 0x00000000,
	0x084C, 0xE0400000,
	0x0850, 0x00000000,
	0x0854, 0x00000000,
	0x0858, 0x000A0000,
	0x085C, 0x000008D2,
	0x0860, 0x00000000,
	0x0864, 0x00000040,
	0x0868, 0x000186A0,
	0x086C, 0x00030D40,
	0x0870, 0x00061A80,
	0x0874, 0x000C3500,
	0x0878, 0x00000000,
	0x087C, 0x00000000,
	0x0880, 0x00000000,
	0x0884, 0x00000000,
	0x0888, 0x00000000,
	0x088C, 0x00000000,
	0x09FC, 0xE0400000,
	0x0A00, 0x00000000,
	0x0A04, 0x00000000,
	0x0A08, 0x000A0000,
	0x0A0C, 0x000008D2,
	0x0A10, 0x00000000,
	0x0A14, 0x00000040,
	0x0A18, 0x000186A0,
	0x0A1C, 0x00030D40,
	0x0A20, 0x00061A80,
	0x0A24, 0x000C3500,
	0x0A28, 0x00000000,
	0x0A2C, 0x00000000,
	0x0A30, 0x00000000,
	0x0A34, 0x00000000,
	0x0A38, 0x00000000,
	0x0A3C, 0x00000000,
	0x0BAC, 0xE0400000,
	0x0BB0, 0x00000000,
	0x0BB4, 0x00000000,
	0x0BB8, 0x000A0000,
	0x0BBC, 0x000008D2,
	0x0BC0, 0x00000000,
	0x0BC4, 0x00000040,
	0x0BC8, 0x000186A0,
	0x0BCC, 0x00030D40,
	0x0BD0, 0x00061A80,
	0x0BD4, 0x000C3500,
	0x0BD8, 0x00000000,
	0x0BDC, 0x00000000,
	0x0BE0, 0x00000000,
	0x0BE4, 0x00000000,
	0x0BE8, 0x00000000,
	0x0BEC, 0x00000000,
	0x0D5C, 0xE0400000,
	0x0D60, 0x00000000,
	0x0D64, 0x00000000,
	0x0D68, 0x000A0000,
	0x0D6C, 0x000008D2,
	0x0D70, 0x00000000,
	0x0D74, 0x00000040,
	0x0D78, 0x000186A0,
	0x0D7C, 0x00030D40,
	0x0D80, 0x00061A80,
	0x0D84, 0x000C3500,
	0x0D88, 0x00000000,
	0x0D8C, 0x00000000,
	0x0D90, 0x00000000,
	0x0D94, 0x00000000,
	0x0D98, 0x00000000,
	0x0D9C, 0x00000000,
	0x0F0C, 0xE0400000,
	0x0F10, 0x00000000,
	0x0F14, 0x00000000,
	0x0F18, 0x000A0000,
	0x0F1C, 0x000008D2,
	0x0F20, 0x00000000,
	0x0F24, 0x00000040,
	0x0F28, 0x000186A0,
	0x0F2C, 0x00000000,
	0x0F30, 0x00000000,
	0x0F34, 0x00000000,
	0x0F38, 0x00000000,
	0x0F3C, 0x00000000,
	0x0F40, 0x00000000,
	0x0F44, 0x00000000,
	0x0F48, 0x00000000,
	0x0F4C, 0x00000000,
	0x10BC, 0xE0400000,
	0x10C0, 0x00000000,
	0x10C4, 0x00000000,
	0x10C8, 0x000A0000,
	0x10CC, 0x000008D2,
	0x10D0, 0x00000000,
	0x10D4, 0x00000040,
	0x10D8, 0x00000000,
	0x10DC, 0x00000000,
	0x10E0, 0x00000000,
	0x10E4, 0x00000000,
	0x10E8, 0x00000000,
	0x10EC, 0x00000000,
	0x10F0, 0x00000000,
	0x10F4, 0x00000000,
	0x10F8, 0x00000000,
	0x10FC, 0x00000000,
	0x126C, 0xE0400000,
	0x1270, 0x00000000,
	0x1274, 0x00000000,
	0x1278, 0x000A0000,
	0x127C, 0x000008D2,
	0x1280, 0x00000000,
	0x1284, 0x00000040,
	0x1288, 0x00000000,
	0x128C, 0x00000000,
	0x1290, 0x00000000,
	0x1294, 0x00000000,
	0x1298, 0x00000000,
	0x129C, 0x00000000,
	0x12A0, 0x00000000,
	0x12A4, 0x00000000,
	0x12A8, 0x00000000,
	0x12AC, 0x00000000,
	0x141C, 0xE0400000,
	0x1420, 0x00000000,
	0x1424, 0x00000000,
	0x1428, 0x000A0000,
	0x142C, 0x000008D2,
	0x1430, 0x00000000,
	0x1434, 0x00000040,
	0x1438, 0x00000000,
	0x143C, 0x00000000,
	0x1440, 0x00000000,
	0x1444, 0x00000000,
	0x1448, 0x00000000,
	0x144C, 0x00000000,
	0x1450, 0x00000000,
	0x1454, 0x00000000,
	0x1458, 0x00000000,
	0x145C, 0x00000000,
	0x01D0, 0x00000000,
	0x01D4, 0x00000000,
	0x01D8, 0x00000000,
	0x01DC, 0x00000000,
	0x01E0, 0x00000000,
	0x01E4, 0xFFFFFFFF,
	0x01E8, 0x00000000,
	0x01EC, 0x00000000,
	0x01F0, 0x00000000,
	0x01F4, 0x00000000,
	0x01F8, 0x070004B0,
	0x01FC, 0x0E000000,
	0x0200, 0xF2000000,
	0x0204, 0x02000000,
	0x0208, 0x02000000,
	0x020C, 0x00002000,
	0x0210, 0x00010000,
	0x0214, 0x80007530,
	0x0220, 0x00000000,
	0x0224, 0x00000000,
	0x0380, 0x00000000,
	0x0384, 0x00000000,
	0x0388, 0x00000000,
	0x038C, 0x00000000,
	0x0390, 0x00000000,
	0x0394, 0xFFFFFFFF,
	0x0398, 0x00000000,
	0x039C, 0x00000000,
	0x03A0, 0x00000000,
	0x03A4, 0x00000000,
	0x03A8, 0x070004B0,
	0x03AC, 0x0E000000,
	0x03B0, 0xF2000000,
	0x03B4, 0x02000000,
	0x03B8, 0x02000000,
	0x03BC, 0x00002000,
	0x03C0, 0x00010000,
	0x03C4, 0x80007530,
	0x03D0, 0x00000000,
	0x03D4, 0x00000000,
	0x0530, 0x00000000,
	0x0534, 0x00000000,
	0x0538, 0x00000000,
	0x053C, 0x00000000,
	0x0540, 0x00000000,
	0x0544, 0xFFFFFFFF,
	0x0548, 0x00000000,
	0x054C, 0x00000000,
	0x0550, 0x00000000,
	0x0554, 0x00000000,
	0x0558, 0x070004B0,
	0x055C, 0x0E000000,
	0x0560, 0xF2000000,
	0x0564, 0x02000000,
	0x0568, 0x02000000,
	0x056C, 0x00002000,
	0x0570, 0x00010000,
	0x0574, 0x80007530,
	0x0580, 0x00000000,
	0x0584, 0x00000000,
	0x06E0, 0x00000000,
	0x06E4, 0x00000000,
	0x06E8, 0x00000000,
	0x06EC, 0x00000000,
	0x06F0, 0x00000000,
	0x06F4, 0xFFFFFFFF,
	0x06F8, 0x00000000,
	0x06FC, 0x00000000,
	0x0700, 0x00000000,
	0x0704, 0x00000000,
	0x0708, 0x070004B0,
	0x070C, 0x0E000000,
	0x0710, 0xF2000000,
	0x0714, 0x02000000,
	0x0718, 0x02000000,
	0x071C, 0x00002000,
	0x0720, 0x00010000,
	0x0724, 0x80007530,
	0x0730, 0x00000000,
	0x0734, 0x00000000,
	0x0890, 0x00000000,
	0x0894, 0x00000000,
	0x0898, 0x00000000,
	0x089C, 0x00000000,
	0x08A0, 0x00000000,
	0x08A4, 0xFFFFFFFF,
	0x08A8, 0x00000000,
	0x08AC, 0x00000000,
	0x08B0, 0x00000000,
	0x08B4, 0x00000000,
	0x08B8, 0x070004B0,
	0x08BC, 0x0E000000,
	0x08C0, 0xF2000000,
	0x08C4, 0x02000000,
	0x08C8, 0x02000000,
	0x08CC, 0x00002000,
	0x08D0, 0x00010000,
	0x08D4, 0x80007530,
	0x08E0, 0x00000000,
	0x08E4, 0x00000000,
	0x0A40, 0x00000000,
	0x0A44, 0x00000000,
	0x0A48, 0x00000000,
	0x0A4C, 0x00000000,
	0x0A50, 0x00000000,
	0x0A54, 0xFFFFFFFF,
	0x0A58, 0x00000000,
	0x0A5C, 0x00000000,
	0x0A60, 0x00000000,
	0x0A64, 0x00000000,
	0x0A68, 0x070004B0,
	0x0A6C, 0x0E000000,
	0x0A70, 0xF2000000,
	0x0A74, 0x02000000,
	0x0A78, 0x02000000,
	0x0A7C, 0x00002000,
	0x0A80, 0x00010000,
	0x0A84, 0x80007530,
	0x0A90, 0x00000000,
	0x0A94, 0x00000000,
	0x0BF0, 0x00000000,
	0x0BF4, 0x00000000,
	0x0BF8, 0x00000000,
	0x0BFC, 0x00000000,
	0x0C00, 0x00000000,
	0x0C04, 0xFFFFFFFF,
	0x0C08, 0x00000000,
	0x0C0C, 0x00000000,
	0x0C10, 0x00000000,
	0x0C14, 0x00000000,
	0x0C18, 0x070004B0,
	0x0C1C, 0x0E000000,
	0x0C20, 0xF2000000,
	0x0C24, 0x02000000,
	0x0C28, 0x02000000,
	0x0C2C, 0x00002000,
	0x0C30, 0x00010000,
	0x0C34, 0x80007530,
	0x0C40, 0x00000000,
	0x0C44, 0x00000000,
	0x0DA0, 0x00000000,
	0x0DA4, 0x00000000,
	0x0DA8, 0x00000000,
	0x0DAC, 0x00000000,
	0x0DB0, 0x00000000,
	0x0DB4, 0xFFFFFFFF,
	0x0DB8, 0x00000000,
	0x0DBC, 0x00000000,
	0x0DC0, 0x00000000,
	0x0DC4, 0x00000000,
	0x0DC8, 0x070004B0,
	0x0DCC, 0x0E000000,
	0x0DD0, 0xF2000000,
	0x0DD4, 0x02000000,
	0x0DD8, 0x02000000,
	0x0DDC, 0x00002000,
	0x0DE0, 0x00010000,
	0x0DE4, 0x80007530,
	0x0DF0, 0x00000000,
	0x0DF4, 0x00000000,
	0x0F50, 0x00000000,
	0x0F54, 0x00000000,
	0x0F58, 0x00000000,
	0x0F5C, 0x00000000,
	0x0F60, 0x00000000,
	0x0F64, 0xFFFFFFFF,
	0x0F68, 0x00000000,
	0x0F6C, 0x00000000,
	0x0F70, 0x00000000,
	0x0F74, 0x00000000,
	0x0F78, 0x070004B0,
	0x0F7C, 0x0E000000,
	0x0F80, 0xF2000000,
	0x0F84, 0x02000000,
	0x0F88, 0x02000000,
	0x0F8C, 0x00002000,
	0x0F90, 0x00010000,
	0x0F94, 0x80007530,
	0x0FA0, 0x00000000,
	0x0FA4, 0x00000000,
	0x1100, 0x00000000,
	0x1104, 0x00000000,
	0x1108, 0x00000000,
	0x110C, 0x00000000,
	0x1110, 0x00000000,
	0x1114, 0xFFFFFFFF,
	0x1118, 0x00000000,
	0x111C, 0x00000000,
	0x1120, 0x00000000,
	0x1124, 0x00000000,
	0x1128, 0x070004B0,
	0x112C, 0x0E000000,
	0x1130, 0xF2000000,
	0x1134, 0x02000000,
	0x1138, 0x02000000,
	0x113C, 0x00002000,
	0x1140, 0x00010000,
	0x1144, 0x80007530,
	0x1150, 0x00000000,
	0x1154, 0x00000000,
	0x12B0, 0x00000000,
	0x12B4, 0x00000000,
	0x12B8, 0x00000000,
	0x12BC, 0x00000000,
	0x12C0, 0x00000000,
	0x12C4, 0xFFFFFFFF,
	0x12C8, 0x00000000,
	0x12CC, 0x00000000,
	0x12D0, 0x00000000,
	0x12D4, 0x00000000,
	0x12D8, 0x070004B0,
	0x12DC, 0x0E000000,
	0x12E0, 0xF2000000,
	0x12E4, 0x02000000,
	0x12E8, 0x02000000,
	0x12EC, 0x00002000,
	0x12F0, 0x00010000,
	0x12F4, 0x80007530,
	0x1300, 0x00000000,
	0x1304, 0x00000000,
	0x1460, 0x00000000,
	0x1464, 0x00000000,
	0x1468, 0x00000000,
	0x146C, 0x00000000,
	0x1470, 0x00000000,
	0x1474, 0xFFFFFFFF,
	0x1478, 0x00000000,
	0x147C, 0x00000000,
	0x1480, 0x00000000,
	0x1484, 0x00000000,
	0x1488, 0x070004B0,
	0x148C, 0x0E000000,
	0x1490, 0xF2000000,
	0x1494, 0x02000000,
	0x1498, 0x02000000,
	0x149C, 0x00002000,
	0x14A0, 0x00010000,
	0x14A4, 0x80007530,
	0x14B0, 0x00000000,
	0x14B4, 0x00000000,
	0xF084, 0x00000006,
	0x004C, 0xFFFFFFFF,
	0x0050, 0xFFFFFFFF,
	0x0054, 0xFFFFFFFF,
	0x0058, 0xFFFFFFFF,
	0x0090, 0x00000000,
	0x0094, 0x00000064,
	0x0098, 0x40000000,
	0x009C, 0x00000000,
	0x00A0, 0x00140014,
	0x00A4, 0x0019000D,
	0x00A8, 0x0096004B,
};

static void aw963xx_set_cs_as_irq(struct aw_sar *p_sar, int flag);

static int aw963xx_load_reg_bin(struct aw_bin *aw_bin, void *load_bin_para)
{
	struct aw_sar *p_sar = (struct aw_sar *)load_bin_para;
	struct aw963xx *aw963xx = (struct aw963xx *)p_sar->priv_data;
	int ret;

	ret = strncmp(p_sar->chip_name, aw_bin->header_info[0].chip_type,
			sizeof(aw_bin->header_info[0].chip_type));
	if (ret != 0)
		dev_err(p_sar->dev, "load_binname(%s) incompatible with chip type(%s)",
			p_sar->chip_name, aw_bin->header_info[0].chip_type);

	p_sar->load_bin.bin_data_ver = aw_bin->header_info[0].bin_data_ver;

	ret = aw_sar_load_reg(aw_bin, p_sar->i2c);

	if (!strncmp(p_sar->chip_name, AW96308, sizeof(AW96308)) ||
		!strncmp(p_sar->chip_name, AW96305BFOR, sizeof(AW96305BFOR)))
		aw963xx_set_cs_as_irq(p_sar, aw963xx->irq_mux);

	return ret;
}

static void aw963xx_irq_handle_func(unsigned int irq_status, void *data)
{
	struct aw_sar *p_sar = (struct aw_sar *)data;
	unsigned int ch_th[AW963XX_CHANNEL_NUM_MAX] = { 0 };
	unsigned int curr_status_val[4] = { 0 };
	unsigned char i;
	unsigned char j;

	if (((irq_status & 0x01) == 1) && (p_sar->driver_code_initover_flag == 1))
		p_sar->fault_flag = AW_SAR_UNHEALTHY;

	for (i = 0; i < AW963XX_VALID_TH; i++)
		aw_sar_i2c_read(p_sar->i2c, REG_STAT0 + i * (REG_STAT1 - REG_STAT0),
				&curr_status_val[i]);

	for (j = 0; j < AW963XX_CHANNEL_NUM_MAX; j++) {
		for (i = 0; i < AW963XX_VALID_TH; i++)
			ch_th[j] |= ((curr_status_val[i] >> j) & 0x01) << i;

		if (p_sar->channels_arr[j].last_channel_info != ch_th[j]) {
			if (ch_th[j] == 0) {
				/* far */
				iio_push_event(p_sar->aw_iio_dev,
						IIO_UNMOD_EVENT_CODE(IIO_PROXIMITY, j,
							IIO_EV_TYPE_THRESH, IIO_EV_DIR_RISING),
						iio_get_time_ns(p_sar->aw_iio_dev));
			} else {
				/* near */
				iio_push_event(p_sar->aw_iio_dev,
						IIO_UNMOD_EVENT_CODE(IIO_PROXIMITY, j,
							IIO_EV_TYPE_THRESH, IIO_EV_DIR_FALLING),
						iio_get_time_ns(p_sar->aw_iio_dev));
			}
			p_sar->channels_arr[j].last_channel_info = ch_th[j];
		}
	}
}

static ssize_t aw963xx_operation_mode_get(void *data, char *buf)
{
	struct aw_sar *p_sar = (struct aw_sar *)data;
	ssize_t len = 0;

	if (p_sar->last_mode == AW963XX_ACTIVE_MODE)
		len += snprintf(buf + len, PAGE_SIZE - len, "operation mode: Active\n");
	else if (p_sar->last_mode == AW963XX_SLEEP_MODE)
		len += snprintf(buf + len, PAGE_SIZE - len, "operation mode: Sleep\n");
	else if (p_sar->last_mode == AW963XX_DEEPSLEEP_MODE)
		len += snprintf(buf + len, PAGE_SIZE - len, "operation mode: DeepSleep\n");
	else
		len += snprintf(buf + len, PAGE_SIZE - len, "operation mode: Unconfirmed\n");

	return len;
}

static void aw963xx_sar_chip_info_get(void *data, char *buf, ssize_t *p_len)
{
	struct aw_sar *p_sar = (struct aw_sar *)data;
	unsigned int reg_data;

	*p_len += snprintf(buf + *p_len, PAGE_SIZE - *p_len,
			"sar%u\n", p_sar->dts_info.sar_num);
	*p_len += snprintf(buf + *p_len, PAGE_SIZE - *p_len, "The driver supports UI\n");

	aw_sar_i2c_read(p_sar->i2c, REG_CHIP_ID0, &reg_data);
	*p_len += snprintf(buf + *p_len, PAGE_SIZE - *p_len, "chipid is 0x%08x\n", reg_data);

	aw_sar_i2c_read(p_sar->i2c, REG_IRQEN, &reg_data);
	*p_len += snprintf(buf + *p_len, PAGE_SIZE - *p_len, "REG_HOSTIRQEN is 0x%08x\n", reg_data);

	*p_len += snprintf(buf + *p_len, PAGE_SIZE - *p_len, "aw963xx Bin data version:0x%08x\n",
			p_sar->load_bin.bin_data_ver);
}

static int aw963xx_get_signed_cap(void *data, unsigned short reg_addr)
{
	struct aw_sar *p_sar = (struct aw_sar *)data;
	int s_ofst_c = 0;
	unsigned int off_m_bit;
	unsigned int off_c_bit;
	unsigned int reg_data;
	unsigned int off_c;
	unsigned int off_m;
	int off_f;
	unsigned int i;

	aw_sar_i2c_read(p_sar->i2c, reg_addr, &reg_data);

	off_f = ((reg_data >> AW_BIT16) & ONE_WORD) * AW963XX_STEP_LEN_UNSIGNED_CAP_FINE_ADJ;
	off_c = (reg_data >> AW_BIT8) & ONE_WORD;
	off_m = reg_data & ONE_WORD;

	for (i = 0; i < 8; i++) {
		off_m_bit = (off_m >> i) & 0x01;
		off_c_bit = (off_c >> i) & 0x01;
		s_ofst_c += ((1 - 2 * off_m_bit) * off_c_bit * aw_sar_pow2(i)) *
			AW963XX_STEP_LEN_UNSIGNED_CAP_ROUGH_ADJ;
	}

	return (s_ofst_c + off_f);
}

static unsigned int aw963xx_get_unsigned_cap(void *data, unsigned short reg_addr)
{
	struct aw_sar *p_sar = (struct aw_sar *)data;
	unsigned int reg_data;
	unsigned int rough;
	unsigned int fine;

	aw_sar_i2c_read(p_sar->i2c, reg_addr, &reg_data);

	rough = ((reg_data >> AW_BIT8) & ONE_WORD) * AW963XX_STEP_LEN_UNSIGNED_CAP_ROUGH_ADJ;
	fine = ((reg_data >> AW_BIT16) & ONE_WORD) * AW963XX_STEP_LEN_UNSIGNED_CAP_FINE_ADJ;

	return (rough + fine);
}

static void aw963xx_get_ref_ch_enable(struct aw_sar *p_sar)
{
	struct aw963xx *aw963xx = (struct aw963xx *)p_sar->priv_data;
	unsigned int refa_ch;
	unsigned int refb_ch;
	unsigned int reg_data;
	int i;

	for (i = 0; i < AW963XX_CHANNEL_NUM_MAX; i++) {
		aw_sar_i2c_read(p_sar->i2c,
				REG_FILTCTRL0_CH0 +
				i * (REG_FILTCTRL0_CH1 - REG_FILTCTRL0_CH0),
				&reg_data);
		if ((reg_data >> AW963XX_FILTCTRL0_CHX_REFAEN) & 0x01) {
			refa_ch = (reg_data >> AW963XX_FILTCTRL0_CHX_REFASEL) & 0x1f;
			aw963xx->ref_ch_en[refa_ch] = AW963XX_REF_EN;
		}
		if ((reg_data >> AW963XX_FILTCTRL0_CHX_REFBEN) & 0x01) {
			refb_ch = (reg_data >> AW963XX_FILTCTRL0_CHX_REFBSEL) & 0x1f;
			aw963xx->ref_ch_en[refb_ch] = AW963XX_REF_EN;
		}
	}
}

/* Note: Because the kernel cannot handle floating-point types, it expands mul by 10 times */
static unsigned char aw963xx_get_offset_multiple(struct aw_sar *p_sar, unsigned char ch)
{
	unsigned int reg_data;
	unsigned char mul = 1;

	aw_sar_i2c_read(p_sar->i2c, REG_AFECFG2_CH0 + ch * (REG_AFECFG2_CH1 - REG_AFECFG2_CH0),
			&reg_data);
	if ((reg_data >> 27) & 0x1) {
		if (((reg_data >> 29) & 0x3) == 0)
			mul = 16;
		else if (((reg_data >> 29) & 0x3) == 1)
			mul = 20;
		else if (((reg_data >> 29) & 0x3) == 2)
			mul = 26;
		else if (((reg_data >> 29) & 0x3) == 3)
			mul = 40;
		return mul;
	}

	aw_sar_i2c_read(p_sar->i2c, REG_AFECFG3_CH0 +
			ch * (REG_AFECFG3_CH1 - REG_AFECFG3_CH0),
			&reg_data);
	if ((reg_data >> 11) & 0x1)
		mul = 20;
	else
		mul = 10;

	return mul;
}

static ssize_t aw963xx_get_cap_offset(void *data, char *buf)
{
	struct aw_sar *p_sar = (struct aw_sar *)data;
	struct aw963xx *aw963xx = (struct aw963xx *)p_sar->priv_data;
	int signed_cap_ofst;
	unsigned int mode = 0xff;
	unsigned int reg_data;
	unsigned int cap_ofst;
	unsigned char mul = 10;
	ssize_t len = 0;
	unsigned int tmp;
	unsigned int i;

	aw963xx_get_ref_ch_enable(p_sar);

	for (i = 0; i < AW963XX_CHANNEL_NUM_MAX; i++) {
		aw_sar_i2c_read(p_sar->i2c,
				REG_AFESOFTCFG0_CH0 +
				i * (REG_AFESOFTCFG0_CH1 - REG_AFESOFTCFG0_CH0),
				&reg_data);
		mul = aw963xx_get_offset_multiple(p_sar, i);
		mode = reg_data & 0x0ff;
		switch (mode) {
		case AW963XX_UNSIGNED_CAP:	/* self-capacitance mode unsigned cail */
			cap_ofst = aw963xx_get_unsigned_cap(p_sar,
				REG_AFECFG1_CH0 + i * (REG_AFECFG1_CH1 - REG_AFECFG1_CH0));
		/*
		 * Because it has been expanded by 1000 times before,
		 * the accuracy of removing mul's expansion loss can be ignored
		 */
			cap_ofst = cap_ofst * mul / 10;
			len += snprintf(buf + len, PAGE_SIZE - len,
					"unsigned cap ofst ch%u: %u.%u pf\r\n",
					i,
					cap_ofst / AW963XX_STEP_LEN_UNSIGNED_CAP_ENLARGE,
					cap_ofst % AW963XX_STEP_LEN_UNSIGNED_CAP_ENLARGE);
			break;
		case AW963XX_SIGNED_CAP:	/* self-capacitance mode signed cail */
			signed_cap_ofst = aw963xx_get_signed_cap(p_sar,
					REG_AFECFG1_CH0 + i * (REG_AFECFG1_CH1 - REG_AFECFG1_CH0));
			signed_cap_ofst = signed_cap_ofst * mul / 10;
			if (signed_cap_ofst < 0) {
				tmp = -signed_cap_ofst;
				len += snprintf(buf + len, PAGE_SIZE - len,
						"signed cap ofst ch%u: -%u.%upf\r\n",
						i,
						tmp / AW963XX_STEP_LEN_UNSIGNED_CAP_ENLARGE,
						tmp % AW963XX_STEP_LEN_UNSIGNED_CAP_ENLARGE);
			} else {
				len += snprintf(buf + len, PAGE_SIZE - len,
						"signed cap ofst ch%u: %d.%dpf\r\n",
						i,
						signed_cap_ofst /
						AW963XX_STEP_LEN_UNSIGNED_CAP_ENLARGE,
						signed_cap_ofst %
						AW963XX_STEP_LEN_UNSIGNED_CAP_ENLARGE);
			}
			break;
		case AW963XX_MUTUAL_CAP:	/* mutual-capacitance mode */
			if (aw963xx->ref_ch_en[i] == AW963XX_REF_EN) {
				cap_ofst = aw963xx_get_unsigned_cap(p_sar,
					REG_AFECFG1_M_CH0 +
					i * (REG_AFECFG1_M_CH1 - REG_AFECFG1_M_CH0));
				cap_ofst = cap_ofst * mul / 10;
				len += snprintf(buf + len, PAGE_SIZE - len,
						"ref unsigned cap ofst ch%u: %u.%udpf\r\n",
						i,
						cap_ofst / AW963XX_STEP_LEN_UNSIGNED_CAP_ENLARGE,
						cap_ofst % AW963XX_STEP_LEN_UNSIGNED_CAP_ENLARGE);
			} else {
				signed_cap_ofst = aw963xx_get_signed_cap(p_sar,
							REG_AFECFG1_CH0 +
							i * (REG_AFECFG1_CH1 - REG_AFECFG1_CH0));
				signed_cap_ofst = signed_cap_ofst * mul / 10;
				if (signed_cap_ofst < 0) {
					tmp = -signed_cap_ofst;
					len += snprintf(buf + len, PAGE_SIZE - len,
							"mutual cap ofst ch%u: -%u.%udpf\r\n",
							i,
							tmp / AW963XX_STEP_LEN_UNSIGNED_CAP_ENLARGE,
							tmp % AW963XX_STEP_LEN_UNSIGNED_CAP_ENLARGE);
				} else {
					len += snprintf(buf + len, PAGE_SIZE - len,
							"mutual cap ofst ch%u: %d.%dpf\r\n",
							i,
							signed_cap_ofst /
							AW963XX_STEP_LEN_UNSIGNED_CAP_ENLARGE,
							signed_cap_ofst %
							AW963XX_STEP_LEN_UNSIGNED_CAP_ENLARGE);
				}
			}
			break;
		default:
			break;
		}
	}

	return len;
}

static void aw963xx_set_cs_as_irq(struct aw_sar *p_sar, int flag)
{
	if (flag == AW963XX_CS2_IRQ) {
		aw_sar_i2c_write(p_sar->i2c, 0xfff4, 0x3c00d013);
		aw_sar_i2c_write(p_sar->i2c, 0xc100, 0x00000020);
		aw_sar_i2c_write(p_sar->i2c, 0xe018, 0x00000004);
	} else if (flag == AW963XX_CS5_IRQ) {
		aw_sar_i2c_write(p_sar->i2c, 0xfff4, 0x3c00d013);
		aw_sar_i2c_write(p_sar->i2c, 0xc100, 0x00000800);
		aw_sar_i2c_write(p_sar->i2c, 0xe018, 0x00000020);
	} else {
		aw_sar_i2c_write(p_sar->i2c, 0xfff4, 0x3c00d013);
		aw_sar_i2c_write(p_sar->i2c, 0xc100, 0x00000000);
		aw_sar_i2c_write(p_sar->i2c, 0xe018, 0x00000000);
	}
}

int aw963xx_check_chipid(void *data)
{
	struct aw_sar *p_sar = (struct aw_sar *)data;
	unsigned int reg_val;
	int ret;

	if (!p_sar)
		return -EINVAL;

	ret = aw_sar_i2c_read(p_sar->i2c, REG_CHIP_ID0, &reg_val);
	if (ret < 0) {
		dev_err(p_sar->dev, "read CHIP ID failed: %d", ret);
		return ret;
	}

	switch (reg_val) {
	case AW96303_CHIP_ID:
		memcpy(p_sar->chip_name, AW96303, 8);
		ret = 0;
		break;
	case AW96305_CHIP_ID:
		memcpy(p_sar->chip_name, AW96305, 8);
		ret = 0;
		break;
	case AW96305BFOR_CHIP_ID:
		memcpy(p_sar->chip_name, AW96305BFOR, 8);
		ret = 0;
		break;
	case AW96308_CHIP_ID:
		memcpy(p_sar->chip_name, AW96308, 8);
		ret = 0;
		break;
	case AW96310_CHIP_ID:
		memcpy(p_sar->chip_name, AW96310, 8);
		ret = 0;
		break;
	default:
		dev_err(p_sar->dev, "chip id error, 0x%04x", reg_val);
		ret = -EIO;
		break;
	}

	return ret;
}
EXPORT_SYMBOL_NS_GPL(aw963xx_check_chipid, AWINIC_PROX);

static void aw963xx_enable_clock(void *i2c)
{
	aw_sar_i2c_write_bits(i2c, REG_CHIPSTAT, ~AW963XX_CPU_OSC_CTRL_MASK,
			AW963XX_CPU_OSC_CTRL_MASK);
}

static unsigned int aw963xx_rc_irqscr(void *i2c)
{
	unsigned int val;

	aw_sar_i2c_read(i2c, REG_IRQSRC, &val);

	return val;
}

static void aw963xx_set_active_cmd(void *i2c)
{
	aw_sar_i2c_write(i2c, REG_CMD, AW963XX_ACTIVE_MODE);
}

static void aw963xx_set_sleep_cmd(void *i2c)
{
	aw_sar_i2c_write(i2c, REG_CMD, AW963XX_SLEEP_MODE);
}

static void aw963xx_set_deepsleep_cmd(void *i2c)
{
	aw_sar_i2c_write(i2c, REG_CMD, AW963XX_DEEPSLEEP_MODE);
}

static const struct aw_sar_mode_set_t g_aw963xx_mode_set[] = {
	{
		.chip_id = AW_SAR_NONE_CHECK_CHIP,
		.chip_mode = {
			.curr_mode = AW963XX_ACTIVE_MODE,
			.last_mode = AW963XX_DEEPSLEEP_MODE,
		},
		.mode_switch_ops = {
			.enable_clock = aw963xx_enable_clock,
			.rc_irqscr = NULL,
			.mode_update = aw963xx_set_active_cmd,
		},
	},
	{
		.chip_id = AW_SAR_NONE_CHECK_CHIP,
		.chip_mode = {
			.curr_mode = AW963XX_ACTIVE_MODE,
			.last_mode = AW963XX_SLEEP_MODE,
		},
		.mode_switch_ops = {
			.enable_clock = NULL,
			.rc_irqscr = NULL,
			.mode_update = aw963xx_set_active_cmd,
		},
	},
	{
		.chip_id = AW_SAR_NONE_CHECK_CHIP,
		.chip_mode = {
			.curr_mode = AW963XX_ACTIVE_MODE,
			.last_mode = AW963XX_ACTIVE_MODE,
		},
		.mode_switch_ops = {
			.enable_clock = NULL,
			.rc_irqscr = NULL,
			.mode_update = aw963xx_set_active_cmd,
		},
	},
	{
		.chip_id = AW_SAR_NONE_CHECK_CHIP,
		.chip_mode = {
			.curr_mode = AW963XX_SLEEP_MODE,
			.last_mode = AW963XX_DEEPSLEEP_MODE,
		},
		.mode_switch_ops = {
			.enable_clock = aw963xx_enable_clock,
			.rc_irqscr = aw963xx_rc_irqscr,
			.mode_update = aw963xx_set_sleep_cmd,
		},
	},
	{
		.chip_id = AW_SAR_NONE_CHECK_CHIP,
		.chip_mode = {
			.curr_mode = AW963XX_SLEEP_MODE,
			.last_mode = AW963XX_ACTIVE_MODE,
		},
		.mode_switch_ops = {
			.enable_clock = NULL,
			.rc_irqscr = aw963xx_rc_irqscr,
			.mode_update = aw963xx_set_sleep_cmd,
		},
	},
	{
		.chip_id = AW_SAR_NONE_CHECK_CHIP,
		.chip_mode = {
			.curr_mode = AW963XX_DEEPSLEEP_MODE,
			.last_mode = AW963XX_SLEEP_MODE,
		},
		.mode_switch_ops = {
			.enable_clock = NULL,
			.rc_irqscr = aw963xx_rc_irqscr,
			.mode_update = aw963xx_set_deepsleep_cmd,
		},
	},
	{
		.chip_id = AW_SAR_NONE_CHECK_CHIP,
		.chip_mode = {
			.curr_mode = AW963XX_DEEPSLEEP_MODE,
			.last_mode = AW963XX_ACTIVE_MODE,
		},
		.mode_switch_ops = {
			.enable_clock = NULL,
			.rc_irqscr = aw963xx_rc_irqscr,
			.mode_update = aw963xx_set_deepsleep_cmd,
		},
	},
};

static int aw963xx_parse_dts(void *data)
{
	struct aw_sar *p_sar = (struct aw_sar *)data;
	struct aw963xx *aw963xx = (struct aw963xx *)p_sar->priv_data;
	struct device_node *np = p_sar->i2c->dev.of_node;
	int val;

	val = of_property_read_u32(np, "awinic,irq-mux", &aw963xx->irq_mux);
	if (val != 0)
		dev_err(p_sar->dev, "irq-mux not detected");

	return 0;
}

static const struct aw_sar_mode_t g_aw963xx_mode = {
	.mode_set_arr = &g_aw963xx_mode_set[0],
	.mode_set_arr_len = ARRAY_SIZE(g_aw963xx_mode_set),
	.p_set_mode_node_fn = NULL,
	.p_get_mode_node_fn = aw963xx_operation_mode_get,
};

static const struct aw_sar_diff_t g_aw963xx_diff = {
	.diff0_reg = REG_DIFF_CH0,
	.diff_step = REG_DIFF_CH1 - REG_DIFF_CH0,
	.rm_float = AW963XX_DATA_PROCESS_FACTOR,
	.p_get_diff_node_fn = NULL,
};

static const struct aw_sar_offset_t g_aw963xx_offset = {
	.p_get_offset_node_fn = aw963xx_get_cap_offset,
};

static const struct aw_sar_aot_t g_aw963xx_aot = {
	.aot_reg = REG_SCANCTRL1,
	.aot_mask = ~0xfff,
	.aot_flag = 0xfff,
};

static const struct aw_sar_para_load_t g_aw963xx_reg_arr_para = {
	.reg_arr = aw963xx_reg_default,
	.reg_arr_len = ARRAY_SIZE(aw963xx_reg_default),
};

static const struct aw_sar_regulator_config_t g_regulator_config = {
	.vcc_name = "vcc",
	.min_uV = AW963XX_SAR_VCC_MIN_UV,
	.max_uV = AW963XX_SAR_VCC_MAX_UV,
};

static const struct aw_sar_reg_list_t g_aw963xx_reg_list = {
	.reg_none_access = REG_NONE_ACCESS,
	.reg_rd_access = REG_RD_ACCESS,
	.reg_wd_access = REG_WR_ACCESS,
	.reg_perm = (struct aw_sar_reg_data *)&g_aw963xx_reg_access[0],
	.reg_num = ARRAY_SIZE(g_aw963xx_reg_access),
};

static const struct aw_sar_chip_mode_t g_aw963xx_chip_mode = {
	.init_mode = AW963XX_ACTIVE_MODE,
	.active = AW963XX_ACTIVE_MODE,
	.pre_init_mode = AW963XX_SLEEP_MODE,
};

static const struct aw_sar_load_bin_t g_aw963xx_load_reg_bin = {
	.bin_name = "aw963xx_reg",
	.bin_opera_func = aw963xx_load_reg_bin,
	.p_update_fn = NULL,
};

static const struct aw_sar_get_chip_info_t g_aw963xx_get_chip_info = {
	.p_get_chip_info_node_fn = aw963xx_sar_chip_info_get,
};

static const struct aw_sar_check_chipid_t g_aw963xx_check_chipid = {
	.p_check_chipid_fn = aw963xx_check_chipid,
};

static const struct aw_sar_irq_init_t g_aw963xx_irq_init = {
	.flags = GPIOF_DIR_IN | GPIOF_INIT_HIGH,
	.irq_flags = IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
	.handler = NULL,
	.thread_fn = NULL,
	.rc_irq_fn = aw963xx_rc_irqscr,
	.irq_spec_handler_fn = aw963xx_irq_handle_func,

	.p_irq_init_fn = NULL,
	.p_irq_deinit_fn = NULL,
};

static const struct aw_sar_soft_rst_t g_aw963xx_soft_rst = {
	.reg_rst = REG_SA_RSTNALL,
	.reg_rst_val = AW963XX_SOFT_RST_EN,
	.delay_ms = AW963XX_CHIP_INIT_MAX_TIME_MS,
	.p_soft_reset_fn = NULL,
};

static const struct aw_sar_init_over_irq_t g_aw963xx_init_over_irq = {
	.wait_times = 100,
	.daley_step = 1,
	.reg_irqsrc = REG_IRQSRC,
	.irq_offset_bit = 0,
	.irq_mask = 0x1,
	.irq_flag = 0x1,

	.p_check_init_over_irq_fn = NULL,
	.p_get_err_type_fn = NULL,
};

static const struct aw_sar_pm_t g_aw963xx_pm_chip_mode = {
	.suspend_set_mode = AW963XX_SLEEP_MODE,
	.resume_set_mode = AW963XX_ACTIVE_MODE,
	.shutdown_set_mode = AW963XX_SLEEP_MODE,
};

static const struct aw_sar_platform_config g_aw963xx_platform_config = {
	.p_add_parse_dts_fn = &aw963xx_parse_dts,
	.p_regulator_config = &g_regulator_config,
	.p_irq_init = &g_aw963xx_irq_init,
	.p_pm_chip_mode = &g_aw963xx_pm_chip_mode,
};

static void aw963xx_power_on_prox_detection(void *data, unsigned char en_flag)
{

}

static const struct aw_sar_power_on_prox_detection_t g_aw933xx_power_on_prox_detection = {
	.p_power_on_prox_detection_en_fn = aw963xx_power_on_prox_detection,
	.irq_en_cali_bit = 3,
	.power_on_prox_en_flag = true,
};

static const struct aw_sar_chip_config g_aw963xx_chip_config = {
	.ch_num_max = AW963XX_CHANNEL_NUM_MAX,
	.p_platform_config = &g_aw963xx_platform_config,

	.p_check_chipid = &g_aw963xx_check_chipid,
	.p_soft_rst = &g_aw963xx_soft_rst,
	.p_init_over_irq = &g_aw963xx_init_over_irq,
	.p_reg_bin = &g_aw963xx_load_reg_bin,
	.p_chip_mode = &g_aw963xx_chip_mode,

	/* Node usage parameters */
	.p_reg_list = &g_aw963xx_reg_list,
	.p_reg_arr = &g_aw963xx_reg_arr_para,
	.p_aot = &g_aw963xx_aot,
	.p_diff = &g_aw963xx_diff,
	.p_offset = &g_aw963xx_offset,
	.p_mode = &g_aw963xx_mode,
	.p_get_chip_info = &g_aw963xx_get_chip_info,

	.power_on_prox_detection = &g_aw933xx_power_on_prox_detection,
};

int aw963xx_init(struct aw_sar *p_sar)
{
	if (!p_sar)
		return -EINVAL;

	p_sar->priv_data = devm_kzalloc(p_sar->dev, sizeof(struct aw963xx), GFP_KERNEL);
	if (!p_sar->priv_data)
		return -ENOMEM;

	/* Chip private function operation */
	p_sar->p_sar_para = &g_aw963xx_chip_config;

	return 0;
}
EXPORT_SYMBOL_NS_GPL(aw963xx_init, AWINIC_PROX);

void aw963xx_deinit(struct aw_sar *p_sar)
{
	if ((!p_sar) || (!p_sar->priv_data))
		return;

	if (p_sar->priv_data != NULL)
		devm_kfree(p_sar->dev, p_sar->priv_data);
}
EXPORT_SYMBOL_NS_GPL(aw963xx_deinit, AWINIC_PROX);
