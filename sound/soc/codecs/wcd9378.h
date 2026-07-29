/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2026, Jorijn van der Graaf
 *
 * Register map for the Qualcomm WCD9378 audio codec.
 *
 * The codec exposes its control registers in the SoundWire SDCA control
 * address space (bit 30 set, SDCA function number in bits 25:22), accessed
 * through the TX SoundWire slave. The analog core registers (function 0,
 * implementation-defined region at +0x180000) are layout-compatible with
 * the WCD937x family; on top of that the chip adds SDCA functions
 * (SmartMIC0/1/2, SmartJACK, SmartAMP) whose sequencers drive the analog
 * power-up autonomously.
 */

#ifndef __WCD9378_H__
#define __WCD9378_H__

#include <linux/regmap.h>
#include <linux/soundwire/sdw.h>
#include <linux/soundwire/sdw_type.h>
#include <sound/soc.h>

/* SDCA function 0 (extension unit): device identity */
#define WCD9378_FUNC_EXT_ID_0			0x40000048
#define WCD9378_FUNC_EXT_ID_1			0x40000049
#define WCD9378_FUNC_EXT_VER			0x40000050
#define WCD9378_FUNC_STAT			0x40080000

/*
 * FUNC_STAT sticky (write-1-to-clear) bits are everything except the
 * live FUNCTION_BUSY flag, as in sound/soc/sdca.
 */
#define WCD9378_FUNC_STAT_STICKY_MASK		((u8)~SDCA_CTL_ENTITY_0_FUNCTION_BUSY)
#define WCD9378_DEV_MANU_ID_0			0x40100060
#define WCD9378_DEV_MANU_ID_1			0x40100061
#define WCD9378_DEV_PART_ID_0			0x40100068
#define WCD9378_DEV_PART_ID_1			0x40100069
#define WCD9378_DEV_VER				0x40100070

/* Analog core (WCD937x-compatible layout), function 0 + 0x180000 */
#define WCD9378_ANA_PAGE			0x40180000
#define WCD9378_ANA_BIAS			0x40180001
#define WCD9378_ANA_BIAS_ANALOG_BIAS_EN		BIT(7)
#define WCD9378_ANA_BIAS_PRECHRG_EN		BIT(6)
#define WCD9378_ANA_RX_SUPPLIES			0x40180008
#define WCD9378_ANA_TX_CH1			0x4018000e
#define WCD9378_ANA_TX_CH2			0x4018000f
#define WCD9378_ANA_TX_CH2_HPF1_INIT		BIT(6)
#define WCD9378_ANA_TX_CH2_HPF2_INIT		BIT(5)
#define WCD9378_ANA_TX_CH3			0x40180010
#define WCD9378_ANA_TX_CH3_HPF			0x40180011
#define WCD9378_ANA_TX_CH3_HPF3_INIT		BIT(6)
#define WCD9378_ANA_MICB1			0x40180022
#define WCD9378_ANA_MICB2			0x40180023
#define WCD9378_ANA_MICB2_RAMP			0x40180024
#define WCD9378_ANA_MICB2_RAMP_SHIFT_CTL_MASK	GENMASK(4, 2)
#define WCD9378_ANA_MICB2_RAMP_EN		BIT(7)
#define WCD9378_ANA_MICB3			0x40180025
#define WCD9378_BIAS_VBG_FINE_ADJ		0x40180029
#define WCD9378_MBHC_CTL_SPARE_1		0x40180058
#define WCD9378_MICB1_TEST_CTL_2		0x4018006c
#define WCD9378_MICB2_TEST_CTL_2		0x4018006f
#define WCD9378_MICB3_TEST_CTL_2		0x40180072
#define WCD9378_TX_COM_TXFE_DIV_CTL		0x4018007b
#define WCD9378_TX_COM_TXFE_DIV_SEQ_BYPASS	BIT(7)
#define WCD9378_SLEEP_CTL			0x40180103
#define WCD9378_SLEEP_CTL_BG_CTL_MASK		GENMASK(3, 1)
#define WCD9378_SLEEP_CTL_BG_EN			BIT(7)
#define WCD9378_SLEEP_CTL_LDOL_BG_SEL		BIT(6)
#define WCD9378_TX_NEW_CH12_MUX			0x4018012e
#define WCD9378_TX_NEW_CH12_MUX_CH1_SEL_MASK	GENMASK(2, 0)
#define WCD9378_TX_NEW_CH12_MUX_CH2_SEL_MASK	GENMASK(5, 3)
#define WCD9378_TX_NEW_CH34_MUX			0x4018012f
#define WCD9378_TX_NEW_CH34_MUX_CH3_SEL_MASK	GENMASK(2, 0)
#define WCD9378_HPH_RDAC_GAIN_CTL		0x40180132
#define WCD9378_HPH_RDAC_HD2_CTL_L		0x40180133
#define WCD9378_HPH_RDAC_HD2_CTL_R		0x40180136

/* Digital page */
#define WCD9378_TOP_CLK_CFG			0x40180407
#define WCD9378_CDC_ANA_TX_CLK_CTL		0x40180417
#define WCD9378_CDC_ANA_TXSCBIAS_CLK_EN		BIT(0)
#define WCD9378_CDC_AMIC_CTL			0x4018045a
#define WCD9378_PDM_WD_CTL0			0x40180465
#define WCD9378_PDM_WD_CTL1			0x40180466
#define WCD9378_EFUSE_REG_16			0x401804c0
#define WCD9378_EFUSE_REG_29			0x401804cd
#define WCD9378_PLATFORM_CTL			0x401804f0

/* Sequencer block (SEQR) */
#define WCD9378_SYS_USAGE_CTRL			0x40180501
#define WCD9378_SYS_USAGE_CTRL_MASK		GENMASK(3, 0)
#define WCD9378_HPH_UP_T0			0x40180510
#define WCD9378_HPH_UP_T9			0x40180519
#define WCD9378_HPH_DN_T0			0x4018051b
#define WCD9378_SEQ_TX0_STAT			0x40180592
#define WCD9378_SEQ_TX1_STAT			0x40180593
#define WCD9378_SEQ_TX2_STAT			0x40180594
#define WCD9378_MICB_REMAP_TABLE_VAL_3		0x401805a3
#define WCD9378_MICB_REMAP_TABLE_VAL_4		0x401805a4
#define WCD9378_MICB_REMAP_TABLE_VAL_5		0x401805a5
#define WCD9378_SM0_MB_SEL			0x401805b0
#define WCD9378_SM1_MB_SEL			0x401805b1
#define WCD9378_SM2_MB_SEL			0x401805b2
#define WCD9378_SM_MB_SEL_MASK			GENMASK(1, 0)
#define WCD9378_MB_PULLUP_EN			0x401805b3

/* SmartAMP SDCA function */
#define WCD9378_SMP_AMP_FUNC_STAT		0x40880000
#define WCD9378_SMP_AMP_FUNC_ACT		0x40880008

/* SmartJACK SDCA function (hosts ADC2 when fed from AMIC2) */
#define WCD9378_CMT_GRP_MASK			0x40c00008
#define WCD9378_SMP_JACK_IT31_MICB		0x40c00798
#define WCD9378_SMP_JACK_IT31_USAGE		0x40c007a0
#define WCD9378_SMP_JACK_PDE34_REQ_PS		0x40c00808
#define WCD9378_SMP_JACK_FUNC_STAT		0x40c80000
#define WCD9378_SMP_JACK_FUNC_ACT		0x40c80008
#define WCD9378_SMP_JACK_PDE34_ACT_PS		0x40c80800

/* SmartMIC0/1/2 SDCA functions (ADC1/ADC2/ADC3 sequencers) */
#define WCD9378_SMP_MIC_BASE(n)			(0x41000000 + (n) * 0x400000)
#define WCD9378_SMP_MIC_IT11_MICB(n)		(WCD9378_SMP_MIC_BASE(n) + 0x98)
#define WCD9378_SMP_MIC_IT11_USAGE(n)		(WCD9378_SMP_MIC_BASE(n) + 0xa0)
#define WCD9378_SMP_MIC_PDE11_REQ_PS(n)		(WCD9378_SMP_MIC_BASE(n) + 0x108)
#define WCD9378_SMP_MIC_OT10_USAGE(n)		(WCD9378_SMP_MIC_BASE(n) + 0x3a0)
#define WCD9378_SMP_MIC_FUNC_STAT(n)		(WCD9378_SMP_MIC_BASE(n) + 0x80000)
#define WCD9378_SMP_MIC_FUNC_ACT(n)		(WCD9378_SMP_MIC_BASE(n) + 0x80008)
#define WCD9378_SMP_MIC_PDE11_ACT_PS(n)		(WCD9378_SMP_MIC_BASE(n) + 0x80100)

#define WCD9378_MAX_REGISTER			0x41900070

/*
 * Vendor bus-interface registers of the slave itself: raw 16-bit
 * addresses in the direct (unpaged) SoundWire address space, written
 * with sdw_write() directly - distinct from the paged codec control
 * space all other registers in this file live in.
 */
#define WCD9378_SWRS_SCP_SDCA_INTRTYPE_1	0xf4
#define WCD9378_SWRS_SCP_SDCA_INTRTYPE_2	0xf8
#define WCD9378_SWRS_SCP_SDCA_INTRTYPE_3	0xfc

/* ITxx_USAGE ADC mode values */
#define WCD9378_ADC_USAGE_HIFI			0x01
#define WCD9378_ADC_USAGE_LO_HIF		0x02
#define WCD9378_ADC_USAGE_NORMAL		0x03
#define WCD9378_ADC_USAGE_LP			0x05
#define WCD9378_ADC_USAGE_OFF			0x00

/* ITxx_MICB usage values */
#define WCD9378_MICB_USAGE_OFF			0x00
#define WCD9378_MICB_USAGE_PULL_DOWN		0x01
#define WCD9378_MICB_USAGE_1P2V			0x02
#define WCD9378_MICB_USAGE_1P8V_OR_PULLUP	0x03
#define WCD9378_MICB_USAGE_2P5V			0x04
#define WCD9378_MICB_USAGE_2P75V		0x05
#define WCD9378_MICB_USAGE_2P2V			0xf0
#define WCD9378_MICB_USAGE_2P7V			0xf1
#define WCD9378_MICB_USAGE_2P8V			0xf2
#define WCD9378_MICB_USAGE_REMAP_TABLE_3	0xf3
#define WCD9378_MICB_USAGE_REMAP_TABLE_4	0xf4
#define WCD9378_MICB_USAGE_REMAP_TABLE_5	0xf5

/* PDExx_REQ_PS power states */
#define WCD9378_PDE_PS0_ON			0x00
#define WCD9378_PDE_PS3_OFF			0x03

#define WCD9378_MAX_MICBIAS			3
#define WCD9378_MAX_SWR_CH_IDS			15

enum wcd9378_tx_sdw_ports {
	WCD9378_ADC_1_PORT = 1,
	WCD9378_ADC_2_PORT,
	WCD9378_ADC_3_PORT,
	WCD9378_DMIC_0_1_MBHC_PORT,
	WCD9378_DMIC_2_5_PORT,
	WCD9378_MAX_TX_SWR_PORTS = WCD9378_DMIC_2_5_PORT,
};

enum wcd9378_rx_sdw_ports {
	WCD9378_HPH_PORT = 1,
	WCD9378_CLSH_PORT,
	WCD9378_COMP_PORT,
	WCD9378_LO_PORT,
	WCD9378_DSD_PORT,
	WCD9378_MAX_SWR_PORTS = WCD9378_DSD_PORT,
};

enum wcd9378_tx_sdw_channels {
	WCD9378_ADC1,
	WCD9378_ADC2,
	WCD9378_ADC3,
	WCD9378_DMIC0,
	WCD9378_DMIC1,
	WCD9378_MBHC,
	WCD9378_DMIC2,
	WCD9378_DMIC3,
	WCD9378_DMIC4,
	WCD9378_DMIC5,
};

enum wcd9378_rx_sdw_channels {
	WCD9378_HPH_L,
	WCD9378_HPH_R,
	WCD9378_CLSH,
	WCD9378_COMP_L,
	WCD9378_COMP_R,
	WCD9378_LO,
	WCD9378_DSD_L,
	WCD9378_DSD_R,
};

struct wcd9378_priv;
struct wcd9378_sdw_priv {
	struct sdw_slave *sdev;
	struct sdw_stream_config sconfig;
	struct sdw_stream_runtime *sruntime;
	struct sdw_port_config port_config[WCD9378_MAX_SWR_PORTS];
	const struct wcd_sdw_ch_info *ch_info;
	bool port_enable[WCD9378_MAX_SWR_CH_IDS];
	unsigned int master_channel_map[SDW_MAX_PORTS];
	int active_ports;
	bool is_tx;
	struct wcd9378_priv *wcd9378;
	struct regmap *regmap;
};

#if IS_ENABLED(CONFIG_SND_SOC_WCD9378_SDW)
int wcd9378_sdw_hw_params(struct wcd9378_sdw_priv *wcd,
			  struct snd_pcm_substream *substream,
			  struct snd_pcm_hw_params *params,
			  struct snd_soc_dai *dai);
#else
static inline int wcd9378_sdw_hw_params(struct wcd9378_sdw_priv *wcd,
					struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params,
					struct snd_soc_dai *dai)
{
	return -EOPNOTSUPP;
}
#endif

#endif /* __WCD9378_H__ */
