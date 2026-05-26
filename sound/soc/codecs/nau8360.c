// SPDX-License-Identifier: GPL-2.0-only
//
// The NAU83G60 Stereo Class-D Amplifier with DSP and I/V-sense driver.
//
// Copyright (C) 2026 Nuvoton Technology Corp.
// Author: David Lin <ctlin0@nuvoton.com>
//         Seven Lee <wtli@nuvoton.com>
//         John Hsu <kchsu0@nuvoton.com>
//         Neo Chang <ylchang2@nuvoton.com>

#include <linux/acpi.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/units.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "nau8360-dsp.h"
#include "nau8360.h"

static inline void nau8360_dsp_software_reset(struct snd_soc_component *component);
static inline void nau8360_dsp_enable(struct regmap *regmap, bool enable);

/* range of Master Clock MCLK (Hz) */
#define MASTER_CLK_MIN 11025000
#define MASTER_CLK_MAX 24576000
/* DSP Optimal Clock Range 120MHz~126M(Hz) */
#define DSP_OP_CLK48 122880000
#define DSP_OP_CLK44 112896000
/* the maximum frequency of DAC and IV sense clock */
#define CLK_DA_IVSNS_MAX 6144000
#define ADSP_SR_48000 48000
#define ADSP_SR_44100 44100

static const int ivsns_clk_div[] = { 1, 2, 4, 5, 8, 10 };

static const int dac_clk_div[] = { 1, 2, 4, 8 };

static const char * const nau8360_rx_func_names[] = {
	[NAU8360_TDM_DACL] = "DAC_L",
	[NAU8360_TDM_DACR] = "DAC_R",
	[NAU8360_TDM_ANCL] = "ANC_L",
	[NAU8360_TDM_ANCR] = "ANC_R",
};

static const char * const nau8360_tx_func_names[] = {
	[NAU8360_TDM_AECL]  = "AEC_L",
	[NAU8360_TDM_AECR]  = "AEC_R",
	[NAU8360_TDM_ISNSL] = "ISNS_L",
	[NAU8360_TDM_ISNSR] = "ISNS_R",
	[NAU8360_TDM_VSNSL] = "VSNS_L",
	[NAU8360_TDM_VSNSR] = "VSNS_R",
	[NAU8360_TDM_TJ]    = "TJ",
	[NAU8360_TDM_VBAT]  = "VBAT",
};

/* PLL threshold */
#define PLL_FREQ_MIN 1000000
#define PLL_FREQ_MAX 32000000
#define PLL_FOUT_MIN 12500000
#define PLL_FOUT_MAX 125000000
#define PLL_FREF_MAX 8000000
#define PLL_FVCO_MIN 50000000
#define MSEL_MAX 32
#define RSEL_MAX 4

static const struct reg_default nau8360_reg_defaults[] = {
	{ NAU8360_R02_I2C_ADDR, 0x0000 },
	{ NAU8360_R03_CLK_CTRL0, 0x0000 },
	{ NAU8360_R04_CLK_CTRL1, 0x0000 },
	{ NAU8360_R05_INTERRUPT_CTRL, 0x40ff },
	{ NAU8360_R07_GP_CTRL, 0xaa30 },
	{ NAU8360_R08_GP_CTRL0, 0x1e1e },
	{ NAU8360_R09_GP_CTRL1, 0x1e1e },
	{ NAU8360_R0A_GP_CTRL2, 0x0000 },
	{ NAU8360_R0B_I2S_PCM_CTRL1, 0x0702 },
	{ NAU8360_R0C_I2S_PCM_CTRL2, 0x0a10 },
	{ NAU8360_R0D_I2S_PCM_CTRL3, 0x1300 },
	{ NAU8360_R0E_I2S_DATA_CTRL1, 0x0304 },
	{ NAU8360_R0F_I2S_DATA_CTRL2, 0x080b },
	{ NAU8360_R10_I2S_DATA_CTRL3, 0x0014 },
	{ NAU8360_R11_I2S_DATA_CTRL4, 0x0014 },
	{ NAU8360_R12_PATH_CTRL, 0x0400 },
	{ NAU8360_R17_I2S0_DATA_CTRL5, 0x0014 },
	{ NAU8360_R1A_DSP_CORE_CTRL2, 0x0010 },
	{ NAU8360_R2C_ALC_CTRL1, 0x2000 },
	{ NAU8360_R2D_ALC_CTRL2, 0x8400 },
	{ NAU8360_R2E_ALC_CTRL3, 0x2083 },
	{ NAU8360_R31_UVLOP_CTRL1, 0x0000 },
	{ NAU8360_R32_UVLOP_CTRL2, 0x8400 },
	{ NAU8360_R33_UVLOP_CTRL3, 0x0000 },
	{ NAU8360_R40_CLK_DET_CTRL, 0xca60 },
	{ NAU8360_R41_CLK_CTL2, 0xc400 },
	{ NAU8360_R5D_SINC_CFG, 0x0010 },
	{ NAU8360_R5F_ANA_TRIM_CFG1, 0x8400 },
	{ NAU8360_R60_RST, 0x0010 },
	{ NAU8360_R67_ANALOG_CONTROL_0, 0x0160 },
	{ NAU8360_R68_ANALOG_CONTROL_1, 0x00d4 },
	{ NAU8360_R6A_SARADC_CFG0, 0x5c09 },
	{ NAU8360_R6B_SARADC_CFG1, 0x0008 },
	{ NAU8360_R6C_IVSNS_CFG0, 0xf040 },
	{ NAU8360_R6D_IVSNS_CFG1, 0x3555 },
	{ NAU8360_R6E_DAC_CFG0, 0x0ed5 },
	{ NAU8360_R71_CLK_DIV_CFG, 0x0211 },
	{ NAU8360_R72_PLL_CFG0, 0xccc4 },
	{ NAU8360_R73_PLL_CFG1, 0x0101 },
	{ NAU8360_R74_PLL_CFG2, 0x0000 },
	{ NAU8360_R7A_DAC_TRIM_CFG2, 0x0000 },
	{ NAU8360_R7B_IVSNS_TRIM_CFG, 0x0000 },
	{ NAU8360_R7C_MISC_TRIM_CFG, 0x72d5 },
	{ NAU8360_R86_HW3_CTL0, 0x2000 },
	{ NAU8360_R88_ALC_CTRL6, 0x0000 },
	{ NAU8360_R8A_HW3_VL_CTL7, 0xc000 },
	{ NAU8360_R8B_HW3_VR_CTL8, 0xc000 },
	{ NAU8360_R8C_HW3_CTL6, 0x0000 },
	{ NAU8360_R8D_HW3_IL_CTL7, 0xc000 },
	{ NAU8360_R8E_HW3_IR_CTL8, 0xc000 },
	{ NAU8360_R8F_HW3_CTL9, 0x0000 },
	{ NAU8360_R90_HW2_CTL0, 0x2000 },
	{ NAU8360_R96_HW2_CTL6, 0x0000 },
	{ NAU8360_R97_HW2_CTL7, 0xc000 },
	{ NAU8360_R98_HW2_CTL8, 0xc000 },
	{ NAU8360_R99_HW2_CTL9, 0x0000 },
	{ NAU8360_R9A_HW1_CTL0, 0xc000 },
	{ NAU8360_R9B_HW1_CTL1, 0xc000 },
	{ NAU8360_R9C_HW1_CTL2, 0x0800 },
	{ NAU8360_R9D_PEQ_CTL, 0x0001 },
	{ NAU8360_RA0_LEFT_XODRC_CTRL, 0x0000 },
	{ NAU8360_RA2_RIGHT_XODRC_CTRL, 0x0000 },
	{ NAU8360_RA4_ANA_REG_0, 0x7f86 },
	{ NAU8360_RA5_ANA_REG_1, 0x276e },
};

static bool nau8360_readable_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case NAU8360_R00_SOFTWARE_RST ... NAU8360_R12_PATH_CTRL:
	case NAU8360_R17_I2S0_DATA_CTRL5:
	case NAU8360_R1A_DSP_CORE_CTRL2:
	case NAU8360_R21_VBAT_READOUT ... NAU8360_R22_TEMP_READOUT:
	case NAU8360_R2C_ALC_CTRL1 ... NAU8360_R2E_ALC_CTRL3:
	case NAU8360_R31_UVLOP_CTRL1 ... NAU8360_R33_UVLOP_CTRL3:
	case NAU8360_R40_CLK_DET_CTRL ... NAU8360_R41_CLK_CTL2:
	case NAU8360_R46_I2C_DEVICE_ID:
	case NAU8360_R5D_SINC_CFG:
	case NAU8360_R5F_ANA_TRIM_CFG1 ... NAU8360_R60_RST:
	case NAU8360_R67_ANALOG_CONTROL_0 ... NAU8360_R6E_DAC_CFG0:
	case NAU8360_R71_CLK_DIV_CFG ... NAU8360_R74_PLL_CFG2:
	case NAU8360_R77_SOFT_SD ... NAU8360_R7C_MISC_TRIM_CFG:
	case NAU8360_R7E_CLK_GATED_EN:
	case NAU8360_R86_HW3_CTL0:
	case NAU8360_R88_ALC_CTRL6:
	case NAU8360_R8A_HW3_VL_CTL7 ... NAU8360_R90_HW2_CTL0:
	case NAU8360_R96_HW2_CTL6 ... NAU8360_R9D_PEQ_CTL:
	case NAU8360_RA0_LEFT_XODRC_CTRL:
	case NAU8360_RA2_RIGHT_XODRC_CTRL:
	case NAU8360_RA4_ANA_REG_0 ... NAU8360_RA5_ANA_REG_1:
	case NAU8360_R100_LEFT_BIQ0_COE ... (NAU8360_R100_LEFT_BIQ0_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R10C_LEFT_BIQ1_COE ... (NAU8360_R10C_LEFT_BIQ1_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R118_LEFT_BIQ2_COE ... (NAU8360_R118_LEFT_BIQ2_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R124_LEFT_BIQ3_COE ... (NAU8360_R124_LEFT_BIQ3_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R130_LEFT_BIQ4_COE ... (NAU8360_R130_LEFT_BIQ4_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R13C_LEFT_BIQ5_COE ... (NAU8360_R13C_LEFT_BIQ5_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R148_LEFT_BIQ6_COE ... (NAU8360_R148_LEFT_BIQ6_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R154_LEFT_BIQ7_COE ... (NAU8360_R154_LEFT_BIQ7_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R160_LEFT_BIQ8_COE ... (NAU8360_R160_LEFT_BIQ8_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R16C_LEFT_BIQ9_COE ... (NAU8360_R16C_LEFT_BIQ9_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R178_LEFT_BIQ10_COE ... (NAU8360_R178_LEFT_BIQ10_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R184_LEFT_BIQ11_COE ... (NAU8360_R184_LEFT_BIQ11_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R190_LEFT_BIQ12_COE ... (NAU8360_R190_LEFT_BIQ12_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R19C_LEFT_BIQ13_COE ... (NAU8360_R19C_LEFT_BIQ13_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R1A8_LEFT_BIQ14_COE ... (NAU8360_R1A8_LEFT_BIQ14_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R200_RIGHT_BIQ0_COE ... (NAU8360_R200_RIGHT_BIQ0_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R20C_RIGHT_BIQ1_COE ... (NAU8360_R20C_RIGHT_BIQ1_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R218_RIGHT_BIQ2_COE ... (NAU8360_R218_RIGHT_BIQ2_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R224_RIGHT_BIQ3_COE ... (NAU8360_R224_RIGHT_BIQ3_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R230_RIGHT_BIQ4_COE ... (NAU8360_R230_RIGHT_BIQ4_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R23C_RIGHT_BIQ5_COE ... (NAU8360_R23C_RIGHT_BIQ5_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R248_RIGHT_BIQ6_COE ... (NAU8360_R248_RIGHT_BIQ6_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R254_RIGHT_BIQ7_COE ... (NAU8360_R254_RIGHT_BIQ7_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R260_RIGHT_BIQ8_COE ... (NAU8360_R260_RIGHT_BIQ8_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R26C_RIGHT_BIQ9_COE ... (NAU8360_R26C_RIGHT_BIQ9_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R278_RIGHT_BIQ10_COE ... (NAU8360_R278_RIGHT_BIQ10_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R284_RIGHT_BIQ11_COE ... (NAU8360_R284_RIGHT_BIQ11_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R290_RIGHT_BIQ12_COE ... (NAU8360_R290_RIGHT_BIQ12_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R29C_RIGHT_BIQ13_COE ... (NAU8360_R29C_RIGHT_BIQ13_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R2A8_RIGHT_BIQ14_COE ... (NAU8360_R2A8_RIGHT_BIQ14_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_RF000_DSP_COMM:
	case NAU8360_RF002_DSP_COMM:
		return true;
	default:
		return false;
	}

}

static bool nau8360_writeable_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case NAU8360_R00_SOFTWARE_RST ... NAU8360_R12_PATH_CTRL:
	case NAU8360_R17_I2S0_DATA_CTRL5:
	case NAU8360_R1A_DSP_CORE_CTRL2:
	case NAU8360_R2C_ALC_CTRL1 ... NAU8360_R2E_ALC_CTRL3:
	case NAU8360_R31_UVLOP_CTRL1 ... NAU8360_R33_UVLOP_CTRL3:
	case NAU8360_R40_CLK_DET_CTRL ... NAU8360_R41_CLK_CTL2:
	case NAU8360_R5D_SINC_CFG:
	case NAU8360_R5F_ANA_TRIM_CFG1:
	case NAU8360_R60_RST:
	case NAU8360_R67_ANALOG_CONTROL_0 ... NAU8360_R68_ANALOG_CONTROL_1:
	case NAU8360_R6A_SARADC_CFG0 ... NAU8360_R6E_DAC_CFG0:
	case NAU8360_R71_CLK_DIV_CFG ... NAU8360_R74_PLL_CFG2:
	case NAU8360_R77_SOFT_SD ... NAU8360_R7C_MISC_TRIM_CFG:
	case NAU8360_R7E_CLK_GATED_EN:
	case NAU8360_R86_HW3_CTL0:
	case NAU8360_R88_ALC_CTRL6:
	case NAU8360_R8A_HW3_VL_CTL7 ... NAU8360_R90_HW2_CTL0:
	case NAU8360_R96_HW2_CTL6 ... NAU8360_R9D_PEQ_CTL:
	case NAU8360_RA4_ANA_REG_0 ... NAU8360_RA5_ANA_REG_1:
	case NAU8360_R100_LEFT_BIQ0_COE ... (NAU8360_R100_LEFT_BIQ0_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R10C_LEFT_BIQ1_COE ... (NAU8360_R10C_LEFT_BIQ1_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R118_LEFT_BIQ2_COE ... (NAU8360_R118_LEFT_BIQ2_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R124_LEFT_BIQ3_COE ... (NAU8360_R124_LEFT_BIQ3_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R130_LEFT_BIQ4_COE ... (NAU8360_R130_LEFT_BIQ4_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R13C_LEFT_BIQ5_COE ... (NAU8360_R13C_LEFT_BIQ5_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R148_LEFT_BIQ6_COE ... (NAU8360_R148_LEFT_BIQ6_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R154_LEFT_BIQ7_COE ... (NAU8360_R154_LEFT_BIQ7_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R160_LEFT_BIQ8_COE ... (NAU8360_R160_LEFT_BIQ8_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R16C_LEFT_BIQ9_COE ... (NAU8360_R16C_LEFT_BIQ9_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R178_LEFT_BIQ10_COE ... (NAU8360_R178_LEFT_BIQ10_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R184_LEFT_BIQ11_COE ... (NAU8360_R184_LEFT_BIQ11_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R190_LEFT_BIQ12_COE ... (NAU8360_R190_LEFT_BIQ12_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R19C_LEFT_BIQ13_COE ... (NAU8360_R19C_LEFT_BIQ13_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R1A8_LEFT_BIQ14_COE ... (NAU8360_R1A8_LEFT_BIQ14_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R200_RIGHT_BIQ0_COE ... (NAU8360_R200_RIGHT_BIQ0_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R20C_RIGHT_BIQ1_COE ... (NAU8360_R20C_RIGHT_BIQ1_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R218_RIGHT_BIQ2_COE ... (NAU8360_R218_RIGHT_BIQ2_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R224_RIGHT_BIQ3_COE ... (NAU8360_R224_RIGHT_BIQ3_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R230_RIGHT_BIQ4_COE ... (NAU8360_R230_RIGHT_BIQ4_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R23C_RIGHT_BIQ5_COE ... (NAU8360_R23C_RIGHT_BIQ5_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R248_RIGHT_BIQ6_COE ... (NAU8360_R248_RIGHT_BIQ6_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R254_RIGHT_BIQ7_COE ... (NAU8360_R254_RIGHT_BIQ7_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R260_RIGHT_BIQ8_COE ... (NAU8360_R260_RIGHT_BIQ8_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R26C_RIGHT_BIQ9_COE ... (NAU8360_R26C_RIGHT_BIQ9_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R278_RIGHT_BIQ10_COE ... (NAU8360_R278_RIGHT_BIQ10_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R284_RIGHT_BIQ11_COE ... (NAU8360_R284_RIGHT_BIQ11_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R290_RIGHT_BIQ12_COE ... (NAU8360_R290_RIGHT_BIQ12_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R29C_RIGHT_BIQ13_COE ... (NAU8360_R29C_RIGHT_BIQ13_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_R2A8_RIGHT_BIQ14_COE ... (NAU8360_R2A8_RIGHT_BIQ14_COE +
					NAU8360_PEQ_REG_WIDTH):
	case NAU8360_RF000_DSP_COMM:
	case NAU8360_RF002_DSP_COMM:
		return true;
	default:
		return false;
	}
}

static bool nau8360_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case NAU8360_R00_SOFTWARE_RST ... NAU8360_R02_I2C_ADDR:
	case NAU8360_R06_INT_CLR_STATUS:
	case NAU8360_R21_VBAT_READOUT ... NAU8360_R22_TEMP_READOUT:
	case NAU8360_R41_CLK_CTL2:
	case NAU8360_R46_I2C_DEVICE_ID:
	case NAU8360_R69_ANALOG_CONTROL_3:
	case NAU8360_R77_SOFT_SD ... NAU8360_R79_EN_HIRC48M:
	case NAU8360_R7E_CLK_GATED_EN:
	case NAU8360_R9D_PEQ_CTL:
	case NAU8360_RF000_DSP_COMM:
	case NAU8360_RF002_DSP_COMM:
		return true;
	default:
		return false;
	}
}

static const char *const tdm_data_length[] = { "16", "32" };

static const char *const tdm_pdm_length[] = { "16", "32" };

static const char *const tdm_data_n_length[] = { "8", "16" };

static const char *const tdm_tx_slot_text[] = { "Slot 0", "Slot 1", "Slot 2",
	"Slot 3", "Slot 4", "Slot 5", "Slot 6", "Slot 7", "Slot None" };

static const char *const tdm_rx_slot_text[] = { "Slot 0", "Slot 1", "Slot 2",
	"Slot 3", "Slot None"};

static const char *const tdm_pdm_slot_text[] = { "Slot 0", "Slot 1", "Slot 2",
	"Slot 3", "Slot 4", "Slot 5", "Slot 6", "Slot 7" };

static const struct soc_enum nau8360_tdm_dacl_slot_enum =
	SOC_ENUM_SINGLE(NAU8360_R0C_I2S_PCM_CTRL2, 0,
		ARRAY_SIZE(tdm_rx_slot_text), tdm_rx_slot_text);

static const struct soc_enum nau8360_tdm_dacr_slot_enum =
	SOC_ENUM_SINGLE(NAU8360_R0C_I2S_PCM_CTRL2, NAU8360_RX_DACR_SFT,
		ARRAY_SIZE(tdm_rx_slot_text), tdm_rx_slot_text);

static const struct soc_enum nau8360_tdm_ancl_slot_enum =
	SOC_ENUM_SINGLE(NAU8360_R10_I2S_DATA_CTRL3, NAU8360_RX_ANC_L_SFT,
		ARRAY_SIZE(tdm_rx_slot_text), tdm_rx_slot_text);

static const struct soc_enum nau8360_tdm_ancr_slot_enum =
	SOC_ENUM_SINGLE(NAU8360_R10_I2S_DATA_CTRL3, NAU8360_RX_ANC_R_SFT,
		ARRAY_SIZE(tdm_rx_slot_text), tdm_rx_slot_text);

static const struct soc_enum nau8360_aec_data_len_enum =
	SOC_ENUM_DOUBLE(NAU8360_R17_I2S0_DATA_CTRL5, NAU8360_AEC_L_SLEN_SFT,
		NAU8360_AEC_R_SLEN_SFT, ARRAY_SIZE(tdm_data_length), tdm_data_length);

static const struct soc_enum nau8360_aecl_slot_enum =
	SOC_VALUE_ENUM_SINGLE(NAU8360_R17_I2S0_DATA_CTRL5, NAU8360_AEC_L_SLOT_SFT, 0x3f,
		ARRAY_SIZE(tdm_tx_slot_text), tdm_tx_slot_text, NULL);

static const struct soc_enum nau8360_aecr_slot_enum =
	SOC_VALUE_ENUM_SINGLE(NAU8360_R17_I2S0_DATA_CTRL5, 0, 0x3f,
		ARRAY_SIZE(tdm_tx_slot_text), tdm_tx_slot_text, NULL);

static const struct soc_enum nau8360_isnsl_data_len_enum =
	SOC_ENUM_SINGLE(NAU8360_R0E_I2S_DATA_CTRL1, NAU8360_ISNS_L_SLEN_SFT,
		ARRAY_SIZE(tdm_data_n_length), tdm_data_n_length);

static const struct soc_enum nau8360_isnsl_slot_enum =
	SOC_VALUE_ENUM_SINGLE(NAU8360_R0E_I2S_DATA_CTRL1, NAU8360_ISNS_L_SLOT_SFT, 0x3f,
		ARRAY_SIZE(tdm_tx_slot_text), tdm_tx_slot_text, NULL);

static const struct soc_enum nau8360_isnsr_data_len_enum =
	SOC_ENUM_SINGLE(NAU8360_R11_I2S_DATA_CTRL4, NAU8360_ISNS_R_SLEN_SFT,
		ARRAY_SIZE(tdm_data_n_length), tdm_data_n_length);

static const struct soc_enum nau8360_isnsr_slot_enum =
	SOC_VALUE_ENUM_SINGLE(NAU8360_R11_I2S_DATA_CTRL4, 0, 0x3f,
		ARRAY_SIZE(tdm_tx_slot_text), tdm_tx_slot_text, NULL);

static const struct soc_enum nau8360_vsnsl_data_len_enum =
	SOC_ENUM_SINGLE(NAU8360_R0D_I2S_PCM_CTRL3, NAU8360_VSNS_L_SLEN_SFT,
		ARRAY_SIZE(tdm_data_n_length), tdm_data_n_length);

static const struct soc_enum nau8360_vsnsl_slot_enum =
	SOC_VALUE_ENUM_SINGLE(NAU8360_R0D_I2S_PCM_CTRL3, 0, 0x3f,
		ARRAY_SIZE(tdm_tx_slot_text), tdm_tx_slot_text, NULL);

static const struct soc_enum nau8360_vsnsr_data_len_enum =
	SOC_ENUM_SINGLE(NAU8360_R11_I2S_DATA_CTRL4, NAU8360_VSNS_R_SLEN_SFT,
		ARRAY_SIZE(tdm_data_n_length), tdm_data_n_length);

static const struct soc_enum nau8360_vsnsr_slot_enum =
	SOC_VALUE_ENUM_SINGLE(NAU8360_R11_I2S_DATA_CTRL4, NAU8360_VSNS_R_SLOT_SFT, 0x3f,
		ARRAY_SIZE(tdm_tx_slot_text), tdm_tx_slot_text, NULL);

static const struct soc_enum nau8360_temp_slot_enum =
	SOC_VALUE_ENUM_SINGLE(NAU8360_R10_I2S_DATA_CTRL3, 0, 0x3f,
		ARRAY_SIZE(tdm_tx_slot_text), tdm_tx_slot_text, NULL);

static const struct soc_enum nau8360_vbat_slot_enum =
	SOC_VALUE_ENUM_SINGLE(NAU8360_R0F_I2S_DATA_CTRL2, NAU8360_VBAT_SLOT_SFT, 0x3f,
		ARRAY_SIZE(tdm_tx_slot_text), tdm_tx_slot_text, NULL);

static const struct soc_enum nau8360_pdml_data_len_enum =
	SOC_ENUM_SINGLE(NAU8360_R0E_I2S_DATA_CTRL1, NAU8360_PDM_L_SLEN_SFT,
		ARRAY_SIZE(tdm_pdm_length), tdm_pdm_length);

static const struct soc_enum nau8360_pdml_slot_enum =
	SOC_VALUE_ENUM_SINGLE(NAU8360_R0E_I2S_DATA_CTRL1, 0, 0x3f,
		ARRAY_SIZE(tdm_pdm_slot_text), tdm_pdm_slot_text, NULL);

static const struct soc_enum nau8360_pdmr_data_len_enum =
	SOC_ENUM_SINGLE(NAU8360_R0F_I2S_DATA_CTRL2, NAU8360_PDM_R_SLEN_SFT,
		ARRAY_SIZE(tdm_pdm_length), tdm_pdm_length);

static const struct soc_enum nau8360_pdmr_slot_enum =
	SOC_VALUE_ENUM_SINGLE(NAU8360_R0F_I2S_DATA_CTRL2, 0, 0x3f,
		ARRAY_SIZE(tdm_pdm_slot_text), tdm_pdm_slot_text, NULL);

static const char *const tdm_pdm_mode[] = { "SDO=SDI", "HW2 path" };

static const struct soc_enum nau8360_pdm_mode_enum =
	SOC_ENUM_SINGLE(NAU8360_R10_I2S_DATA_CTRL3, NAU8360_TDM_LOOPBACK_SFT,
		ARRAY_SIZE(tdm_pdm_mode), tdm_pdm_mode);

static int nau8360_anc_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *cp = snd_kcontrol_chip(kcontrol);
	struct nau8360 *nau8360 = snd_soc_component_get_drvdata(cp);
	int ret, value = 8;

	ret = snd_soc_put_volsw(kcontrol, ucontrol);
	/* update anc flag if return value 1 and register value changed */
	if (ret != 1)
		goto done;

	nau8360->anc_enable = ucontrol->value.integer.value[0];
	if (nau8360->dsp_enable)
		value = nau8360->anc_enable ? NAU8360_PEQ_BAND_15 : NAU8360_PEQ_BAND_12;
	regmap_update_bits(nau8360->regmap, NAU8360_R9D_PEQ_CTL, NAU8360_PEQ_BAND_MASK,
		value << NAU8360_PEQ_BAND_SFT);

done:
	return ret;
}

static inline int nau8360_get_tdm_tx_func_idx(struct soc_enum *e)
{
	switch (e->reg) {
	case NAU8360_R17_I2S0_DATA_CTRL5:
		return (e->shift_l == NAU8360_AEC_L_SLOT_SFT) ?
			NAU8360_TDM_AECL : NAU8360_TDM_AECR;

	case NAU8360_R0E_I2S_DATA_CTRL1:
		if (e->shift_l == NAU8360_ISNS_L_SLOT_SFT)
			return NAU8360_TDM_ISNSL;
		break;

	case NAU8360_R11_I2S_DATA_CTRL4:
		return (e->shift_l == NAU8360_VSNS_R_SLOT_SFT) ?
			NAU8360_TDM_VSNSR : NAU8360_TDM_ISNSR;

	case NAU8360_R0D_I2S_PCM_CTRL3:
		return NAU8360_TDM_VSNSL;

	case NAU8360_R10_I2S_DATA_CTRL3:
		return NAU8360_TDM_TJ;

	case NAU8360_R0F_I2S_DATA_CTRL2:
		if (e->shift_l == NAU8360_VBAT_SLOT_SFT)
			return NAU8360_TDM_VBAT;
		break;
	}

	return -EINVAL;
}

static inline int nau8360_get_tdm_rx_func_idx(struct soc_enum *e)
{
	switch (e->reg) {
	case NAU8360_R0C_I2S_PCM_CTRL2:
		return (e->shift_l == NAU8360_RX_DACR_SFT) ?
			NAU8360_TDM_DACR : NAU8360_TDM_DACL;

	case NAU8360_R10_I2S_DATA_CTRL3:
		return (e->shift_l == NAU8360_RX_ANC_R_SFT) ?
			NAU8360_TDM_ANCR : NAU8360_TDM_ANCL;
	}

	return -EINVAL;
}

/* TDM TX slot maps according to TDM channel length */
static inline void compute_slotx_scale(struct snd_soc_component *cp, int *scale)
{
	int value = snd_soc_component_read(cp, NAU8360_R0C_I2S_PCM_CTRL2);

	value = (value & NAU8360_TDM_CLEN_MASK) >> NAU8360_TDM_CLEN_SFT;
	*scale = (16 + value * 8) >> 3;
}

static int nau8360_tdm_rx_slot_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *cp = snd_kcontrol_chip(kcontrol);
	struct nau8360 *nau8360 = snd_soc_component_get_drvdata(cp);
	struct soc_enum *e = (struct soc_enum *)kcontrol->private_value;
	unsigned int *item = ucontrol->value.enumerated.item;
	unsigned int mask = e->mask << e->shift_l;
	int ret, func_idx, val_to_cache;
	bool changed = false;
	bool is_none = (item[0] == NAU8360_TDM_RXN);

	func_idx = nau8360_get_tdm_rx_func_idx(e);
	if (item[0] >= e->items || func_idx < 0)
		return -EINVAL;

	val_to_cache = is_none ? TDM_SLOT_NONE : item[0];
	if (nau8360->tdm_rx_func_slot[func_idx] == val_to_cache) {
		dev_dbg(cp->dev, "RX func %s: Slot no change (%d)",
			nau8360_rx_func_names[func_idx], val_to_cache);
		goto done;
	}

	nau8360->tdm_rx_func_slot[func_idx] = val_to_cache;
	changed = true;
	dev_dbg(cp->dev, "Cache RX func %s to %s",
		nau8360_rx_func_names[func_idx], tdm_rx_slot_text[item[0]]);

	if (is_none) {
		dev_dbg(cp->dev, "Slot is None, skipping register update");
		goto done;
	}

	ret = snd_soc_component_update_bits(cp, e->reg, mask, item[0] << e->shift_l);
	if (ret < 0)
		return ret;

done:
	return (int)changed;
}

static int nau8360_tdm_rx_slot_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *cp = snd_kcontrol_chip(kcontrol);
	struct nau8360 *nau8360 = snd_soc_component_get_drvdata(cp);
	struct soc_enum *e = (struct soc_enum *)kcontrol->private_value;
	int func_idx, slot;

	func_idx = nau8360_get_tdm_rx_func_idx(e);
	if (func_idx < 0 || func_idx >= NAU8360_TDM_RXN)
		return -EINVAL;

	slot = nau8360->tdm_rx_func_slot[func_idx];
	if (slot == TDM_SLOT_NONE)
		ucontrol->value.enumerated.item[0] = NAU8360_TDM_RXN;
	else
		ucontrol->value.enumerated.item[0] = slot;

	return 0;
}
static int nau8360_tdm_tx_slot_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *cp = snd_kcontrol_chip(kcontrol);
	struct nau8360 *nau8360 = snd_soc_component_get_drvdata(cp);
	struct soc_enum *e = (struct soc_enum *)kcontrol->private_value;
	unsigned int *item = ucontrol->value.enumerated.item;
	unsigned int mask = e->mask << e->shift_l;
	int ret, scale, slot, func_idx, val_to_cache;
	bool changed = false;
	bool is_none = (item[0] == NAU8360_TDM_TXN);

	func_idx = nau8360_get_tdm_tx_func_idx(e);
	if (item[0] >= e->items || func_idx < 0)
		return -EINVAL;

	val_to_cache = is_none ? TDM_SLOT_NONE : item[0];
	if (nau8360->tdm_tx_func_slot[func_idx] == val_to_cache) {
		dev_dbg(cp->dev, "TX func %s: Slot no change (%d)",
			nau8360_tx_func_names[func_idx], val_to_cache);
		goto done;
	}

	nau8360->tdm_tx_func_slot[func_idx] = val_to_cache;
	changed = true;
	dev_dbg(cp->dev, "Cache TX func %s to %s",
		nau8360_tx_func_names[func_idx], tdm_tx_slot_text[item[0]]);

	if (is_none) {
		dev_dbg(cp->dev, "Slot is None, skipping register update");
		goto done;
	}

	compute_slotx_scale(cp, &scale);
	slot = item[0] * scale;
	ret = snd_soc_component_update_bits(cp, e->reg, mask, slot << e->shift_l);
	if (ret < 0)
		return ret;

done:
	return (int)changed;
}

static int nau8360_tdm_tx_slot_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *cp = snd_kcontrol_chip(kcontrol);
	struct nau8360 *nau8360 = snd_soc_component_get_drvdata(cp);
	struct soc_enum *e = (struct soc_enum *)kcontrol->private_value;
	int func_idx, slot;

	func_idx = nau8360_get_tdm_tx_func_idx(e);
	if (func_idx < 0 || func_idx >= NAU8360_TDM_TXN)
		return -EINVAL;

	slot = nau8360->tdm_tx_func_slot[func_idx];
	if (slot == TDM_SLOT_NONE)
		ucontrol->value.enumerated.item[0] = NAU8360_TDM_TXN;
	else
		ucontrol->value.enumerated.item[0] = slot;

	return 0;
}

static int nau8360_pdm_slot_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *cp = snd_kcontrol_chip(kcontrol);
	struct soc_enum *e = (struct soc_enum *)kcontrol->private_value;
	unsigned int *item = ucontrol->value.enumerated.item;
	unsigned int mask = e->mask << e->shift_l;
	int scale, slot;

	if (item[0] >= e->items)
		return -EINVAL;

	compute_slotx_scale(cp, &scale);
	slot = item[0] * scale;
	return snd_soc_component_update_bits(cp, e->reg, mask, slot << e->shift_l);
}

static int nau8360_pdm_slot_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *cp = snd_kcontrol_chip(kcontrol);
	struct soc_enum *e = (struct soc_enum *)kcontrol->private_value;
	int scale, slot = snd_soc_component_read(cp, e->reg);

	compute_slotx_scale(cp, &scale);
	slot = ((slot >> e->shift_l) & e->mask) / scale;
	ucontrol->value.enumerated.item[0] = slot;

	return 0;
}

static inline int nau8360_peq_regaddr(const char *id_name)
{
	int reg, band_num, dsp_addr = NAU8360_DSP_ADDR_BYNAME(id_name);
	char *band = strstr(id_name, "BIQ");

	if (kstrtoint((band + 3), 10, &band_num))
		return -EINPROGRESS;
	reg = dsp_addr == NAU8360_RF000_DSP_COMM ? NAU8360_R100_LEFT_BIQ0_COE :
		NAU8360_R200_RIGHT_BIQ0_COE;
	reg += band_num * NAU8360_TOT_BAND_COE_RANGE;

	return reg;
}

static int nau8360_peq_coeff_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *cp = snd_kcontrol_chip(kcontrol);
	struct soc_bytes_ext *params = (void *)kcontrol->private_value;
	int i, value, reg = nau8360_peq_regaddr(kcontrol->id.name);
	u16 *val = (u16 *)ucontrol->value.bytes.data;

	if (reg < 0)
		return -EIO;

	snd_soc_component_update_bits(cp, NAU8360_R9D_PEQ_CTL, NAU8360_HW1_MEM_TEST,
		NAU8360_HW1_MEM_TEST);
	for (i = 0; i < params->max / sizeof(u16); i++) {
		value = snd_soc_component_read(cp, reg + i);
		*(val + i) = cpu_to_be16(value);
	}
	snd_soc_component_update_bits(cp, NAU8360_R9D_PEQ_CTL, NAU8360_HW1_MEM_TEST, 0);

	return 0;
}

static int nau8360_peq_coeff_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *cp = snd_kcontrol_chip(kcontrol);
	struct soc_bytes_ext *params = (void *)kcontrol->private_value;
	int i, ret, reg = nau8360_peq_regaddr(kcontrol->id.name);
	__be16 *data;
	bool changed = false;

	if (reg < 0) {
		ret = -EIO;
		goto err;
	}
	data = kmemdup(ucontrol->value.bytes.data, params->max, GFP_KERNEL);
	if (!data) {
		ret = -ENOMEM;
		goto err;
	}

	snd_soc_component_update_bits(cp, NAU8360_R9D_PEQ_CTL, NAU8360_HW1_MEM_TEST,
		NAU8360_HW1_MEM_TEST);
	for (i = 0; i < params->max / sizeof(u16); i++) {
		if (snd_soc_component_read(cp, reg + i) != be16_to_cpu(*(data + i))) {
			snd_soc_component_write(cp, reg + i, be16_to_cpu(*(data + i)));
			changed = true;
		}
	}
	snd_soc_component_update_bits(cp, NAU8360_R9D_PEQ_CTL, NAU8360_HW1_MEM_TEST, 0);

	kfree(data);

	return changed ? 1 : 0;
err:
	return ret;
}

#define NAU8360_PEQ_COEF_BYTES_EXT(ch, band) \
	SND_SOC_BYTES_EXT(ch " PEQ Coefficients " band, 20, nau8360_peq_coeff_get, \
		nau8360_peq_coeff_put)

static const struct snd_kcontrol_new nau8360_snd_controls[] = {
	NAU8360_PEQ_COEF_BYTES_EXT("Left", "BIQ0"),
	NAU8360_PEQ_COEF_BYTES_EXT("Left", "BIQ1"),
	NAU8360_PEQ_COEF_BYTES_EXT("Left", "BIQ2"),
	NAU8360_PEQ_COEF_BYTES_EXT("Left", "BIQ3"),
	NAU8360_PEQ_COEF_BYTES_EXT("Left", "BIQ4"),
	NAU8360_PEQ_COEF_BYTES_EXT("Left", "BIQ5"),
	NAU8360_PEQ_COEF_BYTES_EXT("Left", "BIQ6"),
	NAU8360_PEQ_COEF_BYTES_EXT("Left", "BIQ7"),
	NAU8360_PEQ_COEF_BYTES_EXT("Left", "BIQ8"),
	NAU8360_PEQ_COEF_BYTES_EXT("Left", "BIQ9"),
	NAU8360_PEQ_COEF_BYTES_EXT("Left", "BIQ10"),
	NAU8360_PEQ_COEF_BYTES_EXT("Left", "BIQ11"),
	NAU8360_PEQ_COEF_BYTES_EXT("Left", "BIQ12"),
	NAU8360_PEQ_COEF_BYTES_EXT("Left", "BIQ13"),
	NAU8360_PEQ_COEF_BYTES_EXT("Left", "BIQ14"),

	NAU8360_PEQ_COEF_BYTES_EXT("Right", "BIQ0"),
	NAU8360_PEQ_COEF_BYTES_EXT("Right", "BIQ1"),
	NAU8360_PEQ_COEF_BYTES_EXT("Right", "BIQ2"),
	NAU8360_PEQ_COEF_BYTES_EXT("Right", "BIQ3"),
	NAU8360_PEQ_COEF_BYTES_EXT("Right", "BIQ4"),
	NAU8360_PEQ_COEF_BYTES_EXT("Right", "BIQ5"),
	NAU8360_PEQ_COEF_BYTES_EXT("Right", "BIQ6"),
	NAU8360_PEQ_COEF_BYTES_EXT("Right", "BIQ7"),
	NAU8360_PEQ_COEF_BYTES_EXT("Right", "BIQ8"),
	NAU8360_PEQ_COEF_BYTES_EXT("Right", "BIQ9"),
	NAU8360_PEQ_COEF_BYTES_EXT("Right", "BIQ10"),
	NAU8360_PEQ_COEF_BYTES_EXT("Right", "BIQ11"),
	NAU8360_PEQ_COEF_BYTES_EXT("Right", "BIQ12"),
	NAU8360_PEQ_COEF_BYTES_EXT("Right", "BIQ13"),
	NAU8360_PEQ_COEF_BYTES_EXT("Right", "BIQ14"),

	SOC_ENUM_EXT("DACL TDM RX Slot", nau8360_tdm_dacl_slot_enum,
		nau8360_tdm_rx_slot_get, nau8360_tdm_rx_slot_put),
	SOC_ENUM_EXT("DACR TDM RX Slot", nau8360_tdm_dacr_slot_enum,
		nau8360_tdm_rx_slot_get, nau8360_tdm_rx_slot_put),
	SOC_SINGLE("Low Latency Switch", NAU8360_R96_HW2_CTL6,
		NAU8360_HW2_LATENCY_SFT, 1, 0),
	SOC_SINGLE_EXT("ANC Path Switch", NAU8360_R96_HW2_CTL6, NAU8360_HW1_ANC_EN_SFT,
		1, 0, snd_soc_get_volsw, nau8360_anc_put),
	SOC_ENUM_EXT("ANCL TDM RX Slot", nau8360_tdm_ancl_slot_enum,
		nau8360_tdm_rx_slot_get, nau8360_tdm_rx_slot_put),
	SOC_ENUM_EXT("ANCR TDM RX Slot", nau8360_tdm_ancr_slot_enum,
		nau8360_tdm_rx_slot_get, nau8360_tdm_rx_slot_put),

	SOC_DOUBLE("AEC REF Switch", NAU8360_R17_I2S0_DATA_CTRL5,
		NAU8360_AEC_L_EN_SFT, NAU8360_AEC_R_EN_SFT, 1, 0),
	SOC_ENUM("AEC REF Data Length", nau8360_aec_data_len_enum),
	SOC_ENUM_EXT("AEC Left REF Slot", nau8360_aecl_slot_enum,
		nau8360_tdm_tx_slot_get, nau8360_tdm_tx_slot_put),
	SOC_ENUM_EXT("AEC Right REF Slot", nau8360_aecr_slot_enum,
		nau8360_tdm_tx_slot_get, nau8360_tdm_tx_slot_put),

	SOC_SINGLE("ISNSL TDM TX Switch", NAU8360_R0E_I2S_DATA_CTRL1,
		NAU8360_ISNS_L_TX_EN_SFT, 1, 0),
	SOC_ENUM("ISNSL TDM Data Length", nau8360_isnsl_data_len_enum),
	SOC_ENUM_EXT("ISNSL TDM TX Slot", nau8360_isnsl_slot_enum,
		nau8360_tdm_tx_slot_get, nau8360_tdm_tx_slot_put),
	SOC_SINGLE("ISNSR TDM TX Switch", NAU8360_R11_I2S_DATA_CTRL4,
		NAU8360_ISNS_R_TX_EN_SFT, 1, 0),
	SOC_ENUM("ISNSR TDM Data Length", nau8360_isnsr_data_len_enum),
	SOC_ENUM_EXT("ISNSR TDM TX Slot", nau8360_isnsr_slot_enum,
		nau8360_tdm_tx_slot_get, nau8360_tdm_tx_slot_put),

	SOC_SINGLE("VSNSL TDM TX Switch", NAU8360_R0D_I2S_PCM_CTRL3,
		NAU8360_VSNS_L_TX_EN_SFT, 1, 0),
	SOC_ENUM("VSNSL TDM Data Length", nau8360_vsnsl_data_len_enum),
	SOC_ENUM_EXT("VSNSL TDM TX Slot", nau8360_vsnsl_slot_enum,
		nau8360_tdm_tx_slot_get, nau8360_tdm_tx_slot_put),
	SOC_SINGLE("VSNSR TDM TX Switch", NAU8360_R11_I2S_DATA_CTRL4,
		NAU8360_VSNS_R_TX_EN_SFT, 1, 0),
	SOC_ENUM("VSNSR TDM Data Length", nau8360_vsnsr_data_len_enum),
	SOC_ENUM_EXT("VSNSR TDM TX Slot", nau8360_vsnsr_slot_enum,
		nau8360_tdm_tx_slot_get, nau8360_tdm_tx_slot_put),

	SOC_SINGLE("Junction Temperature TDM TX Switch", NAU8360_R10_I2S_DATA_CTRL3,
		NAU8360_TEMP_TX_EN_SFT, 1, 0),
	SOC_ENUM_EXT("Junction Temperature TDM TX Slot", nau8360_temp_slot_enum,
		nau8360_tdm_tx_slot_get, nau8360_tdm_tx_slot_put),
	SOC_SINGLE("VBAT Measured TDM TX Switch", NAU8360_R0F_I2S_DATA_CTRL2,
		NAU8360_VBAT_TX_EN_SFT, 1, 0),
	SOC_ENUM_EXT("VBAT Measured TDM TX Slot", nau8360_vbat_slot_enum,
		nau8360_tdm_tx_slot_get, nau8360_tdm_tx_slot_put),

	SOC_DOUBLE_R("PDM Loopback Switch", NAU8360_R0E_I2S_DATA_CTRL1,
		NAU8360_R0F_I2S_DATA_CTRL2, NAU8360_PDM_L_TX_EN_SFT, 1, 0),
	SOC_ENUM("PDM Loopback Left Data Length", nau8360_pdml_data_len_enum),
	SOC_ENUM_EXT("PDM Loopback Left Slot", nau8360_pdml_slot_enum,
		nau8360_pdm_slot_get, nau8360_pdm_slot_put),
	SOC_ENUM("PDM Loopback Right Data Length", nau8360_pdmr_data_len_enum),
	SOC_ENUM_EXT("PDM Loopback Right Slot", nau8360_pdmr_slot_enum,
		nau8360_pdm_slot_get, nau8360_pdm_slot_put),
	SOC_ENUM("PDM Loopback Mode", nau8360_pdm_mode_enum),
};

static int nau8360_adci_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);

	if (SND_SOC_DAPM_EVENT_ON(event))
		snd_soc_component_update_bits(component, NAU8360_R6C_IVSNS_CFG0,
			NAU8360_PD_ISNS_R_PMD | NAU8360_PD_ISNS_L_PMD, 0);
	else if (SND_SOC_DAPM_EVENT_OFF(event))
		snd_soc_component_update_bits(component, NAU8360_R6C_IVSNS_CFG0,
			NAU8360_PD_ISNS_R_PMD | NAU8360_PD_ISNS_L_PMD,
			NAU8360_PD_ISNS_R_PMD | NAU8360_PD_ISNS_L_PMD);

	return 0;
}

static int nau8360_adcv_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);

	if (SND_SOC_DAPM_EVENT_ON(event))
		snd_soc_component_update_bits(component, NAU8360_R6C_IVSNS_CFG0,
			NAU8360_PD_VSNS_R_PMD | NAU8360_PD_VSNS_L_PMD, 0);
	else if (SND_SOC_DAPM_EVENT_OFF(event))
		snd_soc_component_update_bits(component, NAU8360_R6C_IVSNS_CFG0,
			NAU8360_PD_VSNS_R_PMD | NAU8360_PD_VSNS_L_PMD,
			NAU8360_PD_VSNS_R_PMD | NAU8360_PD_VSNS_L_PMD);

	return 0;
}

static int nau8360_aif_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);

	if (SND_SOC_DAPM_EVENT_OFF(event))
		snd_soc_component_update_bits(component, NAU8360_R67_ANALOG_CONTROL_0,
			NAU8360_HV_EN, 0);

	return 0;
}

static int nau8360_hw1_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);

	if (SND_SOC_DAPM_EVENT_OFF(event))
		snd_soc_component_update_bits(component, NAU8360_R67_ANALOG_CONTROL_0,
			NAU8360_ANA_MUTE, NAU8360_ANA_MUTE);

	return 0;
}

static int nau8360_hw1_mux_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);

	if (SND_SOC_DAPM_EVENT_OFF(event)) {
		snd_soc_component_update_bits(component, NAU8360_R68_ANALOG_CONTROL_1,
			NAU8360_DRVCTL_SEGL_FULL | NAU8360_DRVCTL_SEGR_FULL, 0);
		snd_soc_component_update_bits(component, NAU8360_RA5_ANA_REG_1,
			NAU8360_CLASSD_SHT_IN | NAU8360_HVEN_SYNC_SAW,
			NAU8360_CLASSD_SHT_IN | NAU8360_HVEN_SYNC_SAW);
		msleep(20);
	}

	return 0;
}

static int nau8360_hw2_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);

	if (SND_SOC_DAPM_EVENT_ON(event)) {
		snd_soc_component_update_bits(component, NAU8360_R99_HW2_CTL9,
			NAU8360_HW2_CH_MUTE, 0);
		snd_soc_component_update_bits(component, NAU8360_R9C_HW1_CTL2,
			NAU8360_HW1_CH_MUTE, 0);
	}

	return 0;
}

static int nau8360_adacl_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);

	if (SND_SOC_DAPM_EVENT_ON(event))
		snd_soc_component_update_bits(component, NAU8360_R6E_DAC_CFG0,
			NAU8360_PD_DACL_DIS, 0);

	return 0;
}

static int nau8360_adacr_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);

	if (SND_SOC_DAPM_EVENT_ON(event))
		snd_soc_component_update_bits(component, NAU8360_R6E_DAC_CFG0,
			NAU8360_PD_DACR_DIS, 0);

	return 0;
}

static int nau8360_dacl_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);

	if (SND_SOC_DAPM_EVENT_OFF(event))
		snd_soc_component_update_bits(component, NAU8360_R6E_DAC_CFG0,
			NAU8360_PD_DACL_DIS, NAU8360_PD_DACL_DIS);

	return 0;
}

static int nau8360_dacr_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);

	if (SND_SOC_DAPM_EVENT_OFF(event))
		snd_soc_component_update_bits(component, NAU8360_R6E_DAC_CFG0,
			NAU8360_PD_DACR_DIS, NAU8360_PD_DACR_DIS);

	return 0;
}

static int nau8360_hv_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);

	if (SND_SOC_DAPM_EVENT_ON(event)) {
		/* enable Class D HV power after DAC power is stable */
		snd_soc_component_update_bits(component, NAU8360_R67_ANALOG_CONTROL_0,
			NAU8360_HV_EN, NAU8360_HV_EN);
		msleep(50);
		/* Class D modulator input short setting for mute and de-pop purpose.
		 * Restore normal after initiation. Set segment driver as full driving
		 * strength.
		 */
		snd_soc_component_update_bits(component, NAU8360_RA5_ANA_REG_1,
			NAU8360_CLASSD_SHT_IN | NAU8360_HVEN_SYNC_SAW, 0);
		snd_soc_component_update_bits(component, NAU8360_R68_ANALOG_CONTROL_1,
			NAU8360_DRVCTL_SEGL_FULL | NAU8360_DRVCTL_SEGR_FULL,
			NAU8360_DRVCTL_SEGL_FULL | NAU8360_DRVCTL_SEGR_FULL);
	}

	return 0;
}

static int nau8360_hv_pre_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);

	if (SND_SOC_DAPM_EVENT_OFF(event)) {
		snd_soc_component_update_bits(component, NAU8360_R9C_HW1_CTL2,
			NAU8360_HW1_CH_MUTE, NAU8360_HW1_CH_MUTE);
		snd_soc_component_update_bits(component, NAU8360_R99_HW2_CTL9,
			NAU8360_HW2_CH_MUTE, NAU8360_HW2_CH_MUTE);
	}

	return 0;
}

static int nau8360_hv_post_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);

	if (SND_SOC_DAPM_EVENT_ON(event))
		snd_soc_component_update_bits(component, NAU8360_R67_ANALOG_CONTROL_0,
			NAU8360_ANA_MUTE, 0);

	return 0;
}

static int nau8360_dsp_switch(struct snd_soc_component *component, bool enable)
{
	struct nau8360 *nau8360 = snd_soc_component_get_drvdata(component);
	struct regmap *regmap = nau8360->regmap;
	int ret, value = NAU8360_PEQ_BAND_8;

	/* If DSP is enabled, unstall HW3 engine and DSP, loading DSP firmware,
	 * and configure PEQ after dsp reset.
	 */
	if (enable) {
		nau8360_dsp_software_reset(component);
		value = nau8360->anc_enable ? NAU8360_PEQ_BAND_15 : NAU8360_PEQ_BAND_12;
		nau8360_dsp_enable(regmap, true);
		if (!nau8360->dsp_created)
			ret = nau8360_dsp_init(component);
		else
			ret = nau8360_dsp_reinit(component);
		if (ret) {
			nau8360_dsp_enable(regmap, false);
			dev_err(nau8360->dev, "can't enable DSP (%d)", ret);
			return ret;
		}
	} else {
		dev_warn(nau8360->dev, "Bypass DSP !!");
		nau8360_dsp_enable(regmap, false);
	}
	regmap_update_bits(regmap, NAU8360_R9D_PEQ_CTL, NAU8360_PEQ_BAND_MASK,
		value << NAU8360_PEQ_BAND_SFT);

	return 0;
}

static int nau8360_dac_mux_put_enum(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dapm_context *dapm = snd_soc_dapm_kcontrol_to_dapm(kcontrol);
	struct snd_soc_component *component = snd_soc_dapm_to_component(dapm);
	struct nau8360 *nau8360 = snd_soc_component_get_drvdata(component);
	struct soc_enum *e = (struct soc_enum *)kcontrol->private_value;
	unsigned int *item = ucontrol->value.enumerated.item;
	bool dsp_en = snd_soc_enum_item_to_val(e, item[0]);
	int ret = 0;

	if (nau8360->dsp_enable == dsp_en)
		goto done;

	if (snd_soc_dapm_get_bias_level(dapm) > SND_SOC_BIAS_STANDBY) {
		dev_err(nau8360->dev, "changing path is not allowed during playback");
		ret = -EBUSY;
		goto done;
	}

	ret = nau8360_dsp_switch(component, dsp_en);
	if (ret)
		goto err;

	ret = snd_soc_dapm_put_enum_double(kcontrol, ucontrol);
	if (ret < 0)
		goto err;

	nau8360->dsp_enable = dsp_en;

done:
	return ret;
err:
	nau8360_dsp_switch(component, !dsp_en);
	return ret;
}

/* Select HW1 output source (enables PEQ bypass) */
static const char *const nau8360_hw1out_src[] = { "Audio", "PEQ" };

static SOC_ENUM_SINGLE_DECL(nau8360_hw1out_enum, NAU8360_R12_PATH_CTRL,
	NAU8360_SEL_HW1_SFT, nau8360_hw1out_src);

static const struct snd_kcontrol_new nau8360_hw1out_mux =
	SOC_DAPM_ENUM("HW1 Output Source", nau8360_hw1out_enum);

/* Select DAC input source (enables DSP bypass) */
static const char *const nau8360_dac_src[] = { "HW1", "DSP" };

static SOC_ENUM_SINGLE_DECL(nau8360_dac_enum, NAU8360_R12_PATH_CTRL,
	NAU8360_DAC_SEL_SFT, nau8360_dac_src);

static const struct snd_kcontrol_new nau8360_dac_mux =
	SOC_DAPM_ENUM_EXT("DAC Source", nau8360_dac_enum,
		snd_soc_dapm_get_enum_double, nau8360_dac_mux_put_enum);

static const struct snd_soc_dapm_widget nau8360_dapm_widgets[] = {
	SND_SOC_DAPM_SIGGEN("Sense"),
	SND_SOC_DAPM_ADC_E("ADC_I", NULL, SND_SOC_NOPM, 0, 0, nau8360_adci_event,
		SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_ADC_E("ADC_V", NULL, SND_SOC_NOPM, 0, 0, nau8360_adcv_event,
		SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_PGA("DSP", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("HW3 Engine", NAU8360_R8C_HW3_CTL6, 3, 0, NULL, 0),

	SND_SOC_DAPM_AIF_IN_E("AIFRX", "Playback", 0, SND_SOC_NOPM, 0, 0,
		nau8360_aif_event, SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_PGA_S("HW1 Engine", 0, NAU8360_R9D_PEQ_CTL, 0, 1,
		nau8360_hw1_event, SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_MUX_E("HW1 Mux", SND_SOC_NOPM, 0, 0, &nau8360_hw1out_mux,
		nau8360_hw1_mux_event, SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_MUX("DAC Mux", SND_SOC_NOPM, 0, 0, &nau8360_dac_mux),

	SND_SOC_DAPM_PGA_S("HW2 Engine", 1, NAU8360_R96_HW2_CTL6, 5, 0,
		nau8360_hw2_event, SND_SOC_DAPM_POST_PMU),

	SND_SOC_DAPM_SUPPLY("DAC Clock", NAU8360_R71_CLK_DIV_CFG, 9, 1, NULL, 0),
	SND_SOC_DAPM_DAC_E("DACL", NULL, SND_SOC_NOPM, 0, 0,
		nau8360_dacl_event, SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_DAC_E("DACR", NULL, SND_SOC_NOPM, 0, 0,
		nau8360_dacr_event, SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_PGA_S("ADACL", 2, SND_SOC_NOPM, 0, 0,
		nau8360_adacl_event, SND_SOC_DAPM_POST_PMU),
	SND_SOC_DAPM_PGA_S("ADACR", 2, SND_SOC_NOPM, 0, 0,
		nau8360_adacr_event, SND_SOC_DAPM_POST_PMU),
	SND_SOC_DAPM_PGA_S("Class D", 3, SND_SOC_NOPM, 0, 0,
		nau8360_hv_event, SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_PRE("Class D Pre", nau8360_hv_pre_event),
	SND_SOC_DAPM_POST("Class D Post", nau8360_hv_post_event),

	SND_SOC_DAPM_OUTPUT("OUTL"),
	SND_SOC_DAPM_OUTPUT("OUTR"),
};

static const struct snd_soc_dapm_route nau8360_dapm_routes[] = {
	{ "ADC_I", NULL, "Sense" },
	{ "ADC_V", NULL, "Sense" },
	{ "HW3 Engine", NULL, "ADC_I" },
	{ "HW3 Engine", NULL, "ADC_V" },
	{ "Capture", NULL, "HW3 Engine" },

	{ "HW1 Engine", NULL, "AIFRX" },
	{ "HW1 Mux", "Audio", "AIFRX" },
	{ "HW1 Mux", "PEQ", "HW1 Engine" },

	{ "DSP", NULL, "HW1 Mux" },
	{ "DAC Mux", "HW1", "HW1 Mux" },
	{ "DAC Mux", "DSP", "DSP" },

	{ "HW2 Engine", NULL, "DAC Mux" },
	{ "DACL", NULL, "HW2 Engine" },
	{ "DACR", NULL, "HW2 Engine" },
	{ "DACL", NULL, "DAC Clock" },
	{ "DACR", NULL, "DAC Clock" },

	{ "ADACL", NULL, "DACL" },
	{ "ADACR", NULL, "DACR" },
	{ "Class D", NULL, "ADACL" },
	{ "Class D", NULL, "ADACR" },

	{ "OUTL", NULL, "Class D" },
	{ "OUTR", NULL, "Class D" },
};

static int nau8360_startup(struct snd_pcm_substream *substream, struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct nau8360 *nau8360 = snd_soc_component_get_drvdata(component);

	if (nau8360->dsp_enable && substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		snd_soc_dapm_enable_pin(nau8360->dapm, "Sense");

	return 0;
}

static void nau8360_shutdown(struct snd_pcm_substream *substream,
	struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct nau8360 *nau8360 = snd_soc_component_get_drvdata(component);
	unsigned int tdm_mask;

	tdm_mask = (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) ?
		NAU8360_EN_TDM_RX : NAU8360_EN_TDM_TX;
	regmap_update_bits(nau8360->regmap, NAU8360_R0B_I2S_PCM_CTRL1,
		tdm_mask, 0);
	if (nau8360->dsp_enable && substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		snd_soc_dapm_disable_pin(nau8360->dapm, "Sense");
}

static int nau8360_hw_params(struct snd_pcm_substream *substream,
	struct snd_pcm_hw_params *params, struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct nau8360 *nau8360 = snd_soc_component_get_drvdata(component);
	unsigned int val_len, val_srate;
	int ret, dlen = params_width(params);

	if (dlen > nau8360->tdm_chan_len) {
		dev_err(nau8360->dev, "Invalid data length");
		ret = -EINVAL;
		goto err;
	}

	switch (dlen) {
	case 16:
		val_len = NAU8360_TDM_DLEN_16;
		break;
	case 20:
		val_len = NAU8360_TDM_DLEN_20;
		break;
	case 24:
		val_len = NAU8360_TDM_DLEN_24;
		break;
	case 32:
		val_len = NAU8360_TDM_DLEN_32;
		break;
	default:
		ret = -EINVAL;
		goto err;
	}

	switch (params_rate(params)) {
	case 16000:
		val_srate = NAU8360_SRATE_16000;
		break;
	case 32000:
		val_srate = NAU8360_SRATE_32000;
		break;
	case 44100:
	case 48000:
		val_srate = NAU8360_SRATE_48000;
		break;
	case 88200:
	case 96000:
		val_srate = NAU8360_SRATE_96000;
		break;
	case 176400:
	case 192000:
		val_srate = NAU8360_SRATE_192000;
		break;
	default:
		ret = -EINVAL;
		goto err;
	}

	regmap_update_bits(nau8360->regmap, NAU8360_R0C_I2S_PCM_CTRL2,
		NAU8360_TDM_DLEN_MASK, val_len);
	regmap_update_bits(nau8360->regmap, NAU8360_R40_CLK_DET_CTRL,
		NAU8360_SRATE_MASK, val_srate);
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		regmap_update_bits(nau8360->regmap, NAU8360_R0B_I2S_PCM_CTRL1,
			NAU8360_EN_TDM_RX, NAU8360_EN_TDM_RX);

	return 0;
err:
	return ret;
}

static int nau8360_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct snd_soc_component *component = dai->component;
	unsigned int ctrl_val, ctrl1_val;
	int ret;

	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBC_CFC:
		break;
	default:
		ret = -EINVAL;
		goto err;
	}

	switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_NB_NF:
		break;
	default:
		ret = -EINVAL;
		goto err;
	}

	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
		ctrl_val = NAU8360_FRAME_START_H2L | NAU8360_RX_OFFSET_I2S;
		ctrl1_val = NAU8360_TX_OFFSET_I2S;
		break;
	case SND_SOC_DAIFMT_LEFT_J:
		ctrl_val = NAU8360_FRAME_START_H2L | NAU8360_RX_OFFSET_LEFT;
		ctrl1_val = NAU8360_TX_OFFSET_LEFT;
		break;
	case SND_SOC_DAIFMT_RIGHT_J:
		ctrl_val = NAU8360_FRAME_START_H2L | NAU8360_RX_OFFSET_RIGHT;
		ctrl1_val = NAU8360_TX_OFFSET_RIGHT;
		break;
	case SND_SOC_DAIFMT_DSP_A:
		ctrl_val = NAU8360_RX_OFFSET_PCM_A;
		ctrl1_val = NAU8360_TX_OFFSET_PCM_A;
		break;
	case SND_SOC_DAIFMT_DSP_B:
		ctrl_val = NAU8360_RX_OFFSET_PCM_B;
		ctrl1_val = NAU8360_TX_OFFSET_PCM_B;
		break;
	default:
		ret = -EINVAL;
		goto err;
	}

	snd_soc_component_update_bits(component, NAU8360_R0B_I2S_PCM_CTRL1,
		NAU8360_FRAME_START_MASK | NAU8360_RX_OFFSET_MASK, ctrl_val);
	snd_soc_component_update_bits(component, NAU8360_R0D_I2S_PCM_CTRL3,
		NAU8360_TX_OFFSET_MASK, ctrl1_val);

	return 0;
err:
	return ret;
}

static int nau8360_validate_tdm_slots(struct device *dev, unsigned int mask,
	const int *func_slots, const char * const *func_names,
	int num_funcs, const char *dir,
	unsigned int *slot_used)
{
	int i, func_slot;
	*slot_used = 0;

	if (!mask)
		return 0;

	for (i = 0; i < num_funcs; i++) {
		func_slot = func_slots[i];
		if (func_slot == TDM_SLOT_NONE) {
			dev_warn(dev, "%s %s slot disabled\n",
				dir, func_names[i]);
			continue;
		}

		if (!(mask & BIT(func_slot))) {
			dev_warn(dev, "%s %s mapped to slot %d, but disabled by mask!\n",
				dir, func_names[i], func_slot);
			continue;
		}

		if (*slot_used & BIT(func_slot)) {
			dev_err(dev, "%s %s slot %d collision!\n",
				dir, func_names[i], func_slot);
			return -EINVAL;
		}

		*slot_used |= BIT(func_slot);
	}

	return 0;
}

static int nau8360_set_tdm_slot(struct snd_soc_dai *dai, unsigned int tx_mask,
	unsigned int rx_mask, int slots, int slot_width)
{
	struct snd_soc_component *cp = dai->component;
	struct nau8360 *nau8360 = snd_soc_component_get_drvdata(cp);
	unsigned int tx_slot_used = 0, rx_slot_used = 0;
	int ret = 0;

	if (slot_width != 16 && slot_width != 24 && slot_width != 32) {
		dev_err(cp->dev, "Invalid TDM channel length: %d", slot_width);
		return -EINVAL;
	}

	ret = nau8360_validate_tdm_slots(cp->dev, rx_mask,
		nau8360->tdm_rx_func_slot,
		nau8360_rx_func_names,
		NAU8360_TDM_RXN, "RX",
		&rx_slot_used);
	if (ret < 0)
		return ret;

	ret = nau8360_validate_tdm_slots(cp->dev, tx_mask,
		nau8360->tdm_tx_func_slot,
		nau8360_tx_func_names,
		NAU8360_TDM_TXN, "TX",
		&tx_slot_used);
	if (ret < 0)
		return ret;

	if (tx_slot_used)
		regmap_update_bits(nau8360->regmap, NAU8360_R0B_I2S_PCM_CTRL1,
			NAU8360_EN_TDM_TX, NAU8360_EN_TDM_TX);

	nau8360->tdm_chan_len = slot_width;
	regmap_update_bits(nau8360->regmap, NAU8360_R0C_I2S_PCM_CTRL2,
		NAU8360_TDM_CLEN_MASK,
		((nau8360->tdm_chan_len - 16) >> 3) << NAU8360_TDM_CLEN_SFT);

	dev_dbg(cp->dev, "TDM: tx_mask 0x%x, rx_mask 0x%x", tx_mask, rx_mask);

	return ret;
}

static inline int dyn_clk_div(int div_max, int fin, int fout)
{
	int clk_div;

	if (!fin || !fout)
		return -EINVAL;

	for (clk_div = 0; clk_div <= div_max; clk_div++)
		if (fin / (clk_div + 1) <= fout)
			break;
	if (clk_div > div_max)
		return -EINVAL;

	return clk_div;
}

static inline int tab_clk_div(const int clk_div[], int num_div, int fin)
{
	int i;

	if (!fin)
		return -EINVAL;

	for (i = 0; i < num_div; i++)
		if ((fin / clk_div[i]) <= CLK_DA_IVSNS_MAX)
			break;
	if (i == num_div)
		return -EINVAL;

	return i;
}

static int nau8360_set_sysclk_output(struct nau8360 *nau8360, unsigned int freq)
{
	int ratio = 0, mclk_rate;

	if (freq < MASTER_CLK_MIN || freq > MASTER_CLK_MAX) {
		dev_err(nau8360->dev, "system clock %d Hz exceed range", freq);
		goto err;
	}

	if (freq % ADSP_SR_48000 == 0)
		ratio = freq / ADSP_SR_48000;
	else if (freq % ADSP_SR_44100 == 0)
		ratio = freq / ADSP_SR_44100;
	else
		goto err;

	switch (ratio) {
	case NAU8360_MCLK_FS_RATIO_250:
		mclk_rate = NAU8360_MCLK_RATE_12000;
		break;
	case NAU8360_MCLK_FS_RATIO_256:
		mclk_rate = NAU8360_MCLK_RATE_12288;
		break;
	case NAU8360_MCLK_FS_RATIO_400:
		mclk_rate = NAU8360_MCLK_RATE_19200;
		break;
	case NAU8360_MCLK_FS_RATIO_500:
		mclk_rate = NAU8360_MCLK_RATE_24000;
		break;
	case NAU8360_MCLK_FS_RATIO_512:
		mclk_rate = NAU8360_MCLK_RATE_24576;
		break;
	default:
		dev_err(nau8360->dev, "invalid sysclk %d", freq);
		goto err;
	}

	dev_dbg(nau8360->dev, "sysclk %d Hz, ratio %d", freq, ratio);

	nau8360->sys_clk = freq;
	regmap_update_bits(nau8360->regmap, NAU8360_R40_CLK_DET_CTRL,
		NAU8360_MCLK_RATE_MASK, mclk_rate);

	return 0;
err:
	return -EINVAL;
}

static int nau8360_dig_sys_clk(struct nau8360 *nau8360, int source, unsigned int freq)
{
	struct device *dev = nau8360->dev;
	struct regmap *regmap = nau8360->regmap;
	int value, mclk_div;

	switch (source) {
	case NAU8360_CLK_SRC_ICLK:
		/* switch HIRC and clock down to 12MHz  */
		regmap_update_bits(regmap, NAU8360_R04_CLK_CTRL1, NAU8360_MCLK_SEL_MASK,
			NAU8360_MCLK_SEL_HIRC48M);
		regmap_update_bits(regmap, NAU8360_R03_CLK_CTRL0,
			NAU8360_MCLK_DIV_MASK, 0x3 << NAU8360_MCLK_DIV_SFT);
		return 0;

	case NAU8360_CLK_SRC_MCLK:
		value = NAU8360_MCLK_SEL_MCLK;
		break;

	case NAU8360_CLK_SRC_PLL:
		value = NAU8360_MCLK_SEL_PLL;
		break;

	default:
		goto err;
	}

	regmap_update_bits(regmap, NAU8360_R04_CLK_CTRL1, NAU8360_MCLK_SEL_MASK, value);

	mclk_div = dyn_clk_div(NAU8360_MCLK_DIV_MAX, freq, nau8360->sys_clk);
	if (mclk_div < 0) {
		dev_err(dev, "mclk_div (%d -> %d):  error", freq, nau8360->sys_clk);
		goto err;
	}
	regmap_update_bits(regmap, NAU8360_R03_CLK_CTRL0, NAU8360_MCLK_DIV_MASK,
		mclk_div << NAU8360_MCLK_DIV_SFT);

	dev_dbg(dev, " mclk_div (%d -> %d): %d", freq, nau8360->sys_clk, mclk_div + 1);

	return 0;
err:
	return -EINVAL;
}

static int nau8360_ana_sys_clk(struct nau8360 *nau8360, int source, unsigned int freq)
{
	struct device *dev = nau8360->dev;
	struct regmap *regmap = nau8360->regmap;
	struct nau8360_pll *pll = &nau8360->pll;
	int value, pclk_div, ivdiv_sel, ddiv_sel;

	if (source == NAU8360_CLK_SRC_PLL)
		value = NAU8360_CLK_ANA_SEL_PLL;
	else if (source == NAU8360_CLK_SRC_MCLK)
		value = NAU8360_CLK_ANA_SEL_MCLK;
	else if (source == NAU8360_CLK_SRC_BCLK)
		value = NAU8360_CLK_ANA_SEL_BCLK;
	else
		goto err;

	if (source == NAU8360_CLK_SRC_PLL) {
		freq = pll->output;
		pclk_div = dyn_clk_div(NAU8360_PLLOUT_DIV_MAX, freq, nau8360->sys_clk);
		if (pclk_div < 0) {
			dev_err(dev, "pll_div (%d -> %d): error", freq, nau8360->sys_clk);
			goto err;
		}

		dev_dbg(dev, " pll_div (%d -> %d): %d", freq, nau8360->sys_clk, pclk_div + 1);

		regmap_update_bits(regmap, NAU8360_R72_PLL_CFG0, NAU8360_PLLOUT_DIV_MASK,
			pclk_div << NAU8360_PLLOUT_DIV_SFT);
		freq /= (pclk_div + 1);
	}

	ivdiv_sel = tab_clk_div(ivsns_clk_div, ARRAY_SIZE(ivsns_clk_div), freq);
	if (ivdiv_sel < 0) {
		dev_err(dev, "iv_div (%d -> %d): error", freq, CLK_DA_IVSNS_MAX);
		goto err;
	}
	value |= ivdiv_sel << NAU8360_IVSNS_CLK_DIV_SFT;

	ddiv_sel = tab_clk_div(dac_clk_div, ARRAY_SIZE(dac_clk_div), freq);
	if (ddiv_sel < 0) {
		dev_err(dev, "dac_div (%d -> %d): error", freq, CLK_DA_IVSNS_MAX);
		goto err;
	}
	value |= ddiv_sel << NAU8360_DAC_CLK_DIV_SFT;

	dev_dbg(dev, " clk_div (%d -> %d): ivsns %d, dac %d", freq, CLK_DA_IVSNS_MAX,
		ivsns_clk_div[ivdiv_sel], dac_clk_div[ddiv_sel]);

	regmap_update_bits(regmap, NAU8360_R71_CLK_DIV_CFG, NAU8360_CLK_ANA_SEL_MASK |
		NAU8360_IVSNS_CLK_DIV_MASK | NAU8360_DAC_CLK_DIV_MASK, value);

	return 0;
err:
	return -EINVAL;
}

static int nau8360_dsp_hw_clk(struct nau8360 *nau8360, int source, unsigned int freq)
{
	struct device *dev = nau8360->dev;
	struct regmap *regmap = nau8360->regmap;
	int val_dsp, val_hw, clk_div, dsp_clk;

	if (source == NAU8360_CLK_SRC_ICLK) {
		/* switch HIRC and clock down to 12MHz  */
		regmap_update_bits(regmap, NAU8360_R03_CLK_CTRL0,
			NAU8360_DSP_CLK_SEL_MASK | NAU8360_DSP_CLK_DIV_MASK,
			NAU8360_DSP_CLK_SEL_HIRC48M | 0x3 << NAU8360_DSP_CLK_DIV_SFT);
		regmap_update_bits(regmap, NAU8360_R04_CLK_CTRL1,
			NAU8360_HW_CLK_SEL_MASK | NAU8360_HW_CLK_DIV_MASK,
			NAU8360_HW_CLK_SEL_HIRC48M | 0x3 << NAU8360_HW_CLK_DIV_SFT);
		return 0;
	}

	if (freq % ADSP_SR_48000 == 0)
		dsp_clk = DSP_OP_CLK48;
	else if (freq % ADSP_SR_44100 == 0)
		dsp_clk = DSP_OP_CLK44;
	else
		goto err;
	clk_div = dyn_clk_div(NAU8360_DSP_CLK_DIV_MAX, freq, dsp_clk);
	if (clk_div < 0) {
		dev_err(dev, "dsp/hw clk_div (%d -> %d): error", freq, dsp_clk);
		goto err;
	}
	val_dsp = clk_div << NAU8360_DSP_CLK_DIV_SFT;
	val_hw = clk_div << NAU8360_HW_CLK_DIV_SFT;
	if (source == NAU8360_CLK_SRC_MCLK) {
		val_dsp |= NAU8360_DSP_CLK_SEL_MCLK;
		val_hw |= NAU8360_HW_CLK_SEL_MCLK;
	} else if (source == NAU8360_CLK_SRC_PLL) {
		val_dsp |= NAU8360_DSP_CLK_SEL_PLL;
		val_hw |= NAU8360_HW_CLK_SEL_PLL;
	} else
		goto err;

	dev_dbg(dev, " dsp/hw clk_div (%d -> %d): %d", freq, dsp_clk, clk_div + 1);

	regmap_update_bits(regmap, NAU8360_R03_CLK_CTRL0, NAU8360_DSP_CLK_SEL_MASK |
		NAU8360_DSP_CLK_DIV_MASK, val_dsp);
	regmap_update_bits(regmap, NAU8360_R04_CLK_CTRL1, NAU8360_HW_CLK_SEL_MASK |
		NAU8360_HW_CLK_DIV_MASK, val_hw);

	return 0;
err:
	return -EINVAL;
}

static int nau8360_set_sysclk(struct snd_soc_component *component,
	int clk_id, int source, unsigned int freq, int dir)
{
	struct nau8360 *nau8360 = snd_soc_component_get_drvdata(component);
	static const char * const idtab[] = { "DIG", "ANA", "Internal" };
	static const char * const srctab[] = { "MCLK", "PLL", "HIRC48M", "BCLK" };
	int ret;

	if (dir == SND_SOC_CLOCK_OUT) {
		dev_dbg(nau8360->dev, "sysclk: freq %d (out)", freq);
		return nau8360_set_sysclk_output(nau8360, freq);
	}

	switch (clk_id) {
	case NAU8360_CLK_ID_INT:
		source = NAU8360_CLK_SRC_ICLK;
		dev_dbg(nau8360->dev, "sysclk: id %d (%s), src %d (%s) (in)",
			clk_id, idtab[clk_id], source, srctab[source]);

		nau8360_dsp_hw_clk(nau8360, source, 0);
		nau8360_dig_sys_clk(nau8360, source, 0);

		regmap_update_bits(nau8360->regmap, NAU8360_R72_PLL_CFG0,
			NAU8360_PD_PLL_MASK, NAU8360_PD_PLL_DIS);
		break;

	case NAU8360_CLK_ID_DIG:
		dev_dbg(nau8360->dev, "sysclk: id %d (%s), src %d (%s), freq %d (in)",
			clk_id, idtab[clk_id], source, srctab[source], freq);

		if (source == NAU8360_CLK_SRC_BCLK) {
			ret = -EINVAL;
			goto err;
		}
		if (source == NAU8360_CLK_SRC_MCLK)
			regmap_update_bits(nau8360->regmap, NAU8360_R72_PLL_CFG0,
				NAU8360_PD_PLL_MASK, NAU8360_PD_PLL_DIS);

		ret = nau8360_dig_sys_clk(nau8360, source, freq);
		if (ret)
			goto err;
		ret = nau8360_dsp_hw_clk(nau8360, source, freq);
		if (ret)
			goto err;
		break;

	case NAU8360_CLK_ID_ANA:
		dev_dbg(nau8360->dev, "sysclk: id %d (%s), src %d (%s), freq %d (in)",
			clk_id, idtab[clk_id], source, srctab[source], freq);

		if (source == NAU8360_CLK_SRC_MCLK || source == NAU8360_CLK_SRC_BCLK)
			regmap_update_bits(nau8360->regmap, NAU8360_R72_PLL_CFG0,
				NAU8360_PD_PLL_MASK, NAU8360_PD_PLL_DIS);

		ret = nau8360_ana_sys_clk(nau8360, source, freq);
		if (ret)
			goto err;
		break;

	default:
		ret = -EINVAL;
		goto err;
	}

	return 0;
err:
	return ret;
}

static int nau8360_calc_pll(struct nau8360 *nau8360)
{
	struct nau8360_pll *pll = &nau8360->pll;
	u64 fref = 0ULL, fvco = 0ULL, ratio;
	int ret, fr, fv;

	if (pll->src != NAU8360_PLL_INTERNAL &&
		(pll->input > PLL_FREQ_MAX || pll->input < PLL_FREQ_MIN)) {
		ret = -EINVAL;
		goto err;
	}

	if (pll->output > PLL_FOUT_MAX || pll->output < PLL_FOUT_MIN) {
		ret = -EINVAL;
		goto err;
	}

	for (pll->msel = 1; pll->msel <= MSEL_MAX; pll->msel++) {
		fr = pll->input / pll->msel;
		if (fr <= PLL_FREF_MAX) {
			fref = fr;
			break;
		}
	}
	if (!fref) {
		ret = -ERANGE;
		goto err;
	}

	for (pll->rsel = 1; pll->rsel <= RSEL_MAX; pll->rsel++) {
		fv = pll->output * pll->rsel;
		if (fv >= PLL_FVCO_MIN) {
			fvco = fv;
			break;
		}
	}
	if (!fvco) {
		ret = -ERANGE;
		goto err;
	}

	dev_dbg(nau8360->dev, "fref(1-8MHz): %lld, fvco(50-125MHz): %lld", fref, fvco);

	/* Calculate the PLL 8-bit integer input and 12-bit fractional */
	ratio = div_u64(fvco << 12, fref);
	pll->nsel = (ratio >> 12) & 0xff;
	pll->xsel = ratio & 0xfff;

	return 0;
err:
	return ret;
}

static int nau8360_set_pll(struct snd_soc_component *cp, int pll_id, int source,
	unsigned int freq_in, unsigned int freq_out)
{
	struct nau8360 *nau8360 = snd_soc_component_get_drvdata(cp);
	struct nau8360_pll *pll = &nau8360->pll;
	int ctrl_val, ret;

	switch (source) {
	case NAU8360_PLL_MCLK:
		ctrl_val = NAU8360_PLL_CLK_SEL_MCLK;
		break;
	case NAU8360_PLL_BCLK:
		ctrl_val = NAU8360_PLL_CLK_SEL_BCLK;
		break;
	case NAU8360_PLL_INTERNAL:
		ctrl_val = NAU8360_PLL_CLK_SEL_HIRC;
		break;
	default:
		return -EINVAL;
	}
	pll->src = source;
	pll->input = freq_in;
	pll->output = freq_out;
	ret = nau8360_calc_pll(nau8360);
	if (ret) {
		dev_err(nau8360->dev, "clock error input %d output %d", freq_in, freq_out);
		return ret;
	}
	dev_dbg(nau8360->dev, "src:%d, input:%d, output:%d, M:%d, R:%d, N:%d, X:%d", pll->src,
		pll->input, pll->output, pll->msel, pll->rsel, pll->nsel, pll->xsel);

	regmap_update_bits(nau8360->regmap, NAU8360_R72_PLL_CFG0, NAU8360_PD_PLL_MASK |
		NAU8360_PLL_CLK_SEL_MASK, NAU8360_PD_PLL_EN | ctrl_val);
	regmap_write_bits(nau8360->regmap, NAU8360_R73_PLL_CFG1,
		NAU8360_RSEL_MASK | NAU8360_MSEL_MASK | NAU8360_NSEL_MASK,
		(pll->rsel - 1) << NAU8360_RSEL_SFT |
		(pll->msel - 1) << NAU8360_MSEL_SFT | (pll->nsel - 1));
	regmap_write_bits(nau8360->regmap, NAU8360_R74_PLL_CFG2, NAU8360_XSEL_MASK,
		pll->xsel);

	return 0;
}

static void nau8360_coeff_set_def(struct regmap *regmap)
{
	int i;

	regmap_update_bits(regmap, NAU8360_R9D_PEQ_CTL, NAU8360_HW1_MEM_TEST,
		NAU8360_HW1_MEM_TEST);
	for (i = 0; i < NAU8360_TOT_BAND_PER_CH; i++) {
		regmap_write(regmap, NAU8360_R100_LEFT_BIQ0_COE + 1 +
			i * NAU8360_TOT_BAND_COE_RANGE, 0x20);
		regmap_write(regmap, NAU8360_R200_RIGHT_BIQ0_COE + 1 +
			i * NAU8360_TOT_BAND_COE_RANGE, 0x20);
	}
	regmap_update_bits(regmap, NAU8360_R9D_PEQ_CTL, NAU8360_HW1_MEM_TEST, 0);
}

static inline int nau8360_vbat_level(struct regmap *regmap)
{
	int value;

	regmap_read(regmap, NAU8360_R21_VBAT_READOUT, &value);
	/* multiple 100 on value scale */
	return (value * 100 + NAU8360_VBAT_BASE) / NAU8360_VBAT_STEP;
}

static inline int nau8360_sawtooth_params(int vbat, int *vsaw_level, int *vsaw_slope)
{
	if (!vsaw_level || !vsaw_slope)
		return -EINVAL;

	if (vbat < NAU8360_VBAT_MID_THRES) {
		*vsaw_level = 0x1;
		*vsaw_slope = 0x0;
	} else if (vbat < NAU8360_VBAT_HIGH_THRES) {
		*vsaw_level = 0x2;
		*vsaw_slope = 0x1;
	} else {
		*vsaw_level = 0x3;
		*vsaw_slope = 0x2;
	}

	return 0;
}

static int nau8360_codec_probe(struct snd_soc_component *component)
{
	struct snd_soc_dapm_context *dapm = snd_soc_component_to_dapm(component);
	struct nau8360 *nau8360 = snd_soc_component_get_drvdata(component);
	struct regmap *regmap = nau8360->regmap;
	struct device *dev = nau8360->dev;
	int ret, vbat, vsaw_level, vsaw_slope;
	int value = NAU8360_PEQ_BAND_8;

	nau8360->dapm = dapm;

	nau8360_coeff_set_def(regmap);
	regmap_update_bits(regmap, NAU8360_R90_HW2_CTL0, NAU8360_HW2_STALL, 0);
	nau8360_dsp_software_reset(component);
	if (nau8360->dsp_enable) {
		value = NAU8360_PEQ_BAND_12;
		nau8360_dsp_enable(regmap, true);
		ret = nau8360_dsp_init(component);
		if (ret) {
			nau8360_dsp_enable(regmap, false);
			dev_err(dev, "can't enable DSP (%d)", ret);
			goto err;
		}
	}
	regmap_update_bits(regmap, NAU8360_R9D_PEQ_CTL, NAU8360_PEQ_BAND_MASK,
		value << NAU8360_PEQ_BAND_SFT);

	/* defalut disable Sense signal after booting */
	snd_soc_dapm_disable_pin(nau8360->dapm, "Sense");
	snd_soc_dapm_sync(nau8360->dapm);

	/* VBAT is sensed by chip. */
	vbat = nau8360_vbat_level(regmap);
	if (vbat < NAU8360_VBAT_MIN || vbat > NAU8360_VBAT_MAX) {
		dev_err(dev, "VBAT %dV is out of valid range (%dV-%dV)\n",
			vbat, NAU8360_VBAT_MIN, NAU8360_VBAT_MAX);
		ret = -ERANGE;
		goto err;
	}
	dev_dbg(dev, "VBAT %dV for nau8360", vbat);

	/* Config sawtooth clock according to VBAT. Class D modulator input short setting
	 * for mute and de-pop purpose. Restore normal after initiation.
	 */
	ret = nau8360_sawtooth_params(vbat, &vsaw_level, &vsaw_slope);
	if (ret) {
		dev_err(dev, "can't get sawtooth clock parameters (%d)", ret);
		goto err;
	}
	regmap_update_bits(regmap, NAU8360_RA5_ANA_REG_1, NAU8360_VSAW_LV_MASK |
		NAU8360_KVCO_SAW_MASK, (vsaw_level << NAU8360_VSAW_LV_SFT) |
		(vsaw_slope << NAU8360_KVCO_SAW_SFT));

	return 0;
err:
	return ret;
}

static int __maybe_unused nau8360_suspend(struct snd_soc_component *component)
{
	struct nau8360 *nau8360 = snd_soc_component_get_drvdata(component);

	regmap_update_bits(nau8360->regmap, NAU8360_R90_HW2_CTL0, NAU8360_HW2_STALL,
		NAU8360_HW2_STALL);
	if (nau8360->dsp_enable)
		nau8360_dsp_enable(nau8360->regmap, false);

	regcache_cache_only(nau8360->regmap, true);
	regcache_mark_dirty(nau8360->regmap);

	return 0;
}

static int __maybe_unused nau8360_resume(struct snd_soc_component *component)
{
	struct nau8360 *nau8360 = snd_soc_component_get_drvdata(component);
	struct regmap *regmap = nau8360->regmap;
	int ret;

	regcache_cache_only(regmap, false);
	regcache_sync(regmap);
	/* disable Sense at standby */
	snd_soc_dapm_disable_pin(nau8360->dapm, "Sense");
	snd_soc_dapm_sync(nau8360->dapm);

	regmap_update_bits(regmap, NAU8360_R90_HW2_CTL0, NAU8360_HW2_STALL, 0);
	nau8360_dsp_software_reset(component);
	if (nau8360->dsp_enable) {
		nau8360_dsp_enable(regmap, true);
		/* loading DSP firmware */
		ret = nau8360_dsp_reinit(component);
		if (ret) {
			nau8360_dsp_enable(regmap, false);
			dev_err(nau8360->dev, "can't enable DSP (%d)", ret);
			goto err;
		}
	}

	return 0;
err:
	return ret;
}

static const struct snd_soc_component_driver soc_comp_dev_nau8360 = {
	.probe			= nau8360_codec_probe,
	.set_sysclk		= nau8360_set_sysclk,
	.set_pll		= nau8360_set_pll,
	.suspend		= nau8360_suspend,
	.resume			= nau8360_resume,
	.controls		= nau8360_snd_controls,
	.num_controls		= ARRAY_SIZE(nau8360_snd_controls),
	.dapm_widgets		= nau8360_dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(nau8360_dapm_widgets),
	.dapm_routes		= nau8360_dapm_routes,
	.num_dapm_routes	= ARRAY_SIZE(nau8360_dapm_routes),
	.suspend_bias_off	= 1,
	.idle_bias_on		= 1,
	.use_pmdown_time	= 1,
	.endianness		= 1,
};

static const struct snd_soc_dai_ops nau8360_dai_ops = {
	.startup	= nau8360_startup,
	.shutdown	= nau8360_shutdown,
	.hw_params	= nau8360_hw_params,
	.set_fmt	= nau8360_set_fmt,
	.set_tdm_slot	= nau8360_set_tdm_slot,
};

#define NAU8360_RATES (SNDRV_PCM_RATE_16000 | SNDRV_PCM_RATE_32000 | \
	SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_88200 | \
	SNDRV_PCM_RATE_96000 | SNDRV_PCM_RATE_176400 | SNDRV_PCM_RATE_192000)

#define NAU8360_FORMATS (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S20_3LE | \
	SNDRV_PCM_FMTBIT_S24_3LE | SNDRV_PCM_FMTBIT_S32_LE)

static struct snd_soc_dai_driver nau8360_dai = {
	.name = NAU8360_CODEC_DAI,
	.playback = {
		.stream_name = "Playback",
		.channels_min = 1,
		.channels_max = 4,
		.rates = NAU8360_RATES,
		.formats = NAU8360_FORMATS,
	},
	.capture = {
		.stream_name = "Capture",
		.channels_min = 1,
		.channels_max = 8,
		.rates = NAU8360_RATES,
		.formats = NAU8360_FORMATS,
	},
	.ops = &nau8360_dai_ops,
};

static int nau8360_reg_write(void *context, unsigned int reg, unsigned int value)
{
	struct i2c_client *client = context;
	int ret, count = 0;
	u8 buf[6];

	buf[count++] = reg >> 8;
	buf[count++] = reg & 0xff;
	if (reg != NAU8360_RF000_DSP_COMM && reg != NAU8360_RF002_DSP_COMM) {
		/* format for Codec, 2 bytes value and endian big */
		buf[count++] = value >> 8;
		buf[count++] = value & 0xff;
		dev_dbg(&client->dev, " %x <= %x", reg, value);
	} else {
		/* format for DSP, 4 bytes value and native */
		buf[count++] = value & 0xff;
		buf[count++] = (value >> 8) & 0xff;
		buf[count++] = (value >> 16) & 0xff;
		buf[count++] = (value >> 24) & 0xff;
	}

	ret = i2c_master_send(client, buf, count);
	if (ret == count)
		return 0;
	else if (ret < 0)
		return ret;
	else
		return -EIO;
}

static int nau8360_reg_read(void *context, unsigned int reg, unsigned int *value)
{
	struct i2c_client *client = context;
	struct i2c_msg xfer[2];
	u8 buf[4];
	u16 reg_buf;
	int ret;

	reg_buf = cpu_to_be16(reg);
	xfer[0].addr = client->addr;
	xfer[0].len = sizeof(reg_buf);
	xfer[0].buf = (u8 *)&reg_buf;
	xfer[0].flags = 0;

	xfer[1].addr = client->addr;
	if (reg != NAU8360_RF000_DSP_COMM && reg != NAU8360_RF002_DSP_COMM)
		xfer[1].len = 2;
	else
		xfer[1].len = 4;
	xfer[1].buf = buf;
	xfer[1].flags = I2C_M_RD;

	ret = i2c_transfer(client->adapter, xfer, ARRAY_SIZE(xfer));
	if (ret < 0)
		return ret;
	else if (ret != ARRAY_SIZE(xfer))
		return -EIO;

	if (reg != NAU8360_RF000_DSP_COMM && reg != NAU8360_RF002_DSP_COMM)
		/* parse for Codec, 2 bytes value and endian big */
		*value = (buf[0] << 8) | buf[1];
	else
		/* parse for DSP, 4 bytes value and native */
		*value = buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);

	return 0;
}

static const struct regmap_config nau8360_regmap_config = {
	.reg_bits = NAU8360_REG_ADDR_LEN,
	.val_bits = NAU8360_REG_DATA_LEN,

	.max_register = NAU8360_REG_MAX,
	.readable_reg = nau8360_readable_reg,
	.writeable_reg = nau8360_writeable_reg,
	.volatile_reg = nau8360_volatile_reg,
	.reg_read = nau8360_reg_read,
	.reg_write = nau8360_reg_write,

	.cache_type = REGCACHE_MAPLE,
	.reg_defaults = nau8360_reg_defaults,
	.num_reg_defaults = ARRAY_SIZE(nau8360_reg_defaults),
};

static inline void nau8360_reset_chip(struct regmap *regmap)
{
	regmap_write(regmap, NAU8360_R00_SOFTWARE_RST, 0x5a5a);
	regmap_write(regmap, NAU8360_R00_SOFTWARE_RST, 0xa5a5);
}

static inline void nau8360_dsp_software_reset(struct snd_soc_component *component)
{
	/* Enable PLL for successful DSP reset. After DSP is alive,
	 * system clock switches to internal clock and disable PLL.
	 */
	snd_soc_component_update_bits(component, NAU8360_R72_PLL_CFG0,
		NAU8360_PD_PLL_MASK, NAU8360_PD_PLL_EN);
	msleep(50);
	snd_soc_component_write(component, NAU8360_R01_DSP_SOFTWARE_RST, 0x5a5a);
	snd_soc_component_write(component, NAU8360_R01_DSP_SOFTWARE_RST, 0xa5a5);
	snd_soc_component_set_sysclk(component, NAU8360_CLK_ID_INT, 0, 0,
		SND_SOC_CLOCK_IN);
}

static inline void nau8360_dsp_enable(struct regmap *regmap, bool enable)
{
	regmap_update_bits(regmap, NAU8360_R86_HW3_CTL0, NAU8360_HW3_STALL,
		enable ? 0 : NAU8360_HW3_STALL);
	regmap_update_bits(regmap, NAU8360_R1A_DSP_CORE_CTRL2, NAU8360_DSP_RUNSTALL,
		enable ? 0 : NAU8360_DSP_RUNSTALL);
}

static void nau8360_init_regs(struct nau8360 *nau8360)
{
	struct regmap *regmap = nau8360->regmap;

	/* Enable Digital LDO */
	regmap_write(regmap, NAU8360_R78_PD_SW_DLDO, NAU8360_PD_SW_DLDO_EN);
	/* Enable Software Shutdown Mode */
	regmap_write(regmap, NAU8360_R77_SOFT_SD, NAU8360_SOFT_SD_EN);
	/* Stall HW1 PEQ Engine and Clear DRAM to Zero */
	regmap_update_bits(regmap, NAU8360_R9D_PEQ_CTL, NAU8360_PEQ_STALL |
		NAU8360_HW1_MEM_CLEAR, NAU8360_PEQ_STALL | NAU8360_HW1_MEM_CLEAR);
	regmap_update_bits(regmap, NAU8360_R9D_PEQ_CTL, NAU8360_HW1_MEM_CLEAR, 0);
	/* Stall HW2 engine */
	regmap_update_bits(regmap, NAU8360_R90_HW2_CTL0, NAU8360_HW2_STALL,
		NAU8360_HW2_STALL);
	/* Stall HW3 engine and DSP processor */
	nau8360_dsp_enable(regmap, false);
	/* Enable Clock Gate */
	regmap_write(regmap, NAU8360_R7E_CLK_GATED_EN, NAU8360_CLK_GATED_EN);
	/* Latch I2C LSB Value */
	regmap_write(regmap, NAU8360_R02_I2C_ADDR, 0x0001);
	/* ANC Left/Right Channel as Slot 2/3 and switch */
	regmap_update_bits(regmap, NAU8360_R10_I2S_DATA_CTRL3,
		NAU8360_RX_ANC_R_MASK | NAU8360_RX_ANC_L_MASK,
		0x3 << NAU8360_RX_ANC_R_SFT | 0x2 << NAU8360_RX_ANC_L_SFT);
	/* Set DAC Clock Divider as 2 and Chopper Divider as 16 */
	regmap_update_bits(regmap, NAU8360_R71_CLK_DIV_CFG,
		NAU8360_DAC_CLK_DIV_MASK | NAU8360_DAC_CHOP_CLK_DIV_MASK,
		NAU8360_DAC_CLK_DIV_2 | NAU8360_DAC_CHOP_CLK_DIV_16);
	/* Set DAC SINC OSR as 128 and IVSENSE BS OSR as 32 */
	regmap_update_bits(regmap, NAU8360_R5D_SINC_CFG,
		NAU8360_DAC_SINC_OSR_MASK | NAU8360_IVSENSE_BS_OSR_MASK,
		NAU8360_DAC_SINC_OSR_128 | NAU8360_IVSENSE_BS_OSR_32);

	/* Set Trim Bit Control of DLDO and GVDD to The Highest Voltage */
	regmap_write(regmap, NAU8360_R5F_ANA_TRIM_CFG1, 0xf407);

	/* Disable Data Det and Clock Detection Settings*/
	regmap_update_bits(regmap, NAU8360_R40_CLK_DET_CTRL, NAU8360_APWRUPEN |
		NAU8360_CLKPWRUPEN | NAU8360_FS_MCLK_DET | NAU8360_FS_BCLK_DET, 0);

	/* Set HW3 Droop and Filter Sample as 192K */
	regmap_update_bits(regmap, NAU8360_R8C_HW3_CTL6, NAU8360_HW3_DROOP_MASK |
		NAU8360_HW3_FS_MASK, NAU8360_HW3_DROOP_192K | NAU8360_HW3_FS_192K);
	/* Set HW1 Mute, Mute Interval as 699ms and HW1 Zero THD as 0xff */
	regmap_update_bits(regmap, NAU8360_R9C_HW1_CTL2, NAU8360_MUTE_INTRVL_MASK |
		NAU8360_HW1_CH_MUTE | NAU8360_HW1_ZERO_THD_MASK,
		NAU8360_MUTE_INTRVL_699MS | NAU8360_HW1_CH_MUTE | 0xff);
	/* set HW2 droop with 192K*/
	regmap_update_bits(regmap, NAU8360_R96_HW2_CTL6, NAU8360_HW2_DROOP_SEL_MASK |
		NAU8360_HW2_DROOP_EN | NAU8360_HW2_FS_MASK,
		NAU8360_HW2_DROOP_SEL_LARGE | NAU8360_HW2_DROOP_EN | NAU8360_HW2_FS_192K);
	/* Set HW2 default volume */
	regmap_write(regmap, NAU8360_R97_HW2_CTL7, 0xbf66);
	regmap_write(regmap, NAU8360_R98_HW2_CTL8, 0xbf66);
	/* Set HW2 Mute and Threshold of Zero Crossing */
	regmap_update_bits(regmap, NAU8360_R99_HW2_CTL9, NAU8360_HW2_CH_MUTE |
		NAU8360_HW2_ZERO_THD_MASK, NAU8360_HW2_CH_MUTE | 0xff);
	/* According to DSP enabled or not, set stream path as
	 * HW1->DSP->HW2->SINC->HW3 or HW1->HW2->SINC->HW3
	 */
	regmap_update_bits(regmap, NAU8360_R12_PATH_CTRL, NAU8360_SEL_HW1_MASK |
		NAU8360_AUD_SEL_MASK | NAU8360_SEL_HW2_MASK | NAU8360_SEL_HW3_MASK |
		NAU8360_DAC_SEL_MASK, NAU8360_SEL_HW1_OUT | NAU8360_AUD_SEL_SINCOUT |
		NAU8360_SEL_HW2_OUT | NAU8360_SEL_HW3_OUT | (nau8360->dsp_enable ?
			NAU8360_DAC_SEL_DSP : NAU8360_DAC_SEL_BYP));
	/* Set Input Status as Low and Input/Output Mode Disable for All GPIO */
	regmap_write(regmap, NAU8360_R07_GP_CTRL, 0x0000);
	/* enable SARADC with extra average filter of VBAT */
	regmap_update_bits(regmap, NAU8360_R6A_SARADC_CFG0, NAU8360_SARADC_EN |
		NAU8360_VBAT_AVG_EN, NAU8360_SARADC_EN | NAU8360_VBAT_AVG_EN);
	/* Enable Temperature Sensor, VBATDIV and VRF.
	 * Set Average Filter Data as 512 for VTEMP and VBAT.
	 */
	regmap_update_bits(regmap, NAU8360_R6B_SARADC_CFG1, NAU8360_VTEMP_EN |
		NAU8360_VBATDIV_EN | NAU8360_VREF_EN | NAU8360_PRELOAD_VREF_EN |
		NAU8360_VTEMP_AVG_N_MASK | NAU8360_VBAT_AVG_N_MASK, NAU8360_VTEMP_EN |
		NAU8360_VBATDIV_EN | NAU8360_VREF_EN | NAU8360_PRELOAD_VREF_EN |
		NAU8360_VTEMP_AVG_N_512 | NAU8360_VBAT_AVG_N_512);
	/* Enable IV sense internal LDO */
	regmap_update_bits(regmap, NAU8360_R6C_IVSNS_CFG0,
		NAU8360_PD_LDO_SDM_IVSNS_PMD, 0);
	/* Enable Bias Current and Reference Voltage for IVSENSE. Set VSNS Gain
	 * as 1/12 and ISNS Gain as x16(24dB). Set PGA Current as 2'b11 and SDN
	 * Delay as 2'b00.
	 */
	regmap_write(regmap, NAU8360_R6D_IVSNS_CFG1, 0x0f5c);
	/* sawtooth clock from ivsense clock, PWM frequency 432 Khz */
	regmap_update_bits(regmap, NAU8360_RA4_ANA_REG_0, NAU8360_SEL_STCLK_MASK |
		NAU8360_NSEL_SAW_MASK, NAU8360_SEL_STCLK_IVCLK | 0x8);
	/* sawtooth PLL APR */
	regmap_update_bits(regmap, NAU8360_RA5_ANA_REG_1, NAU8360_SAW_PLL_MASK,
		NAU8360_SAW_PLL_NOR);

	/* Set DAC gain (0dB/+3.2dB) by current cell adjustment */
	regmap_update_bits(regmap, NAU8360_R6E_DAC_CFG0, NAU8360_DAC_CUR_MASK,
		nau8360->dac_cur_enable ? NAU8360_DAC_CUR_3_2DB : NAU8360_DAC_CUR_0DB);
	/* Config trim short current reference. Enable Non-overlap longer delay.
	 * Set segment driver as half driving strength.
	 */
	regmap_update_bits(regmap, NAU8360_R68_ANALOG_CONTROL_1, NAU8360_DTX_EN |
		NAU8360_TRIMSCR_MASK | NAU8360_DRVCTL_SEGL_FULL |
		NAU8360_DRVCTL_SEGR_FULL, NAU8360_TRIMSCR_MLOW | NAU8360_DTX_EN);
	/* Enable SCP Clear Mode and Class D Modulator to Common Mode */
	regmap_update_bits(regmap, NAU8360_R67_ANALOG_CONTROL_0,
		NAU8360_SCP_CLEAR_MODE_MASK | NAU8360_CM_COMP_EN,
		NAU8360_SCP_CLEAR_MODE_AUTO | NAU8360_CM_COMP_EN);
	/* Set Analog Mute and Class D Modulator Gain as 14dB */
	regmap_update_bits(regmap, NAU8360_R67_ANALOG_CONTROL_0, NAU8360_ANA_MUTE |
		NAU8360_MOD_GAIN_MASK, NAU8360_ANA_MUTE | NAU8360_MOD_GAIN_14DB);
	/* Set Stereo or PBTL Mode */
	regmap_update_bits(regmap, NAU8360_R67_ANALOG_CONTROL_0,
		NAU8360_V_PBTL_EN, nau8360->pbtl_enable ? NAU8360_V_PBTL_EN : 0);
	regmap_update_bits(regmap, NAU8360_R6C_IVSNS_CFG0,
		NAU8360_PBTL_ISENE_LR_MASK, NAU8360_PBTL_ISENE_LR_ILR);
	/* adjust HW3 default volume of voltage/current sense */
	regmap_write(regmap, NAU8360_R8A_HW3_VL_CTL7, 0xbe90);
	regmap_write(regmap, NAU8360_R8B_HW3_VR_CTL8, 0xbe90);
	regmap_write(regmap, NAU8360_R8D_HW3_IL_CTL7, nau8360->pbtl_enable ? 0xc3aa : 0xc24c);
	regmap_write(regmap, NAU8360_R8E_HW3_IR_CTL8, nau8360->pbtl_enable ? 0xc3aa : 0xc24c);
	/* Set HW3 Zero THD as 0xff */
	regmap_update_bits(regmap, NAU8360_R8F_HW3_CTL9,
		NAU8360_HW3_ZERO_THD_MASK, 0xff);
	/* Enable TX SDOUT and Data at BCLK Rising. */
	regmap_update_bits(regmap, NAU8360_R0D_I2S_PCM_CTRL3, NAU8360_TX_FILL_MASK,
		NAU8360_TX_FILL_ZERO);
	regmap_update_bits(regmap, NAU8360_R0D_I2S_PCM_CTRL3,
		NAU8360_VSNS_L_SLEN_MASK, NAU8360_VSNS_L_SLEN_16);
	regmap_update_bits(regmap, NAU8360_R0E_I2S_DATA_CTRL1,
		NAU8360_ISNS_L_SLEN_MASK, NAU8360_ISNS_L_SLEN_16);
	regmap_update_bits(regmap, NAU8360_R11_I2S_DATA_CTRL4,
		NAU8360_VSNS_R_SLEN_MASK | NAU8360_ISNS_R_SLEN_MASK,
		NAU8360_VSNS_R_SLEN_16 | NAU8360_ISNS_R_SLEN_16);
	/* set DAC channel temperature trim code slope as 0x7D */
	regmap_update_bits(regmap, NAU8360_R7A_DAC_TRIM_CFG2,
		NAU8360_DAC_TEMP_SLOPE_MASK, 0x7D << NAU8360_DAC_TEMP_SLOPE_SFT);
	/* set ISNS temperature trim code slope as 0x1a */
	regmap_update_bits(regmap, NAU8360_R7B_IVSNS_TRIM_CFG,
		NAU8360_ISNS_TEMP_SLOPE_MASK, 0x1a << NAU8360_ISNS_TEMP_SLOPE_SFT);
	/* set misc trim config as 0x67F0 */
	regmap_update_bits(regmap, NAU8360_R7C_MISC_TRIM_CFG, NAU8360_DAC_GAIN_SB_MASK |
		NAU8360_DAC_TEMP_SB_MASK | NAU8360_ISNS_TEMP_SB_MASK |
		NAU8360_VSNS_TEMP_SB_MASK | NAU8360_ISNS_GAIN_SB_MASK |
		NAU8360_VSNS_GAIN_SB_MASK, (0x3 << NAU8360_DAC_TEMP_SB_SFT) |
		(0x7 << NAU8360_ISNS_TEMP_SB_SFT) | (0x3 << NAU8360_VSNS_TEMP_SB_SFT));
	/* Enable Efuse Initianllization */
	regmap_update_bits(regmap, NAU8360_R60_RST, NAU8360_EFUSE_CTRL_EN,
		NAU8360_EFUSE_CTRL_EN);
	/* Clear HW2 DRAM */
	regmap_update_bits(regmap, NAU8360_R90_HW2_CTL0,
		NAU8360_HW2_DRAM_CLR, NAU8360_HW2_DRAM_CLR);
	/* Finish HW DRAM Clear */
	regmap_update_bits(regmap, NAU8360_R90_HW2_CTL0, NAU8360_HW2_DRAM_CLR, 0);
	/* Clear HW3 DRAM */
	regmap_update_bits(regmap, NAU8360_R86_HW3_CTL0,
		NAU8360_HW3_DRAM_CLR, NAU8360_HW3_DRAM_CLR);
	/* Finish HW DRAM Clear */
	regmap_update_bits(regmap, NAU8360_R86_HW3_CTL0, NAU8360_HW3_DRAM_CLR, 0);
}

static void nau8360_print_device_properties(struct nau8360 *nau8360)
{
	int i;

	dev_dbg(nau8360->dev, "dsp-bypass:          %d", !nau8360->dsp_enable);
	dev_dbg(nau8360->dev, "pbtl-enable:         %d", nau8360->pbtl_enable);
	dev_dbg(nau8360->dev, "dac-cur-enable:      %d", nau8360->dac_cur_enable);
	for (i = 0; i < nau8360->dsp_fws_num; i++)
		dev_dbg(nau8360->dev, "dsp-fw-names[%d]:     %s", i,
			nau8360->dsp_firmware[i]);
}

static void nau8360_read_device_properties(struct nau8360 *nau8360)
{
	const struct device_node *np = nau8360->dev->of_node;
	struct device *dev = nau8360->dev;
	int i, ret;

	nau8360->dsp_enable = !device_property_read_bool(dev, "nuvoton,dsp-bypass");
	nau8360->pbtl_enable = device_property_read_bool(dev, "nuvoton,pbtl-enable");
	nau8360->dac_cur_enable = device_property_read_bool(dev, "nuvoton,dac-cur-enable");

	ret = of_property_count_strings(np, "firmware-name");
	if (ret == NAU8360_DSP_FW_NUM) {
		nau8360->dsp_fws_num = ret;
		for (i = 0; i < nau8360->dsp_fws_num; i++) {
			ret = of_property_read_string_index(np, "firmware-name",
					i, &nau8360->dsp_firmware[i]);
			if (ret) {
				dev_err(dev, "Invalid firmware-name[%d]", i);
				nau8360->dsp_fws_num = 0;
				break;
			}
		}
	} else
		nau8360->dsp_fws_num = 0;
}

static struct reg_default *nau8360_alloc_defaults(struct device *dev, int *total_regs)
{
	struct reg_default *dyn_defaults;
	int reg_num = ARRAY_SIZE(nau8360_reg_defaults);
	int total = reg_num + (2 * NAU8360_TOT_BAND_PER_CH * NAU8360_TOT_BAND_COE);
	int i, j, idx;

	dyn_defaults = devm_kzalloc(dev, total * sizeof(*dyn_defaults), GFP_KERNEL);
	if (!dyn_defaults)
		return NULL;

	memcpy(dyn_defaults, nau8360_reg_defaults, sizeof(*dyn_defaults) * reg_num);
	idx = reg_num;

	for (i = 0; i < NAU8360_TOT_BAND_PER_CH; i++) {
		unsigned int l_base = NAU8360_R100_LEFT_BIQ0_COE +
					(i * NAU8360_TOT_BAND_COE_RANGE);
		unsigned int r_base = NAU8360_R200_RIGHT_BIQ0_COE +
					(i * NAU8360_TOT_BAND_COE_RANGE);

		for (j = 0; j < NAU8360_TOT_BAND_COE; j++) {
			dyn_defaults[idx++].reg = l_base + j;
			dyn_defaults[idx++].reg = r_base + j;
		}
	}

	*total_regs = total;

	return dyn_defaults;
}

static int nau8360_i2c_probe(struct i2c_client *i2c)
{
	struct device *dev = &i2c->dev;
	struct nau8360 *nau8360 = dev_get_platdata(dev);
	struct regmap_config regmap_cfg = nau8360_regmap_config;
	struct reg_default *dyn_defaults;
	int num_total_regs;
	int i, ret, value;

	if (!nau8360) {
		nau8360 = devm_kzalloc(dev, sizeof(*nau8360), GFP_KERNEL);
		if (!nau8360)
			return -ENOMEM;
	}
	i2c_set_clientdata(i2c, nau8360);

	dyn_defaults = nau8360_alloc_defaults(dev, &num_total_regs);
	if (!dyn_defaults)
		return -ENOMEM;

	regmap_cfg.reg_defaults = dyn_defaults;
	regmap_cfg.num_reg_defaults = num_total_regs;

	nau8360->regmap = devm_regmap_init(dev, NULL, i2c, &regmap_cfg);
	if (IS_ERR(nau8360->regmap))
		return PTR_ERR(nau8360->regmap);
	nau8360->dev = dev;

	nau8360_reset_chip(nau8360->regmap);
	ret = regmap_read(nau8360->regmap, NAU8360_R46_I2C_DEVICE_ID, &value);
	if (ret) {
		dev_err(dev, "Failed to read NAU83G60 device id %d",
			ret);
		return ret;
	}

	for (i = 0; i < NAU8360_TDM_TXN; i++)
		nau8360->tdm_tx_func_slot[i] = TDM_SLOT_NONE;
	for (i = 0; i < NAU8360_TDM_RXN; i++)
		nau8360->tdm_rx_func_slot[i] = TDM_SLOT_NONE;
	nau8360->tdm_chan_len = 32;
	nau8360->dsp_created = false;
	nau8360_read_device_properties(nau8360);
	nau8360_print_device_properties(nau8360);
	nau8360_init_regs(nau8360);

	return devm_snd_soc_register_component(dev, &soc_comp_dev_nau8360, &nau8360_dai, 1);
}

static const struct i2c_device_id nau8360_i2c_ids[] = {
	{ "nau8360", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, nau8360_i2c_ids);

#ifdef CONFIG_OF
static const struct of_device_id nau8360_of_ids[] = {
	{ .compatible = "nuvoton,nau8360", },
	{}
};
MODULE_DEVICE_TABLE(of, nau8360_of_ids);
#endif

#ifdef CONFIG_ACPI
static const struct acpi_device_id nau8360_acpi_match[] = {
	{"NVTN2002", 0,},
	{},
};
MODULE_DEVICE_TABLE(acpi, nau8360_acpi_match);
#endif

static struct i2c_driver nau8360_i2c_driver = {
	.driver = {
		.name = "nau8360",
		.of_match_table = of_match_ptr(nau8360_of_ids),
		.acpi_match_table = ACPI_PTR(nau8360_acpi_match),
	},
	.probe = nau8360_i2c_probe,
	.id_table = nau8360_i2c_ids,
};
module_i2c_driver(nau8360_i2c_driver);

MODULE_DESCRIPTION("ASoC NAU83G60 Stereo Class-D Amplifier with DSP and I/V-sense driver");
MODULE_AUTHOR("David Lin <ctlin0@nuvoton.com>");
MODULE_AUTHOR("Seven Lee <wtli@nuvoton.com>");
MODULE_AUTHOR("John Hsu <kchsu0@nuvoton.com>");
MODULE_AUTHOR("Neo Chang <ylchang2@nuvoton.com>");
MODULE_LICENSE("GPL");
