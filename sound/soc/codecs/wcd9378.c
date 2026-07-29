// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026, Jorijn van der Graaf
 *
 * Qualcomm WCD9378 audio codec driver.
 *
 * The WCD9378 pairs a WCD937x-compatible analog core with SDCA
 * function blocks (SmartMIC0/1/2, SmartJACK, SmartAMP) whose built-in
 * sequencers perform the analog power-up/down autonomously: capture is
 * started by programming the ADC usage mode (ITxx_USAGE), requesting
 * power state 0 on the function's PDE, and letting the sequencer ramp
 * the micbias selected through SMx_MB_SEL. The sequencers are a vendor
 * layer behind the SDCA-shaped controls: a power-state request starts
 * an autonomous multi-step analog program, rather than the plain byte
 * accesses of generic SDCA.
 *
 * TX/capture paths only for now; RX (earpiece/headphone), MBHC and the
 * SmartAMP function are not implemented.
 */

#include <linux/component.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/soundwire/sdw.h>
#include <linux/soundwire/sdw_registers.h>
#include <linux/soundwire/sdw_type.h>
#include <sound/sdca_function.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/tlv.h>

#include "wcd-common.h"
#include "wcd9378.h"

enum {
	AIF1_PB = 0,
	AIF1_CAP,
	NUM_CODEC_DAIS,
};

enum {
	MIC_BIAS_1 = 1,
	MIC_BIAS_2,
	MIC_BIAS_3,
};

enum {
	MICB_PULLUP_ENABLE,
	MICB_PULLUP_DISABLE,
	MICB_ENABLE,
	MICB_DISABLE,
};

/*
 * sys-usage capability bits. SYS_USAGE_CTRL is a vendor register in
 * the sequencer block (not an SDCA control) selecting one of 13 canned
 * "active entity set" profiles; these are the capabilities a profile
 * can advertise, and wcd9378_sys_usage_profiles[] below maps each
 * profile to the set it provides. Names from the downstream driver.
 */
enum {
	WCD9378_SYS_USAGE_CLASS_AB = 0,
	WCD9378_SYS_USAGE_TX1_FOR_JACK,
	WCD9378_SYS_USAGE_TX2_AMIC4,
	WCD9378_SYS_USAGE_TX2_AMIC1,
	WCD9378_SYS_USAGE_TX1_AMIC3,
	WCD9378_SYS_USAGE_TX1_AMIC2,
	WCD9378_SYS_USAGE_TX0_AMIC2,
	WCD9378_SYS_USAGE_TX0_AMIC1,
	WCD9378_SYS_USAGE_RX2_EAR,
	WCD9378_SYS_USAGE_RX2_AUX,
	WCD9378_SYS_USAGE_RX1_AUX,
	WCD9378_SYS_USAGE_RX0_EAR,
	WCD9378_SYS_USAGE_RX0_RX1_HPH,
};

/* Capability sets of the 13 canned SYS_USAGE_CTRL profiles */
static const unsigned int wcd9378_sys_usage_profiles[] = {
	0x0c95, 0x12a7, 0x0c99, 0x1aab, 0x0894, 0x11a6, 0x0898,
	0x11ab, 0x126a, 0x116b, 0x1ca7, 0x1195, 0x1296,
};

struct wcd9378_priv {
	struct sdw_slave *tx_sdw_dev;
	struct wcd9378_sdw_priv *sdw_priv[NUM_CODEC_DAIS];
	struct device *txdev;
	struct device *rxdev;
	struct device_node *rxnode;
	struct device_node *txnode;
	struct regmap *regmap;
	struct wcd_common common;
	/* micbias refcount lock */
	struct mutex micb_lock;
	u8 micb_usage_val[WCD9378_MAX_MICBIAS];
	int micb_ref[WCD9378_MAX_MICBIAS];
	int pullup_ref[WCD9378_MAX_MICBIAS];
	unsigned long sys_usage_mask;
	/* per-ADC profile bit and target function, latched at power-up */
	int tx_sys_bit[3];
	bool tx_is_jack[3];
	u32 tx_mode[3];
	struct gpio_desc *reset_gpio;
};

static const char * const wcd9378_supplies[] = {
	"vdd-rxtx", "vdd-io", "vdd-buck", "vdd-mic-bias",
};

/* SDCA function block registers driving one ADC */
struct wcd9378_smp_fn {
	u32 usage_reg;
	u32 micb_reg;
	u32 req_reg;
	u32 act_reg;
	u32 hpf_reg;
	u8 hpf_mask;
};

/* ADC1/2/3 through SmartMIC0/1/2 */
static const struct wcd9378_smp_fn wcd9378_smp_mic[] = {
	{
		.usage_reg = WCD9378_SMP_MIC_IT11_USAGE(0),
		.micb_reg = WCD9378_SMP_MIC_IT11_MICB(0),
		.req_reg = WCD9378_SMP_MIC_PDE11_REQ_PS(0),
		.act_reg = WCD9378_SMP_MIC_PDE11_ACT_PS(0),
		.hpf_reg = WCD9378_ANA_TX_CH2,
		.hpf_mask = WCD9378_ANA_TX_CH2_HPF1_INIT,
	}, {
		.usage_reg = WCD9378_SMP_MIC_IT11_USAGE(1),
		.micb_reg = WCD9378_SMP_MIC_IT11_MICB(1),
		.req_reg = WCD9378_SMP_MIC_PDE11_REQ_PS(1),
		.act_reg = WCD9378_SMP_MIC_PDE11_ACT_PS(1),
		.hpf_reg = WCD9378_ANA_TX_CH2,
		.hpf_mask = WCD9378_ANA_TX_CH2_HPF2_INIT,
	}, {
		.usage_reg = WCD9378_SMP_MIC_IT11_USAGE(2),
		.micb_reg = WCD9378_SMP_MIC_IT11_MICB(2),
		.req_reg = WCD9378_SMP_MIC_PDE11_REQ_PS(2),
		.act_reg = WCD9378_SMP_MIC_PDE11_ACT_PS(2),
		.hpf_reg = WCD9378_ANA_TX_CH3_HPF,
		.hpf_mask = WCD9378_ANA_TX_CH3_HPF3_INIT,
	},
};

/* ADC2 fed from AMIC2 runs through the SmartJACK function instead */
static const struct wcd9378_smp_fn wcd9378_smp_jack_adc2 = {
	.usage_reg = WCD9378_SMP_JACK_IT31_USAGE,
	.micb_reg = WCD9378_SMP_JACK_IT31_MICB,
	.req_reg = WCD9378_SMP_JACK_PDE34_REQ_PS,
	.act_reg = WCD9378_SMP_JACK_PDE34_ACT_PS,
};

/*
 * Measured acoustically: the TX gain field steps 1.5 dB per code
 * (+6 dB per 4 codes, +30 dB over the full 0..20 range), as does
 * mainline wcd938x; the wcd937x and wcd939x drivers claim 0.25 dB
 * steps for the same field.
 */
static const DECLARE_TLV_DB_SCALE(analog_gain, 0, 150, 0);

static void wcd9378_reset(struct wcd9378_priv *wcd9378)
{
	gpiod_set_value(wcd9378->reset_gpio, 1);
	usleep_range(20, 30);
	gpiod_set_value(wcd9378->reset_gpio, 0);
	usleep_range(20, 30);
}

/*
 * Activate the SDCA function classes. Without FUNC_ACT the sequencer
 * ignores all PDE power-state requests.
 */
static void wcd9378_class_load(struct snd_soc_component *component)
{
	int i;

	/*
	 * Plain writes, not update_bits, so the 0->1 activation edge
	 * always reaches the hardware regardless of regcache state.
	 * The engine boots from this edge only on a freshly reset
	 * codec; once it dies (bus clock-stop) no register write
	 * revives it, see the TX bus PM hold in wcd9378_bind().
	 */
	/* SmartAMP: activation edge, class-load delay, clear the stickies */
	snd_soc_component_write(component, WCD9378_SMP_AMP_FUNC_ACT, 0x00);
	snd_soc_component_write(component, WCD9378_SMP_AMP_FUNC_ACT, 0x01);
	usleep_range(20000, 20010);
	snd_soc_component_write(component, WCD9378_SMP_AMP_FUNC_STAT,
				WCD9378_FUNC_STAT_STICKY_MASK);

	/*
	 * SmartJACK: same steps, plus the commit-group assignment the
	 * downstream driver makes during its class load.
	 */
	snd_soc_component_write(component, WCD9378_SMP_JACK_FUNC_ACT, 0x00);
	snd_soc_component_write(component, WCD9378_SMP_JACK_FUNC_ACT, 0x01);
	usleep_range(30000, 30010);
	snd_soc_component_update_bits(component, WCD9378_CMT_GRP_MASK,
				      0xff, 0x02);
	snd_soc_component_write(component, WCD9378_SMP_JACK_FUNC_STAT,
				WCD9378_FUNC_STAT_STICKY_MASK);

	/* SmartMIC0/1/2: same steps per mic function */
	for (i = 0; i < 3; i++) {
		snd_soc_component_write(component,
					WCD9378_SMP_MIC_FUNC_ACT(i), 0x00);
		snd_soc_component_write(component,
					WCD9378_SMP_MIC_FUNC_ACT(i), 0x01);
		usleep_range(5000, 5010);
		snd_soc_component_write(component,
					WCD9378_SMP_MIC_FUNC_STAT(i),
					WCD9378_FUNC_STAT_STICKY_MASK);
	}
}

static void wcd9378_io_init(struct snd_soc_component *component)
{
	u32 efuse;

	/* Bandgap and analog master bias, with precharge pulse */
	efuse = snd_soc_component_read(component, WCD9378_EFUSE_REG_16);
	snd_soc_component_update_bits(component, WCD9378_MBHC_CTL_SPARE_1,
				      0x03, efuse == 0 ? 0x03 : 0x01);
	snd_soc_component_update_bits(component, WCD9378_SLEEP_CTL,
				      WCD9378_SLEEP_CTL_BG_CTL_MASK, 0x0e);
	snd_soc_component_update_bits(component, WCD9378_SLEEP_CTL,
				      WCD9378_SLEEP_CTL_BG_EN,
				      WCD9378_SLEEP_CTL_BG_EN);
	usleep_range(1000, 1010);
	snd_soc_component_update_bits(component, WCD9378_SLEEP_CTL,
				      WCD9378_SLEEP_CTL_LDOL_BG_SEL,
				      WCD9378_SLEEP_CTL_LDOL_BG_SEL);
	usleep_range(1000, 1010);
	snd_soc_component_update_bits(component, WCD9378_BIAS_VBG_FINE_ADJ,
				      0xf0, 0xb0);
	snd_soc_component_update_bits(component, WCD9378_ANA_BIAS,
				      WCD9378_ANA_BIAS_ANALOG_BIAS_EN,
				      WCD9378_ANA_BIAS_ANALOG_BIAS_EN);
	snd_soc_component_update_bits(component, WCD9378_ANA_BIAS,
				      WCD9378_ANA_BIAS_PRECHRG_EN,
				      WCD9378_ANA_BIAS_PRECHRG_EN);
	usleep_range(10000, 10010);
	snd_soc_component_update_bits(component, WCD9378_ANA_BIAS,
				      WCD9378_ANA_BIAS_PRECHRG_EN, 0x00);

	/* TX supporting clocks/dividers */
	snd_soc_component_update_bits(component, WCD9378_CDC_ANA_TX_CLK_CTL,
				      WCD9378_CDC_ANA_TXSCBIAS_CLK_EN,
				      WCD9378_CDC_ANA_TXSCBIAS_CLK_EN);
	snd_soc_component_update_bits(component, WCD9378_TX_COM_TXFE_DIV_CTL,
				      WCD9378_TX_COM_TXFE_DIV_SEQ_BYPASS,
				      WCD9378_TX_COM_TXFE_DIV_SEQ_BYPASS);
	snd_soc_component_update_bits(component, WCD9378_PDM_WD_CTL0,
				      0x18, 0x10);
	snd_soc_component_update_bits(component, WCD9378_PDM_WD_CTL1,
				      0x18, 0x10);

	/* Micbias LDO driver bias */
	snd_soc_component_update_bits(component, WCD9378_MICB1_TEST_CTL_2,
				      0x07, 0x01);
	snd_soc_component_update_bits(component, WCD9378_MICB2_TEST_CTL_2,
				      0x07, 0x01);
	snd_soc_component_update_bits(component, WCD9378_MICB3_TEST_CTL_2,
				      0x07, 0x01);

	/* RX defaults (harmless while RX is unimplemented) */
	snd_soc_component_update_bits(component, WCD9378_HPH_RDAC_HD2_CTL_L,
				      0x0f, 0x04);
	snd_soc_component_update_bits(component, WCD9378_HPH_RDAC_HD2_CTL_R,
				      0x0f, 0x04);
	snd_soc_component_update_bits(component, WCD9378_HPH_RDAC_GAIN_CTL,
				      0xf0, 0x50);
	snd_soc_component_update_bits(component, WCD9378_HPH_UP_T0,
				      0x07, 0x05);
	snd_soc_component_update_bits(component, WCD9378_HPH_UP_T9,
				      0x07, 0x05);
	snd_soc_component_update_bits(component, WCD9378_HPH_DN_T0,
				      0x07, 0x06);

	/* SmartMIC function N powers the micbias SMx_MB_SEL points at */
	snd_soc_component_update_bits(component, WCD9378_SM0_MB_SEL,
				      WCD9378_SM_MB_SEL_MASK, 0x01);
	snd_soc_component_update_bits(component, WCD9378_SM1_MB_SEL,
				      WCD9378_SM_MB_SEL_MASK, 0x02);
	snd_soc_component_update_bits(component, WCD9378_SM2_MB_SEL,
				      WCD9378_SM_MB_SEL_MASK, 0x03);
	snd_soc_component_update_bits(component, WCD9378_SYS_USAGE_CTRL,
				      WCD9378_SYS_USAGE_CTRL_MASK, 0x00);

	wcd9378_class_load(component);
}

/*
 * Derive the sys-usage capability bit for an ADC from its input mux.
 * Only combinations the canned profiles cover are usable.
 */
static int wcd9378_sys_usage_bit_get(struct snd_soc_component *component,
				     int adc, bool *is_jack)
{
	u32 val;

	*is_jack = false;

	switch (adc) {
	case 0:
		val = snd_soc_component_read(component, WCD9378_TX_NEW_CH12_MUX) &
			WCD9378_TX_NEW_CH12_MUX_CH1_SEL_MASK;
		if (val == 0x01)
			return WCD9378_SYS_USAGE_TX0_AMIC1;
		if (val == 0x02)
			return WCD9378_SYS_USAGE_TX0_AMIC2;
		break;
	case 1:
		val = (snd_soc_component_read(component, WCD9378_TX_NEW_CH12_MUX) &
			WCD9378_TX_NEW_CH12_MUX_CH2_SEL_MASK) >> 3;
		if (val == 0x02) {
			*is_jack = true;
			return WCD9378_SYS_USAGE_TX1_AMIC2;
		}
		if (val == 0x03)
			return WCD9378_SYS_USAGE_TX1_AMIC3;
		break;
	case 2:
		val = snd_soc_component_read(component, WCD9378_TX_NEW_CH34_MUX) &
			WCD9378_TX_NEW_CH34_MUX_CH3_SEL_MASK;
		if (val == 0x01)
			return WCD9378_SYS_USAGE_TX2_AMIC1;
		if (val == 0x03)
			return WCD9378_SYS_USAGE_TX2_AMIC4;
		break;
	}

	dev_err(component->dev,
		"ADC%d input mux selection not supported by any sys-usage profile\n",
		adc + 1);
	return -EINVAL;
}

static int wcd9378_sys_usage_update(struct snd_soc_component *component,
				    int bit, bool enable)
{
	struct wcd9378_priv *wcd9378 = snd_soc_component_get_drvdata(component);
	int i;

	if (!enable) {
		clear_bit(bit, &wcd9378->sys_usage_mask);
		return 0;
	}

	set_bit(bit, &wcd9378->sys_usage_mask);

	for (i = 0; i < ARRAY_SIZE(wcd9378_sys_usage_profiles); i++) {
		if ((wcd9378_sys_usage_profiles[i] & wcd9378->sys_usage_mask) ==
		    wcd9378->sys_usage_mask)
			break;
	}

	if (i == ARRAY_SIZE(wcd9378_sys_usage_profiles)) {
		clear_bit(bit, &wcd9378->sys_usage_mask);
		dev_err(component->dev,
			"no sys-usage profile covers active paths (mask %#lx)\n",
			wcd9378->sys_usage_mask);
		return -EINVAL;
	}

	snd_soc_component_update_bits(component, WCD9378_SYS_USAGE_CTRL,
				      WCD9378_SYS_USAGE_CTRL_MASK, i);

	return 0;
}

static u32 wcd9378_get_mode_val(struct wcd9378_priv *wcd9378, int adc)
{
	switch (wcd9378->tx_mode[adc]) {
	case 1:
		return WCD9378_ADC_USAGE_HIFI;
	case 2:
		return WCD9378_ADC_USAGE_LO_HIF;
	case 4:
		return WCD9378_ADC_USAGE_LP;
	case 0: /* ADC_INVALID (unset) */
	case 3:
	default:
		return WCD9378_ADC_USAGE_NORMAL;
	}
}

/* Actual bus clock is half the SoundWire double-rate frequency */
static unsigned int wcd9378_tx_bus_clk(struct wcd9378_priv *wcd9378)
{
	return wcd9378->tx_sdw_dev->bus->params.curr_dr_freq / 2;
}

static int wcd9378_tx_sequencer_enable(struct snd_soc_dapm_widget *w,
				       struct snd_kcontrol *kcontrol,
				       int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);
	struct wcd9378_priv *wcd9378 = snd_soc_component_get_drvdata(component);
	const struct wcd9378_smp_fn *fn;
	int adc = w->shift;
	bool is_jack = false;
	int sys_bit, retries;
	u32 val;

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		sys_bit = wcd9378_sys_usage_bit_get(component, adc, &is_jack);
		if (sys_bit < 0)
			return sys_bit;

		/*
		 * Latch the profile bit and the target function: the input
		 * mux can be rewritten while the path is powered, and
		 * power-down must tear down what was actually powered up.
		 */
		wcd9378->tx_sys_bit[adc] = sys_bit;
		wcd9378->tx_is_jack[adc] = is_jack;
		if (is_jack)
			fn = &wcd9378_smp_jack_adc2;
		else
			fn = &wcd9378_smp_mic[adc];

		if (wcd9378_sys_usage_update(component, sys_bit, true))
			return -EINVAL;

		/*
		 * NORMAL/HIFI ADC modes need a 9.6 MHz bus clock; on a
		 * 4.8 MHz bus only the LP mode is valid and anything else
		 * makes the sequencer refuse to power up.
		 */
		if (wcd9378_tx_bus_clk(wcd9378) < 9600000)
			val = WCD9378_ADC_USAGE_LP;
		else
			val = wcd9378_get_mode_val(wcd9378, adc);

		snd_soc_component_update_bits(component, fn->usage_reg, 0xff, val);
		if (fn->hpf_reg)
			snd_soc_component_update_bits(component, fn->hpf_reg,
						      fn->hpf_mask, fn->hpf_mask);
		snd_soc_component_update_bits(component, fn->req_reg, 0xff,
					      WCD9378_PDE_PS0_ON);
		usleep_range(800, 810);

		if (fn->hpf_reg)
			snd_soc_component_update_bits(component, fn->hpf_reg,
						      fn->hpf_mask, 0x00);

		/* Wait for the sequencer to reach PS0 */
		retries = 20;
		do {
			val = snd_soc_component_read(component, fn->act_reg);
			if (val == WCD9378_PDE_PS0_ON)
				break;
			usleep_range(500, 510);
		} while (--retries);
		if (val != WCD9378_PDE_PS0_ON)
			dev_warn(component->dev,
				 "TX%d sequencer not in PS0 (act_ps %#x, bus %u Hz)\n",
				 adc, val,
				 wcd9378->tx_sdw_dev->bus->params.curr_dr_freq);
		break;
	case SND_SOC_DAPM_POST_PMD:
		sys_bit = wcd9378->tx_sys_bit[adc];
		if (sys_bit < 0)
			break;
		if (wcd9378->tx_is_jack[adc])
			fn = &wcd9378_smp_jack_adc2;
		else
			fn = &wcd9378_smp_mic[adc];

		snd_soc_component_update_bits(component, fn->usage_reg, 0xff,
					      WCD9378_ADC_USAGE_OFF);
		if (fn->hpf_reg)
			snd_soc_component_update_bits(component, fn->hpf_reg,
						      fn->hpf_mask, 0x00);
		snd_soc_component_update_bits(component, fn->req_reg, 0xff,
					      WCD9378_PDE_PS3_OFF);
		usleep_range(800, 810);
		wcd9378_sys_usage_update(component, sys_bit, false);
		wcd9378->tx_sys_bit[adc] = -1;
		break;
	}

	return 0;
}

static int wcd9378_micbias_control(struct snd_soc_component *component,
				   int micb_num, int req)
{
	struct wcd9378_priv *wcd9378 = snd_soc_component_get_drvdata(component);
	int mb_index = micb_num - 1;
	u32 usage_reg;
	u8 usage_val;
	u8 pullup_bit;

	if (micb_num < MIC_BIAS_1 || micb_num > MIC_BIAS_3)
		return -EINVAL;

	usage_reg = wcd9378_smp_mic[mb_index].micb_reg;
	usage_val = wcd9378->micb_usage_val[mb_index];
	pullup_bit = BIT(mb_index);

	mutex_lock(&wcd9378->micb_lock);

	switch (req) {
	case MICB_ENABLE:
		wcd9378->micb_ref[mb_index]++;
		if (wcd9378->micb_ref[mb_index] == 1) {
			if (micb_num == MIC_BIAS_2) {
				snd_soc_component_update_bits(component,
							      WCD9378_ANA_MICB2_RAMP,
							      WCD9378_ANA_MICB2_RAMP_SHIFT_CTL_MASK,
							      0x0c);
				snd_soc_component_update_bits(component,
							      WCD9378_ANA_MICB2_RAMP,
							      WCD9378_ANA_MICB2_RAMP_EN, 0x00);
			}
			snd_soc_component_update_bits(component, usage_reg,
						      0xff, usage_val);
			if (micb_num == MIC_BIAS_2)
				snd_soc_component_update_bits(component,
							      WCD9378_SMP_JACK_IT31_MICB,
							      0xff, usage_val);
		}
		break;
	case MICB_DISABLE:
		if (wcd9378->micb_ref[mb_index] > 0)
			wcd9378->micb_ref[mb_index]--;
		if (wcd9378->micb_ref[mb_index] == 0 &&
		    wcd9378->pullup_ref[mb_index] > 0) {
			snd_soc_component_update_bits(component,
						      WCD9378_MB_PULLUP_EN,
						      pullup_bit, pullup_bit);
			snd_soc_component_update_bits(component, usage_reg, 0xff,
						      WCD9378_MICB_USAGE_1P8V_OR_PULLUP);
			if (micb_num == MIC_BIAS_2)
				snd_soc_component_update_bits(component,
							      WCD9378_SMP_JACK_IT31_MICB, 0xff,
							      WCD9378_MICB_USAGE_1P8V_OR_PULLUP);
		} else if (wcd9378->micb_ref[mb_index] == 0) {
			snd_soc_component_update_bits(component, usage_reg,
						      0xff, WCD9378_MICB_USAGE_OFF);
			if (micb_num == MIC_BIAS_2) {
				snd_soc_component_update_bits(component,
							      WCD9378_SMP_JACK_IT31_MICB,
							      0xff, WCD9378_MICB_USAGE_OFF);
				snd_soc_component_update_bits(component,
							      WCD9378_ANA_MICB2_RAMP,
							      WCD9378_ANA_MICB2_RAMP_SHIFT_CTL_MASK,
							      0x0c);
				snd_soc_component_update_bits(component,
							      WCD9378_ANA_MICB2_RAMP,
							      WCD9378_ANA_MICB2_RAMP_EN,
							      WCD9378_ANA_MICB2_RAMP_EN);
			}
		}
		break;
	case MICB_PULLUP_ENABLE:
		wcd9378->pullup_ref[mb_index]++;
		if (wcd9378->pullup_ref[mb_index] == 1 &&
		    wcd9378->micb_ref[mb_index] == 0) {
			snd_soc_component_update_bits(component,
						      WCD9378_MB_PULLUP_EN,
						      pullup_bit, pullup_bit);
			snd_soc_component_update_bits(component, usage_reg, 0xff,
						      WCD9378_MICB_USAGE_1P8V_OR_PULLUP);
			if (micb_num == MIC_BIAS_2)
				snd_soc_component_update_bits(component,
							      WCD9378_SMP_JACK_IT31_MICB, 0xff,
							      WCD9378_MICB_USAGE_1P8V_OR_PULLUP);
		}
		break;
	case MICB_PULLUP_DISABLE:
		if (wcd9378->pullup_ref[mb_index] > 0)
			wcd9378->pullup_ref[mb_index]--;
		if (wcd9378->pullup_ref[mb_index] == 0 &&
		    wcd9378->micb_ref[mb_index] == 0) {
			snd_soc_component_update_bits(component,
						      WCD9378_MB_PULLUP_EN, pullup_bit, 0x00);
			snd_soc_component_update_bits(component, usage_reg, 0xff,
						      WCD9378_MICB_USAGE_PULL_DOWN);
			if (micb_num == MIC_BIAS_2)
				snd_soc_component_update_bits(component,
							      WCD9378_SMP_JACK_IT31_MICB, 0xff,
							      WCD9378_MICB_USAGE_PULL_DOWN);
		}
		break;
	}

	mutex_unlock(&wcd9378->micb_lock);

	return 0;
}

static int wcd9378_codec_enable_micbias(struct snd_soc_dapm_widget *w,
					struct snd_kcontrol *kcontrol,
					int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);
	int micb_num = w->shift;

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		wcd9378_micbias_control(component, micb_num, MICB_ENABLE);
		break;
	case SND_SOC_DAPM_POST_PMU:
		usleep_range(1000, 1100);
		break;
	case SND_SOC_DAPM_POST_PMD:
		wcd9378_micbias_control(component, micb_num, MICB_DISABLE);
		break;
	}

	return 0;
}

static int wcd9378_codec_enable_micbias_pullup(struct snd_soc_dapm_widget *w,
					       struct snd_kcontrol *kcontrol,
					       int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);
	int micb_num = w->shift;

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		wcd9378_micbias_control(component, micb_num,
					MICB_PULLUP_ENABLE);
		break;
	case SND_SOC_DAPM_POST_PMU:
		usleep_range(1000, 1100);
		break;
	case SND_SOC_DAPM_POST_PMD:
		wcd9378_micbias_control(component, micb_num,
					MICB_PULLUP_DISABLE);
		break;
	}

	return 0;
}

static int wcd9378_connect_port(struct wcd9378_sdw_priv *wcd, u8 port_idx,
				u8 ch_id, bool enable)
{
	struct sdw_port_config *port_config = &wcd->port_config[port_idx - 1];
	const struct wcd_sdw_ch_info *ch_info = &wcd->ch_info[ch_id];
	u8 port_num = ch_info->port_num;
	u8 ch_mask = ch_info->ch_mask;
	u8 mstr_port_num, mstr_ch_mask;
	struct sdw_slave *sdev = wcd->sdev;

	port_config->num = port_num;

	mstr_port_num = sdev->m_port_map[port_num];
	mstr_ch_mask = ch_info->master_ch_mask;

	if (enable) {
		port_config->ch_mask |= ch_mask;
		wcd->master_channel_map[mstr_port_num] |= mstr_ch_mask;
	} else {
		port_config->ch_mask &= ~ch_mask;
		wcd->master_channel_map[mstr_port_num] &= ~mstr_ch_mask;
	}

	return 0;
}

static int wcd9378_get_swr_port(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct soc_mixer_control *mixer = (struct soc_mixer_control *)kcontrol->private_value;
	struct snd_soc_component *comp = snd_kcontrol_chip(kcontrol);
	struct wcd9378_priv *wcd9378 = snd_soc_component_get_drvdata(comp);
	struct wcd9378_sdw_priv *wcd;
	int dai_id = mixer->shift;
	int ch_idx = mixer->reg;
	int portidx;

	wcd = wcd9378->sdw_priv[dai_id];
	portidx = wcd->ch_info[ch_idx].port_num;

	ucontrol->value.integer.value[0] = wcd->port_enable[portidx];

	return 0;
}

static int wcd9378_set_swr_port(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct soc_mixer_control *mixer = (struct soc_mixer_control *)kcontrol->private_value;
	struct snd_soc_component *comp = snd_kcontrol_chip(kcontrol);
	struct wcd9378_priv *wcd9378 = snd_soc_component_get_drvdata(comp);
	struct wcd9378_sdw_priv *wcd;
	int dai_id = mixer->shift;
	int ch_idx = mixer->reg;
	int portidx;
	bool enable;

	wcd = wcd9378->sdw_priv[dai_id];
	portidx = wcd->ch_info[ch_idx].port_num;

	enable = ucontrol->value.integer.value[0];

	if (enable == wcd->port_enable[portidx]) {
		wcd9378_connect_port(wcd, portidx, ch_idx, enable);
		return 0;
	}

	wcd->port_enable[portidx] = enable;
	wcd9378_connect_port(wcd, portidx, ch_idx, enable);

	return 1;
}

static int wcd9378_tx_mode_get(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct wcd9378_priv *wcd9378 = snd_soc_component_get_drvdata(component);
	struct soc_enum *e = (struct soc_enum *)kcontrol->private_value;
	int adc = e->shift_l;

	ucontrol->value.enumerated.item[0] = wcd9378->tx_mode[adc];

	return 0;
}

static int wcd9378_tx_mode_put(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct wcd9378_priv *wcd9378 = snd_soc_component_get_drvdata(component);
	struct soc_enum *e = (struct soc_enum *)kcontrol->private_value;
	int adc = e->shift_l;
	u32 mode_val = ucontrol->value.enumerated.item[0];

	if (mode_val >= e->items)
		return -EINVAL;

	if (mode_val == wcd9378->tx_mode[adc])
		return 0;

	wcd9378->tx_mode[adc] = mode_val;

	return 1;
}

static const char * const tx_mode_mux_text[] = {
	"ADC_INVALID", "ADC_HIFI", "ADC_LO_HIF", "ADC_NORMAL", "ADC_LP",
};

static const struct soc_enum tx0_mode_enum =
	SOC_ENUM_SINGLE(SND_SOC_NOPM, 0, ARRAY_SIZE(tx_mode_mux_text),
			tx_mode_mux_text);
static const struct soc_enum tx1_mode_enum =
	SOC_ENUM_SINGLE(SND_SOC_NOPM, 1, ARRAY_SIZE(tx_mode_mux_text),
			tx_mode_mux_text);
static const struct soc_enum tx2_mode_enum =
	SOC_ENUM_SINGLE(SND_SOC_NOPM, 2, ARRAY_SIZE(tx_mode_mux_text),
			tx_mode_mux_text);

static const struct snd_kcontrol_new wcd9378_snd_controls[] = {
	SOC_SINGLE_TLV("ADC1 Volume", WCD9378_ANA_TX_CH1, 0, 20, 0,
		       analog_gain),
	SOC_SINGLE_TLV("ADC2 Volume", WCD9378_ANA_TX_CH2, 0, 20, 0,
		       analog_gain),
	SOC_SINGLE_TLV("ADC3 Volume", WCD9378_ANA_TX_CH3, 0, 20, 0,
		       analog_gain),

	SOC_ENUM_EXT("TX0 MODE", tx0_mode_enum,
		     wcd9378_tx_mode_get, wcd9378_tx_mode_put),
	SOC_ENUM_EXT("TX1 MODE", tx1_mode_enum,
		     wcd9378_tx_mode_get, wcd9378_tx_mode_put),
	SOC_ENUM_EXT("TX2 MODE", tx2_mode_enum,
		     wcd9378_tx_mode_get, wcd9378_tx_mode_put),

	SOC_SINGLE_EXT("ADC1 Switch", WCD9378_ADC1, AIF1_CAP, 1, 0,
		       wcd9378_get_swr_port, wcd9378_set_swr_port),
	SOC_SINGLE_EXT("ADC2 Switch", WCD9378_ADC2, AIF1_CAP, 1, 0,
		       wcd9378_get_swr_port, wcd9378_set_swr_port),
	SOC_SINGLE_EXT("ADC3 Switch", WCD9378_ADC3, AIF1_CAP, 1, 0,
		       wcd9378_get_swr_port, wcd9378_set_swr_port),
};

static const char * const adc1_mux_text[] = {
	"CH1_AMIC_DISABLE", "CH1_AMIC1", "CH1_AMIC2", "CH1_AMIC3", "CH1_AMIC4",
};

static const char * const adc2_mux_text[] = {
	"CH2_AMIC_DISABLE", "CH2_AMIC1", "CH2_AMIC2", "CH2_AMIC3", "CH2_AMIC4",
};

static const char * const adc3_mux_text[] = {
	"CH3_AMIC_DISABLE", "CH3_AMIC1", "CH3_AMIC3", "CH3_AMIC4",
};

static const struct soc_enum adc1_mux_enum =
	SOC_ENUM_SINGLE(WCD9378_TX_NEW_CH12_MUX, 0,
			ARRAY_SIZE(adc1_mux_text), adc1_mux_text);
static const struct soc_enum adc2_mux_enum =
	SOC_ENUM_SINGLE(WCD9378_TX_NEW_CH12_MUX, 3,
			ARRAY_SIZE(adc2_mux_text), adc2_mux_text);
static const struct soc_enum adc3_mux_enum =
	SOC_ENUM_SINGLE(WCD9378_TX_NEW_CH34_MUX, 0,
			ARRAY_SIZE(adc3_mux_text), adc3_mux_text);

static const struct snd_kcontrol_new adc1_mux = SOC_DAPM_ENUM("ADC1 MUX", adc1_mux_enum);
static const struct snd_kcontrol_new adc2_mux = SOC_DAPM_ENUM("ADC2 MUX", adc2_mux_enum);
static const struct snd_kcontrol_new adc3_mux = SOC_DAPM_ENUM("ADC3 MUX", adc3_mux_enum);

static const struct snd_kcontrol_new tx0_seq_switch =
	SOC_DAPM_SINGLE("Switch", SND_SOC_NOPM, 0, 1, 0);
static const struct snd_kcontrol_new tx1_seq_switch =
	SOC_DAPM_SINGLE("Switch", SND_SOC_NOPM, 0, 1, 0);
static const struct snd_kcontrol_new tx2_seq_switch =
	SOC_DAPM_SINGLE("Switch", SND_SOC_NOPM, 0, 1, 0);

static const struct snd_soc_dapm_widget wcd9378_dapm_widgets[] = {
	/* Analog mic inputs */
	SND_SOC_DAPM_INPUT("AMIC1"),
	SND_SOC_DAPM_INPUT("AMIC2"),
	SND_SOC_DAPM_INPUT("AMIC3"),
	SND_SOC_DAPM_INPUT("AMIC4"),

	/* ADC input muxes */
	SND_SOC_DAPM_MUX("ADC1 MUX", SND_SOC_NOPM, 0, 0, &adc1_mux),
	SND_SOC_DAPM_MUX("ADC2 MUX", SND_SOC_NOPM, 0, 0, &adc2_mux),
	SND_SOC_DAPM_MUX("ADC3 MUX", SND_SOC_NOPM, 0, 0, &adc3_mux),

	/* SDCA TX sequencers (widget shift = ADC index) */
	SND_SOC_DAPM_MIXER_E("TX0 SEQUENCER", SND_SOC_NOPM, 0, 0,
			     &tx0_seq_switch, 1, wcd9378_tx_sequencer_enable,
			     SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_MIXER_E("TX1 SEQUENCER", SND_SOC_NOPM, 1, 0,
			     &tx1_seq_switch, 1, wcd9378_tx_sequencer_enable,
			     SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_MIXER_E("TX2 SEQUENCER", SND_SOC_NOPM, 2, 0,
			     &tx2_seq_switch, 1, wcd9378_tx_sequencer_enable,
			     SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),

	/* Micbias supplies (widget shift = micbias number) */
	SND_SOC_DAPM_SUPPLY("MIC BIAS1", SND_SOC_NOPM, MIC_BIAS_1, 0,
			    wcd9378_codec_enable_micbias,
			    SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
			    SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_SUPPLY("MIC BIAS2", SND_SOC_NOPM, MIC_BIAS_2, 0,
			    wcd9378_codec_enable_micbias,
			    SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
			    SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_SUPPLY("MIC BIAS3", SND_SOC_NOPM, MIC_BIAS_3, 0,
			    wcd9378_codec_enable_micbias,
			    SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
			    SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_SUPPLY("VA MIC BIAS1", SND_SOC_NOPM, MIC_BIAS_1, 0,
			    wcd9378_codec_enable_micbias_pullup,
			    SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
			    SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_SUPPLY("VA MIC BIAS2", SND_SOC_NOPM, MIC_BIAS_2, 0,
			    wcd9378_codec_enable_micbias_pullup,
			    SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
			    SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_SUPPLY("VA MIC BIAS3", SND_SOC_NOPM, MIC_BIAS_3, 0,
			    wcd9378_codec_enable_micbias_pullup,
			    SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
			    SND_SOC_DAPM_POST_PMD),

	/* Outputs towards the SoundWire TX bus / LPASS TX macro */
	SND_SOC_DAPM_OUTPUT("ADC1_OUTPUT"),
	SND_SOC_DAPM_OUTPUT("ADC2_OUTPUT"),
	SND_SOC_DAPM_OUTPUT("ADC3_OUTPUT"),
};

static const struct snd_soc_dapm_route wcd9378_audio_map[] = {
	{ "ADC1_OUTPUT", NULL, "TX0 SEQUENCER" },
	{ "TX0 SEQUENCER", "Switch", "ADC1 MUX" },
	{ "ADC1 MUX", "CH1_AMIC1", "AMIC1" },
	{ "ADC1 MUX", "CH1_AMIC2", "AMIC2" },
	{ "ADC1 MUX", "CH1_AMIC3", "AMIC3" },
	{ "ADC1 MUX", "CH1_AMIC4", "AMIC4" },

	{ "ADC2_OUTPUT", NULL, "TX1 SEQUENCER" },
	{ "TX1 SEQUENCER", "Switch", "ADC2 MUX" },
	{ "ADC2 MUX", "CH2_AMIC1", "AMIC1" },
	{ "ADC2 MUX", "CH2_AMIC2", "AMIC2" },
	{ "ADC2 MUX", "CH2_AMIC3", "AMIC3" },
	{ "ADC2 MUX", "CH2_AMIC4", "AMIC4" },

	{ "ADC3_OUTPUT", NULL, "TX2 SEQUENCER" },
	{ "TX2 SEQUENCER", "Switch", "ADC3 MUX" },
	{ "ADC3 MUX", "CH3_AMIC1", "AMIC1" },
	{ "ADC3 MUX", "CH3_AMIC3", "AMIC3" },
	{ "ADC3 MUX", "CH3_AMIC4", "AMIC4" },
};

/*
 * Map the DT micbias millivolt values onto ITxx_MICB usage codes;
 * non-standard voltages go through the sequencer remap table.
 */
static void wcd9378_set_micb_usage_vals(struct snd_soc_component *component)
{
	struct wcd9378_priv *wcd9378 = snd_soc_component_get_drvdata(component);
	static const u32 remap_regs[] = {
		WCD9378_MICB_REMAP_TABLE_VAL_3,
		WCD9378_MICB_REMAP_TABLE_VAL_4,
		WCD9378_MICB_REMAP_TABLE_VAL_5,
	};
	int i, vout;

	for (i = 0; i < WCD9378_MAX_MICBIAS; i++) {
		switch (wcd9378->common.micb_mv[i]) {
		case 1200:
			wcd9378->micb_usage_val[i] = WCD9378_MICB_USAGE_1P2V;
			break;
		case 1800:
			wcd9378->micb_usage_val[i] = WCD9378_MICB_USAGE_1P8V_OR_PULLUP;
			break;
		case 2200:
			wcd9378->micb_usage_val[i] = WCD9378_MICB_USAGE_2P2V;
			break;
		case 2500:
			wcd9378->micb_usage_val[i] = WCD9378_MICB_USAGE_2P5V;
			break;
		case 2700:
			wcd9378->micb_usage_val[i] = WCD9378_MICB_USAGE_2P7V;
			break;
		case 2750:
			wcd9378->micb_usage_val[i] = WCD9378_MICB_USAGE_2P75V;
			break;
		case 2800:
			wcd9378->micb_usage_val[i] = WCD9378_MICB_USAGE_2P8V;
			break;
		default:
			vout = wcd_get_micb_vout_ctl_val(component->dev,
							 wcd9378->common.micb_mv[i]);
			if (vout < 0) {
				wcd9378->micb_usage_val[i] =
					WCD9378_MICB_USAGE_1P8V_OR_PULLUP;
				break;
			}
			snd_soc_component_update_bits(component, remap_regs[i],
						      0xff, vout);
			wcd9378->micb_usage_val[i] =
				WCD9378_MICB_USAGE_REMAP_TABLE_3 + i;
			break;
		}
	}
}

static int wcd9378_soc_codec_probe(struct snd_soc_component *component)
{
	struct wcd9378_priv *wcd9378 = snd_soc_component_get_drvdata(component);
	struct device *dev = component->dev;
	unsigned int part0 = 0, part1 = 0;
	int ret;

	/*
	 * Flow up to this point: the two sdw probes registered
	 * components; the platform probe reset the codec and added the
	 * component master, whose bind then bound the slaves and
	 * registered this snd_soc component. io_init() below programs
	 * the analog core and must hit an enumerated codec with the bus
	 * clock indicated, so first wait out the TX slave's
	 * post-reset (re-)enumeration; this codec probe is the earliest
	 * point where all of that holds.
	 */
	ret = sdw_slave_wait_for_init(wcd9378->tx_sdw_dev, 5000);
	if (ret)
		return ret;

	snd_soc_component_init_regmap(component, wcd9378->regmap);

	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		return ret;

	regmap_read(wcd9378->regmap, WCD9378_DEV_PART_ID_0, &part0);
	regmap_read(wcd9378->regmap, WCD9378_DEV_PART_ID_1, &part1);
	dev_dbg(dev, "WCD9378 part id %#x\n", (part1 << 8) | part0);

	/*
	 * SDCA interrupt type configuration, mirroring the downstream init
	 * sequence. Nothing consumes these interrupts yet (no MBHC support);
	 * kept so the bring-up sequence validated on hardware is unchanged.
	 */
	sdw_write(wcd9378->tx_sdw_dev, WCD9378_SWRS_SCP_SDCA_INTRTYPE_1, 0xff);
	sdw_write(wcd9378->tx_sdw_dev, WCD9378_SWRS_SCP_SDCA_INTRTYPE_2, 0x0b);
	sdw_write(wcd9378->tx_sdw_dev, WCD9378_SWRS_SCP_SDCA_INTRTYPE_3, 0xff);

	wcd9378_io_init(component);
	wcd9378_set_micb_usage_vals(component);

	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);

	return 0;
}

static const struct snd_soc_component_driver soc_codec_dev_wcd9378 = {
	.name = "wcd9378_codec",
	.probe = wcd9378_soc_codec_probe,
	.controls = wcd9378_snd_controls,
	.num_controls = ARRAY_SIZE(wcd9378_snd_controls),
	.dapm_widgets = wcd9378_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(wcd9378_dapm_widgets),
	.dapm_routes = wcd9378_audio_map,
	.num_dapm_routes = ARRAY_SIZE(wcd9378_audio_map),
	.endianness = 1,
};

static int wcd9378_codec_hw_params(struct snd_pcm_substream *substream,
				   struct snd_pcm_hw_params *params,
				   struct snd_soc_dai *dai)
{
	struct wcd9378_priv *wcd9378 = dev_get_drvdata(dai->dev);
	struct wcd9378_sdw_priv *wcd = wcd9378->sdw_priv[dai->id];

	return wcd9378_sdw_hw_params(wcd, substream, params, dai);
}

static int wcd9378_codec_free(struct snd_pcm_substream *substream,
			      struct snd_soc_dai *dai)
{
	struct wcd9378_priv *wcd9378 = dev_get_drvdata(dai->dev);
	struct wcd9378_sdw_priv *wcd = wcd9378->sdw_priv[dai->id];

	return sdw_stream_remove_slave(wcd->sdev, wcd->sruntime);
}

static int wcd9378_codec_set_sdw_stream(struct snd_soc_dai *dai,
					void *stream, int direction)
{
	struct wcd9378_priv *wcd9378 = dev_get_drvdata(dai->dev);
	struct wcd9378_sdw_priv *wcd = wcd9378->sdw_priv[dai->id];

	wcd->sruntime = stream;

	return 0;
}

static int wcd9378_get_channel_map(const struct snd_soc_dai *dai,
				   unsigned int *tx_num, unsigned int *tx_slot,
				   unsigned int *rx_num, unsigned int *rx_slot)
{
	struct wcd9378_priv *wcd9378 = dev_get_drvdata(dai->dev);
	struct wcd9378_sdw_priv *wcd = wcd9378->sdw_priv[dai->id];
	int i;

	switch (dai->id) {
	case AIF1_PB:
		if (!rx_slot || !rx_num) {
			dev_err(dai->dev, "Invalid rx_slot %p or rx_num %p\n",
				rx_slot, rx_num);
			return -EINVAL;
		}

		for (i = 0; i < SDW_MAX_PORTS; i++)
			rx_slot[i] = wcd->master_channel_map[i];

		*rx_num = i;
		break;
	case AIF1_CAP:
		if (!tx_slot || !tx_num) {
			dev_err(dai->dev, "Invalid tx_slot %p or tx_num %p\n",
				tx_slot, tx_num);
			return -EINVAL;
		}

		for (i = 0; i < SDW_MAX_PORTS; i++)
			tx_slot[i] = wcd->master_channel_map[i];

		*tx_num = i;
		break;
	default:
		break;
	}

	return 0;
}

static const struct snd_soc_dai_ops wcd9378_sdw_dai_ops = {
	.hw_params = wcd9378_codec_hw_params,
	.hw_free = wcd9378_codec_free,
	.set_stream = wcd9378_codec_set_sdw_stream,
	.get_channel_map = wcd9378_get_channel_map,
};

#define WCD9378_RATES (SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_16000 |\
		       SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_48000 |\
		       SNDRV_PCM_RATE_96000 | SNDRV_PCM_RATE_192000)
#define WCD9378_FORMATS (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE |\
			 SNDRV_PCM_FMTBIT_S24_3LE | SNDRV_PCM_FMTBIT_S32_LE)

static struct snd_soc_dai_driver wcd9378_dais[] = {
	[0] = {
		.name = "wcd9378-sdw-rx",
		.playback = {
			.stream_name = "WCD AIF Playback",
			.rates = WCD9378_RATES,
			.formats = WCD9378_FORMATS,
			.rate_min = 8000,
			.rate_max = 192000,
			.channels_min = 1,
			.channels_max = 4,
		},
		.ops = &wcd9378_sdw_dai_ops,
	},
	[1] = {
		.name = "wcd9378-sdw-tx",
		.capture = {
			.stream_name = "WCD AIF Capture",
			.rates = WCD9378_RATES,
			.formats = WCD9378_FORMATS,
			.rate_min = 8000,
			.rate_max = 192000,
			.channels_min = 1,
			.channels_max = 4,
		},
		.ops = &wcd9378_sdw_dai_ops,
	},
};

static int wcd9378_bind(struct device *dev)
{
	struct wcd9378_priv *wcd9378 = dev_get_drvdata(dev);
	int ret;

	/* Give the SDW subdevices some more time to settle */
	usleep_range(5000, 5010);

	ret = component_bind_all(dev, wcd9378);
	if (ret) {
		dev_err(dev, "Slave bind failed, ret = %d\n", ret);
		return ret;
	}

	wcd9378->rxdev = of_sdw_find_device_by_node(wcd9378->rxnode);
	if (!wcd9378->rxdev) {
		dev_err(dev, "could not find rx slave with matching of node\n");
		ret = -EINVAL;
		goto err_component_unbind;
	}

	wcd9378->sdw_priv[AIF1_PB] = dev_get_drvdata(wcd9378->rxdev);
	wcd9378->sdw_priv[AIF1_PB]->wcd9378 = wcd9378;

	wcd9378->txdev = of_sdw_find_device_by_node(wcd9378->txnode);
	if (!wcd9378->txdev) {
		dev_err(dev, "could not find tx slave with matching of node\n");
		ret = -EINVAL;
		goto err_put_rxdev;
	}

	wcd9378->sdw_priv[AIF1_CAP] = dev_get_drvdata(wcd9378->txdev);
	wcd9378->sdw_priv[AIF1_CAP]->wcd9378 = wcd9378;
	wcd9378->tx_sdw_dev = dev_to_sdw_dev(wcd9378->txdev);

	/*
	 * As TX is the main CSR reg interface, it should not be suspended
	 * first. Explicitly add the dependency link.
	 */
	if (!device_link_add(wcd9378->rxdev, wcd9378->txdev,
			     DL_FLAG_STATELESS | DL_FLAG_PM_RUNTIME)) {
		dev_err(dev, "Could not devlink TX and RX\n");
		ret = -EINVAL;
		goto err_put_txdev;
	}

	if (!device_link_add(dev, wcd9378->txdev,
			     DL_FLAG_STATELESS | DL_FLAG_PM_RUNTIME)) {
		dev_err(dev, "Could not devlink WCD and TX\n");
		ret = -EINVAL;
		goto err_remove_link1;
	}

	if (!device_link_add(dev, wcd9378->rxdev,
			     DL_FLAG_STATELESS | DL_FLAG_PM_RUNTIME)) {
		dev_err(dev, "Could not devlink WCD and RX\n");
		ret = -EINVAL;
		goto err_remove_link2;
	}

	wcd9378->regmap = wcd9378->sdw_priv[AIF1_CAP]->regmap;
	if (!wcd9378->regmap) {
		dev_err(dev, "could not get TX device regmap\n");
		ret = -EINVAL;
		goto err_remove_link3;
	}

	/*
	 * The SDCA function engine dies when the TX bus enters clock-stop
	 * and only a codec reset revives it - registers keep their values
	 * so a regcache sync or a FUNC_ACT re-toggle does not help. The
	 * downstream stack sidesteps the same problem by marking the TX
	 * SoundWire master "qcom,is-always-on"; do the equivalent and
	 * keep the TX slave (and thus its bus) runtime-active while the
	 * codec is bound.
	 */
	ret = pm_runtime_resume_and_get(wcd9378->txdev);
	if (ret < 0) {
		dev_err(dev, "could not resume TX device\n");
		goto err_remove_link3;
	}

	ret = snd_soc_register_component(dev, &soc_codec_dev_wcd9378,
					 wcd9378_dais, ARRAY_SIZE(wcd9378_dais));
	if (ret) {
		dev_err(dev, "Codec registration failed\n");
		pm_runtime_put(wcd9378->txdev);
		goto err_remove_link3;
	}

	return ret;

err_remove_link3:
	device_link_remove(dev, wcd9378->rxdev);
err_remove_link2:
	device_link_remove(dev, wcd9378->txdev);
err_remove_link1:
	device_link_remove(wcd9378->rxdev, wcd9378->txdev);
err_put_txdev:
	put_device(wcd9378->txdev);
err_put_rxdev:
	put_device(wcd9378->rxdev);
err_component_unbind:
	component_unbind_all(dev, wcd9378);
	return ret;
}

static void wcd9378_unbind(struct device *dev)
{
	struct wcd9378_priv *wcd9378 = dev_get_drvdata(dev);

	snd_soc_unregister_component(dev);
	pm_runtime_put(wcd9378->txdev);
	device_link_remove(dev, wcd9378->txdev);
	device_link_remove(dev, wcd9378->rxdev);
	device_link_remove(wcd9378->rxdev, wcd9378->txdev);
	component_unbind_all(dev, wcd9378);
	put_device(wcd9378->txdev);
	put_device(wcd9378->rxdev);
}

static const struct component_master_ops wcd9378_comp_ops = {
	.bind = wcd9378_bind,
	.unbind = wcd9378_unbind,
};

static int wcd9378_add_slave_components(struct wcd9378_priv *wcd9378,
					struct device *dev,
					struct component_match **matchptr)
{
	struct device_node *np = dev->of_node;

	wcd9378->rxnode = of_parse_phandle(np, "qcom,rx-device", 0);
	if (!wcd9378->rxnode) {
		dev_err(dev, "Couldn't parse phandle to qcom,rx-device!\n");
		return -ENODEV;
	}

	component_match_add_release(dev, matchptr, component_release_of,
				    component_compare_of, wcd9378->rxnode);

	wcd9378->txnode = of_parse_phandle(np, "qcom,tx-device", 0);
	if (!wcd9378->txnode) {
		dev_err(dev, "Couldn't parse phandle to qcom,tx-device\n");
		return -ENODEV;
	}

	component_match_add_release(dev, matchptr, component_release_of,
				    component_compare_of, wcd9378->txnode);

	return 0;
}

static int wcd9378_probe(struct platform_device *pdev)
{
	struct component_match *match = NULL;
	struct device *dev = &pdev->dev;
	struct wcd9378_priv *wcd9378;
	int ret, i;

	wcd9378 = devm_kzalloc(dev, sizeof(*wcd9378), GFP_KERNEL);
	if (!wcd9378)
		return -ENOMEM;

	dev_set_drvdata(dev, wcd9378);
	mutex_init(&wcd9378->micb_lock);
	for (i = 0; i < ARRAY_SIZE(wcd9378->tx_sys_bit); i++)
		wcd9378->tx_sys_bit[i] = -1;
	wcd9378->common.dev = dev;
	wcd9378->common.max_bias = WCD9378_MAX_MICBIAS;

	wcd9378->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(wcd9378->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(wcd9378->reset_gpio),
				     "failed to get reset gpio\n");

	ret = devm_regulator_bulk_get_enable(dev, ARRAY_SIZE(wcd9378_supplies),
					     wcd9378_supplies);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to get and enable supplies\n");

	ret = wcd_dt_parse_micbias_info(&wcd9378->common);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get micbias\n");

	ret = wcd9378_add_slave_components(wcd9378, dev, &match);
	if (ret)
		return ret;

	wcd9378_reset(wcd9378);

	pm_runtime_set_autosuspend_delay(dev, 3000);
	pm_runtime_use_autosuspend(dev);
	ret = devm_pm_runtime_set_active_enabled(dev);
	if (ret)
		return ret;

	ret = component_master_add_with_match(dev, &wcd9378_comp_ops, match);
	if (ret)
		return ret;

	pm_runtime_mark_last_busy(dev);
	pm_runtime_idle(dev);

	return 0;
}

static void wcd9378_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct wcd9378_priv *wcd9378 = dev_get_drvdata(dev);

	component_master_del(dev, &wcd9378_comp_ops);

	mutex_destroy(&wcd9378->micb_lock);
}

static const struct of_device_id wcd9378_of_match[] = {
	{ .compatible = "qcom,wcd9378-codec" },
	{ }
};
MODULE_DEVICE_TABLE(of, wcd9378_of_match);

static struct platform_driver wcd9378_codec_driver = {
	.probe = wcd9378_probe,
	.remove = wcd9378_remove,
	.driver = {
		.name = "wcd9378_codec",
		.of_match_table = wcd9378_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(wcd9378_codec_driver);

MODULE_DESCRIPTION("WCD9378 Codec driver");
MODULE_LICENSE("GPL");
