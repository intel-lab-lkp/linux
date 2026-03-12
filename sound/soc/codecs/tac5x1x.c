// SPDX-License-Identifier: GPL-2.0-only
/*
 * tac5x1x.c -- Codec driver for TAC5x1x
 *
 * Copyright (C) 2025 Texas Instruments Incorporated - https://www.ti.com
 *
 * Author: Niranjan H Y <niranjan.hy@ti.com>
 */
#include <linux/types.h>
#include <linux/of_irq.h>
#include <linux/pm_runtime.h>
#include <linux/i2c.h>
#include <sound/soc.h>
#include <sound/pcm_params.h>
#include <sound/tlv.h>
#include <linux/mfd/tac5x1x/registers.h>
#include "tac5x1x.h"

struct mask_to_txt {
	u8 mask;
	const char *const name;
};

struct interrupt_info {
	u32 reg;
	u32 count;
	const struct mask_to_txt *mask_str_map;
};

#define TAC5X1X_EVENT(bit, evt_txt) ( \
	(struct mask_to_txt){             \
	.mask = BIT((bit)),           \
	.name = evt_txt})

static const struct mask_to_txt int_chx_latch[] = {
	TAC5X1X_EVENT(7, "Input Channel1 fault"),
	TAC5X1X_EVENT(6, "Input Channel2 fault"),
	TAC5X1X_EVENT(5, "Output Channel1 fault"),
	TAC5X1X_EVENT(4, "Output Channel2 fault"),
	TAC5X1X_EVENT(3, "Short to VBAT_IN"),
};

static const struct mask_to_txt in_ch1_latch[] = {
	TAC5X1X_EVENT(7, "IN_CH1 open Input"),
	TAC5X1X_EVENT(6, "IN_CH1 Input shorted"),
	TAC5X1X_EVENT(5, "IN_CH1 INP shorted to GND"),
	TAC5X1X_EVENT(4, "IN_CH1 INM shorted to GND"),
	TAC5X1X_EVENT(3, "IN_CH1 INP shorted to MICBIAS"),
	TAC5X1X_EVENT(2, "IN_CH1 INM shorted to MICBIAS"),
	TAC5X1X_EVENT(1, "IN_CH1 INP shorted to VBAT_IN"),
	TAC5X1X_EVENT(0, "IN_CH1 INM shorted to VBAT_IN"),
};

static const struct mask_to_txt in_ch2_latch[] = {
	TAC5X1X_EVENT(7, "IN_CH2 open Input"),
	TAC5X1X_EVENT(6, "IN_CH2 Input shorted"),
	TAC5X1X_EVENT(5, "IN_CH2 INP shorted to GND"),
	TAC5X1X_EVENT(4, "IN_CH2 INM shorted to GND"),
	TAC5X1X_EVENT(3, "IN_CH2 INP shorted to MICBIAS"),
	TAC5X1X_EVENT(2, "IN_CH2 INM shorted to MICBIAS"),
	TAC5X1X_EVENT(1, "IN_CH2 INP shorted to VBAT_IN"),
	TAC5X1X_EVENT(0, "IN_CH2 INM shorted to VBAT_IN"),
};

static const struct mask_to_txt out_ch1_latch[] = {
	TAC5X1X_EVENT(7, "OUT_CH1 OUT1P Short circuit Fault"),
	TAC5X1X_EVENT(6, "OUT_CH1 OUT1M Short circuit Fault"),
	TAC5X1X_EVENT(5, "OUT_CH1 DRVRP Virtual Ground Fault"),
	TAC5X1X_EVENT(4, "OUT_CH1 DRVRM Virtual ground Fault"),
	/* masks */
	TAC5X1X_EVENT(3, "OUT_CH1 ADC CH1 Mask"),
	TAC5X1X_EVENT(2, "OUT_CH1 ADC CH2 MASK"),
};

static const struct mask_to_txt out_ch2_latch[] = {
	TAC5X1X_EVENT(7, "OUT_CH2 OUT2P Short circuit Fault"),
	TAC5X1X_EVENT(6, "OUT_CH2 OUT2M Short circuit Fault"),
	TAC5X1X_EVENT(5, "OUT_CH2 DRVRP Virtual Ground Fault"),
	TAC5X1X_EVENT(4, "OUT_CH2 DRVRM Virtual ground Fault"),
	/* mask */
	TAC5X1X_EVENT(1, "AREG SC Fault Mask"),
	TAC5X1X_EVENT(0, "AREG SC Fault"),
};

static const struct mask_to_txt int_latch1[] = {
	TAC5X1X_EVENT(7, "CH1 INP Over Voltage"),
	TAC5X1X_EVENT(6, "CH1 INM Over Voltage"),
	TAC5X1X_EVENT(5, "CH2 INP over Voltage"),
	TAC5X1X_EVENT(4, "CH2 INM Over Voltage"),
	TAC5X1X_EVENT(3, "Headset Insert Detection"),
	TAC5X1X_EVENT(2, "Headset Remove Detection"),
	TAC5X1X_EVENT(1, "Headset Hook"),
	TAC5X1X_EVENT(0, "MIPS Overload"),
};

static const struct mask_to_txt int_latch2[] = {
	TAC5X1X_EVENT(7, "GPA Up threashold Fault"),
	TAC5X1X_EVENT(6, "GPA low threashold Fault"),
	TAC5X1X_EVENT(5, "VAD Power up detect"),
	TAC5X1X_EVENT(4, "VAD power down detect"),
	TAC5X1X_EVENT(3, "Micbias short circuit"),
	TAC5X1X_EVENT(2, "Micbias high current fault"),
	TAC5X1X_EVENT(1, "Micbias low current fault"),
	TAC5X1X_EVENT(0, "Micbias Over voltage fault"),
};

static const struct mask_to_txt int_latch_0[] = {
	TAC5X1X_EVENT(7, "Clock Error"),
	TAC5X1X_EVENT(6, "PLL Lock"),
	TAC5X1X_EVENT(5, "Boost Over Temperature"),
	TAC5X1X_EVENT(4, "Boost Over Current"),
	TAC5X1X_EVENT(3, "Boost MO"),
};

#define LTCH_TO_MASK_STR_MAP(latch_reg, str_map, map_size) ( \
	(struct interrupt_info){                                 \
	.reg = (latch_reg),                                  \
	.count = (map_size),                                 \
	.mask_str_map = (str_map),                           \
})

static const struct interrupt_info intr_info_list[] = {
	LTCH_TO_MASK_STR_MAP(TAC5X1X_REG_CHX_LTCH, int_chx_latch,
			     ARRAY_SIZE(int_chx_latch)),
	LTCH_TO_MASK_STR_MAP(TAC5X1X_REG_IN_CH1_LTCH, in_ch1_latch,
			     ARRAY_SIZE(in_ch1_latch)),
	LTCH_TO_MASK_STR_MAP(TAC5X1X_REG_IN_CH2_LTCH, in_ch2_latch,
			     ARRAY_SIZE(in_ch2_latch)),
	LTCH_TO_MASK_STR_MAP(TAC5X1X_REG_OUT_CH1_LTCH, out_ch1_latch,
			     ARRAY_SIZE(out_ch1_latch)),
	LTCH_TO_MASK_STR_MAP(TAC5X1X_REG_OUT_CH2_LTCH, out_ch2_latch,
			     ARRAY_SIZE(out_ch2_latch)),
	LTCH_TO_MASK_STR_MAP(TAC5X1X_REG_INT_LTCH1, int_latch1,
			     ARRAY_SIZE(int_latch1)),
	LTCH_TO_MASK_STR_MAP(TAC5X1X_REG_INT_LTCH2, int_latch2,
			     ARRAY_SIZE(int_latch2)),
	/* This should be the last entry */
	LTCH_TO_MASK_STR_MAP(TAC5X1X_REG_INT_LTCH0, int_latch_0,
			     ARRAY_SIZE(int_latch_0)),
};

static void process_one_interrupt(struct tac5x1x_priv *tac5x1x, s32 index,
				  s32 value)
{
	u32 map_count, i;
	const struct mask_to_txt *map_items;
	struct device *dev = tac5x1x->tac5x1x->dev;

	map_count = intr_info_list[index].count;
	map_items = intr_info_list[index].mask_str_map;

	for (i = 0; i < map_count; i++) {
		if (value & map_items[i].mask)
			dev_dbg(dev, "Interrupt %s detected\n",
				map_items[i].name);
	}
}

static irqreturn_t irq_thread_func(s32 irq, void *dev_id)
{
	u8 latch_set = 0;
	u32 latch_count;
	s32 i, ret;
	struct tac5x1x_priv *tac5x1x = (struct tac5x1x_priv *)dev_id;
	struct device *dev = tac5x1x->tac5x1x->dev;

	latch_count = ARRAY_SIZE(intr_info_list);

	ret = regmap_multi_reg_read(tac5x1x->tac5x1x->regmap,
				    tac5x1x->irqinfo.latch_regs,
				    tac5x1x->irqinfo.latch_data, latch_count);
	if (ret) {
		dev_err(dev,
			"interrupt: latch register read failed");
		return IRQ_NONE;
	}

	for (i = 0; i < latch_count; i++) {
		dev_dbg(dev, "reg=0x%0x, val=0x%02x",
			tac5x1x->irqinfo.latch_regs[i],
			tac5x1x->irqinfo.latch_data[i]);
		latch_set |= tac5x1x->irqinfo.latch_data[i] & 0xff;
	}

	if (!latch_set)
		return IRQ_NONE;

	for (i = 0; i < latch_count; i++) {
		if (!tac5x1x->irqinfo.latch_data[i])
			continue;
		process_one_interrupt(tac5x1x, i,
				      tac5x1x->irqinfo.latch_data[i]);
		tac5x1x->irqinfo.latch_data[i] = 0;
	}

	return IRQ_HANDLED;
}

static s32 tac5x1x_register_interrupt(struct tac5x1x_priv *tac5x1x)
{
	struct device_node *np;
	s32 ret, latch_count, i;
	u32 *latch_regs;
	u8 *latch_data;
	struct device *dev;

	dev = tac5x1x->tac5x1x->dev;
	np = dev->of_node;
	latch_count = ARRAY_SIZE(intr_info_list);

	tac5x1x->irqinfo.irq = of_irq_get(np, 0);
	if (tac5x1x->irqinfo.irq < 0) {
		dev_dbg(dev, "No IRQ configured, running without interrupts\n");
		return 0; /* Not an error - interrupts are optional */
	}

	latch_regs = devm_kzalloc(dev, latch_count * sizeof(u32),
				  GFP_KERNEL);
	latch_data = devm_kzalloc(dev, latch_count * sizeof(u8),
				  GFP_KERNEL);
	if (!latch_data || !latch_regs)
		return -ENOMEM;

	for (i = 0; i < latch_count; i++)
		latch_regs[i] = intr_info_list[i].reg;

	tac5x1x->irqinfo.latch_regs = latch_regs;
	tac5x1x->irqinfo.latch_data = latch_data;
	/* Clear any pending interrupts before enabling */
	regmap_write(tac5x1x->tac5x1x->regmap, TAC5X1X_INT, 0x11);

	ret = devm_request_threaded_irq(dev, tac5x1x->irqinfo.irq,
					NULL, irq_thread_func,
					IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					"tac5x1x-irq", tac5x1x);
	if (ret)
		dev_err(dev, "Failed to request IRQ %d: %d\n",
			tac5x1x->irqinfo.irq, ret);

	return ret;
}

#define IS_DAC_CH(ch)	((ch) & 0x0f)
#define IS_ADC_CH(ch)	((ch) & 0xf0)

/* Delay in ms before adjusting power state after last channel change */
#define TAC5X1X_POWER_SETTLE_MS	100

/*
 * Delayed work function that handles power state adjustments.
 *
 * This function is scheduled after any channel enable/disable event.
 * It waits for the hardware to settle, then adjusts power state based
 * on which channels are currently enabled. Using mod_delayed_work()
 * ensures the timer is reset on each new event.
 */
static void post_powerup_work(struct work_struct *work)
{
	u8 mask;
	u8 pwr_cfg;
	u32 input_mode = 0;
	bool ch1_is_pdm, ch2_is_pdm;
	struct tac5x1x_priv *tac5x1x =
		container_of(work, struct tac5x1x_priv, powerup_work.work);
	struct snd_soc_component *component = tac5x1x->component;

	dev_dbg(component->dev, "POST_POWERUP_WORK ch_enabled=0x%02x\n",
		tac5x1x->ch_enabled);

	mutex_lock(&tac5x1x->ch_lock);

	/* Clear the power-up flag so next power event can write PWR_CFG */
	tac5x1x->pwr_up_done = false;

	/* Write the current channel enable state */
	snd_soc_component_write(component, TAC5X1X_CH_EN, tac5x1x->ch_enabled);

	/* Check if PDM mode is selected via INTF4 register */
	regmap_read(tac5x1x->tac5x1x->regmap, TAC5X1X_INTF4, &input_mode);
	ch1_is_pdm = !!(input_mode & BIT(7));
	ch2_is_pdm = !!(input_mode & BIT(6));

	dev_dbg(component->dev, "INTF4=0x%02x ch1_pdm=%d ch2_pdm=%d\n",
		input_mode, ch1_is_pdm, ch2_is_pdm);

	mask = TAC5X1X_PWR_CFG_ADC_PDZ | TAC5X1X_PWR_CFG_MICBIAS |
	       TAC5X1X_PWR_CFG_DAC_PDZ;
	pwr_cfg = mask;

	if (IS_DAC_CH(tac5x1x->ch_enabled) == 0)
		pwr_cfg &= ~TAC5X1X_PWR_CFG_DAC_PDZ;

	/*
	 * Only disable MICBIAS if ALL enabled ADC channels are in PDM mode
	 */
	bool ch1_enabled = !!(tac5x1x->ch_enabled &
			      (TAC5X1X_CH_EN_ADC_CH1 | TAC5X1X_CH_EN_DAC_CH1));
	bool ch2_enabled = !!(tac5x1x->ch_enabled &
			      (TAC5X1X_CH_EN_ADC_CH2 | TAC5X1X_CH_EN_DAC_CH2));
	bool need_micbias = false;

	if ((tac5x1x->ch_enabled & TAC5X1X_CH_EN_ADC_CH1) && !ch1_is_pdm)
		need_micbias = true;
	if ((tac5x1x->ch_enabled & TAC5X1X_CH_EN_ADC_CH2) && !ch2_is_pdm)
		need_micbias = true;

	if (!need_micbias && ((ch1_is_pdm && ch1_enabled) || (ch2_is_pdm && ch2_enabled))) {
		/* PDM mode for enabled channels - no MICBIAS needed */
		pwr_cfg &= ~TAC5X1X_PWR_CFG_MICBIAS;
	}
	if (IS_ADC_CH(tac5x1x->ch_enabled) == 0) {
		/* No ADC channels enabled - disable ADC power and MICBIAS */
		pwr_cfg &= ~TAC5X1X_PWR_CFG_ADC_PDZ;
		pwr_cfg &= ~TAC5X1X_PWR_CFG_MICBIAS;
	}

	dev_dbg(component->dev, "PWR_CFG mask=0x%02x val=0x%02x\n", mask, pwr_cfg);
	snd_soc_component_update_bits(component, TAC5X1X_PWR_CFG, mask, pwr_cfg);
	mutex_unlock(&tac5x1x->ch_lock);
}

static int tac5x1x_enable_channel_unlocked(struct snd_soc_component *comp,
					   bool is_adc, s32 right)
{
	s32 ret;
	u8 mask_dev;
	u8 mask;
	struct tac5x1x_priv *tac5x1x;

	tac5x1x = snd_soc_component_get_drvdata(comp);
	if (right) {
		mask_dev = TAC5X1X_CH_EN_ADC_CH2 | TAC5X1X_CH_EN_DAC_CH2;
		mask = is_adc ? TAC5X1X_CH_EN_ADC_CH2 : TAC5X1X_CH_EN_DAC_CH2;
	} else {
		mask_dev = TAC5X1X_CH_EN_ADC_CH1 | TAC5X1X_CH_EN_DAC_CH1;
		mask = is_adc ? TAC5X1X_CH_EN_ADC_CH1 : TAC5X1X_CH_EN_DAC_CH1;
	}
	tac5x1x->ch_enabled |= mask;
	ret = snd_soc_component_update_bits(comp, TAC5X1X_CH_EN,
					    mask_dev, mask_dev);

	return ret;
}

static int tac5x1x_disable_channel_unlocked(struct snd_soc_component *comp,
					    bool is_adc, s32 right)
{
	u8 mask;
	s32 ret;
	struct tac5x1x_priv *tac5x1x;

	tac5x1x = snd_soc_component_get_drvdata(comp);
	mask = is_adc ?
		(right ? TAC5X1X_CH_EN_ADC_CH2 : TAC5X1X_CH_EN_ADC_CH1) :
		(right ? TAC5X1X_CH_EN_DAC_CH2 : TAC5X1X_CH_EN_DAC_CH1);
	tac5x1x->ch_enabled &= ~mask;
	ret = snd_soc_component_update_bits(comp, TAC5X1X_CH_EN, mask, 0);

	return ret;
}

/*
 * When ADC and DAC are enabled with time delay between them
 * the one which is started latter doesn't work because of HW bug.
 * So DAC and ADC events with delayed work is added to follow a
 * particular powerup sequence.
 *
 * Power-up: Immediately power up both ADC and DAC blocks together.
 * Power-down: Defer to delayed work to avoid races with quick on/off cycles.
 *
 * Using mod_delayed_work() ensures the timer is reset on each event,
 * so rapid on/off cycles are handled correctly.
 */
static int tac5x1x_dac_event(struct snd_soc_dapm_widget *w,
			     struct snd_kcontrol *kcontrol, int event)
{
	u8 pwr_cfg;
	int right = w->shift;
	int ret = 0;
	struct snd_soc_component *component =
		snd_soc_dapm_to_component(w->dapm);
	struct tac5x1x_priv *tac5x1x = snd_soc_component_get_drvdata(component);

	dev_dbg(component->dev, "DAC_EVENT event=%d right=%d\n", event, right);

	mutex_lock(&tac5x1x->ch_lock);

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		ret = tac5x1x_enable_channel_unlocked(component, false, right);
		if (ret < 0) {
			dev_err(component->dev,
				"Failed to update enable DAC channels\n");
			break;
		}

		/* Power up both ADC and DAC together due to HW bug */
		if (!tac5x1x->pwr_up_done) {
			pwr_cfg = TAC5X1X_PWR_CFG_DAC_PDZ |
				TAC5X1X_PWR_CFG_ADC_PDZ |
				TAC5X1X_PWR_CFG_MICBIAS;
			ret = snd_soc_component_write(component,
						      TAC5X1X_PWR_CFG,
						      pwr_cfg);
			if (ret) {
				dev_err(component->dev,
					"Failed to power up DAC\n");
				tac5x1x_disable_channel_unlocked(component,
								 false, right);
				/* Mark done even on error to prevent retry loop */
				tac5x1x->pwr_up_done = true;
				break;
			}
			tac5x1x->pwr_up_done = true;
		}
		mod_delayed_work(system_wq, &tac5x1x->powerup_work,
				 msecs_to_jiffies(TAC5X1X_POWER_SETTLE_MS));
		break;

	case SND_SOC_DAPM_PRE_PMD:
		tac5x1x_disable_channel_unlocked(component, false, right);
		tac5x1x->pwr_up_done = false;
		/*
		 * Don't power down immediately - schedule delayed work to
		 * handle power state. This avoids races with quick on/off.
		 */
		mod_delayed_work(system_wq, &tac5x1x->powerup_work,
				 msecs_to_jiffies(TAC5X1X_POWER_SETTLE_MS));
		break;
	}
	mutex_unlock(&tac5x1x->ch_lock);

	return ret;
}

static int tac5x1x_adc_event(struct snd_soc_dapm_widget *w,
			     struct snd_kcontrol *kcontrol, int event)
{
	u8 pwr_cfg;
	u32 right = w->shift;
	s32 ret = 0;
	struct snd_soc_component *c = snd_soc_dapm_to_component(w->dapm);
	struct tac5x1x_priv *tac5x1x = snd_soc_component_get_drvdata(c);

	dev_dbg(c->dev, "ADC_EVENT event=%d right=%d\n", event, right);

	mutex_lock(&tac5x1x->ch_lock);
	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		ret = tac5x1x_enable_channel_unlocked(c, true, right);
		if (ret < 0) {
			dev_err(c->dev,
				"Failed to enable ADC/PDM channel\n");
			break;
		}

		/* Power up ADC, MICBIAS, and DAC together */
		/* Only write if not already done by a previous event */
		if (!tac5x1x->pwr_up_done) {
			pwr_cfg = TAC5X1X_PWR_CFG_ADC_PDZ |
				  TAC5X1X_PWR_CFG_MICBIAS |
				  TAC5X1X_PWR_CFG_DAC_PDZ;
			ret = snd_soc_component_write(c, TAC5X1X_PWR_CFG, pwr_cfg);
			if (ret) {
				dev_err(c->dev, "Failed to power up\n");
				tac5x1x_disable_channel_unlocked(c, true, right);
				/* Mark done even on error to prevent retry loop */
				tac5x1x->pwr_up_done = true;
				break;
			}
			tac5x1x->pwr_up_done = true;
		}

		mod_delayed_work(system_wq, &tac5x1x->powerup_work,
				 msecs_to_jiffies(TAC5X1X_POWER_SETTLE_MS));
		break;

	case SND_SOC_DAPM_PRE_PMD:
		tac5x1x_disable_channel_unlocked(c, true, right);
		/* Clear power-up flag when disabling channels */
		tac5x1x->pwr_up_done = false;
		/* Power down ADC if no ADC channels active */
		if (IS_ADC_CH(tac5x1x->ch_enabled) == 0)
			snd_soc_component_update_bits(c, TAC5X1X_PWR_CFG,
						      TAC5X1X_PWR_CFG_ADC_PDZ |
						      TAC5X1X_PWR_CFG_MICBIAS,
						      0);
		break;
	}
	mutex_unlock(&tac5x1x->ch_lock);

	return ret;
}

/*
 * ADC full-scale selection
 * 2/10-VRMS is for TAX52xx/TAX51xx devices
 * 4/5-VRMS is for TAX54xx/TAX53xx devices
 */
static const char *const tac5x1x_adc_fscale_text[] = {"2/10-VRMS",
	"4/5-VRMS"};

static SOC_ENUM_SINGLE_DECL(tac5x1x_adc1_fscale_enum, TAC5X1X_ADCCH1C0, 1,
		tac5x1x_adc_fscale_text);
static SOC_ENUM_SINGLE_DECL(tac5x1x_adc2_fscale_enum, TAC5X1X_ADCCH2C0, 1,
		tac5x1x_adc_fscale_text);

static const struct snd_kcontrol_new tac5x1x_dapm_adc1_fscale_control[] = {
	SOC_DAPM_ENUM("ADC1 FSCALE MUX", tac5x1x_adc1_fscale_enum),
};

static const struct snd_kcontrol_new tac5x1x_dapm_adc2_fscale_control[] = {
	SOC_DAPM_ENUM("ADC2 FSCALE MUX", tac5x1x_adc2_fscale_enum),
};

static const char *const pdmclk_text[] = {
	"2.8224 MHz or 3.072 MHz", "1.4112 MHz or 1.536 MHz",
	"705.6 kHz or 768 kHz", "5.6448 MHz or 6.144 MHz"};

static SOC_ENUM_SINGLE_DECL(pdmclk_select_enum, TAC5X1X_CNTCLK0, 6,
		pdmclk_text);

/* Digital Volume control. From -80 to 47 dB in 0.5 dB steps */
static DECLARE_TLV_DB_SCALE(record_dig_vol_tlv, -8000, 50, 0);

/* Gain Calibration control. From -0.8db to 0.7db dB in 0.1 dB steps */
static DECLARE_TLV_DB_MINMAX(record_gain_cali_tlv, -80, 70);

/* Analog Level control. From -12 to 24 dB in 6 dB steps */
static DECLARE_TLV_DB_SCALE(playback_analog_level_tlv, -1200, 600, 0);

/* Digital Volume control. From -100 to 27 dB in 0.5 dB steps */
static DECLARE_TLV_DB_SCALE(dac_dig_vol_tlv, -10000, 50, 0); // mute ?

/* Gain Calibration control. From -0.8db to 0.7db dB in 0.1 dB steps */
static DECLARE_TLV_DB_MINMAX(playback_gain_cali_tlv, -80, 70);

/* Output Source Selection */
static const char *const tac5x1x_output_source_text[] = {
	"Disabled",
	"DAC Input",
	"Analog Bypass",
	"DAC + Analog Bypass Mix",
	"DAC -> OUTxP, INxP -> OUTxM",
	"INxM -> OUTxP, DAC -> OUTxM",
};

static SOC_ENUM_SINGLE_DECL(tac5x1x_out1_source_enum, TAC5X1X_OUT1CFG0, 5,
		tac5x1x_output_source_text);
static SOC_ENUM_SINGLE_DECL(tac5x1x_out2_source_enum, TAC5X1X_OUT2CFG0, 5,
		tac5x1x_output_source_text);

static const struct snd_kcontrol_new tac5x1x_dapm_out1_source_control[] = {
	SOC_DAPM_ENUM("OUT1X MUX", tac5x1x_out1_source_enum),
};

static const struct snd_kcontrol_new tac5x1x_dapm_out2_source_control[] = {
	SOC_DAPM_ENUM("OUT2X MUX", tac5x1x_out2_source_enum),
};

/* Output Config Selection */
static const char *const tac5x1x_output_config_text[] = {
	"Differential",
	"Stereo Single-ended",
	"Mono Single-ended at OUTxP only",
	"Mono Single-ended at OUTxM only",
	"Pseudo differential with OUTxM as VCOM",
	"Pseudo differential with OUTxM as external sensing",
	"Pseudo differential with OUTxP as VCOM",
};

static const char *const tac5x1x_output2_config_text[] = {
	"Differential",
	"Stereo Single-ended",
	"Mono Single-ended at OUTxP only",
	"Mono Single-ended at OUTxM only",
	"Pseudo differential with OUTxM as VCOM",
	"Pseudo differential with OUTxP as VCOM",
};

static const s32 tac5x1x_output2_config_values[] = {
	0, 1, 2, 3, 4, 6
};

static SOC_ENUM_SINGLE_DECL(tac5x1x_out1_config_enum, TAC5X1X_OUT1CFG0, 2,
			tac5x1x_output_config_text);
static SOC_VALUE_ENUM_SINGLE_DECL(tac5x1x_out2_config_enum,
				  TAC5X1X_OUT2CFG0, 2, 0x7,
				  tac5x1x_output2_config_text,
				  tac5x1x_output2_config_values);

static const struct snd_kcontrol_new tac5x1x_dapm_out1_config_control[] = {
	SOC_DAPM_ENUM("OUT1X Config MUX", tac5x1x_out1_config_enum),
};

static const struct snd_kcontrol_new tac5x1x_dapm_out2_config_control[] = {
	SOC_DAPM_ENUM("OUT2X Config MUX", tac5x1x_out2_config_enum),
};

static const char *const tac5x1x_wideband_text[] = {
	"Audio BW 24-kHz",
	"Wide BW 96-kHz",
};

static SOC_ENUM_SINGLE_DECL(tac5x1x_adc1_wideband_enum, TAC5X1X_ADCCH1C0, 0,
		tac5x1x_wideband_text);
static SOC_ENUM_SINGLE_DECL(tac5x1x_adc2_wideband_enum, TAC5X1X_ADCCH2C0, 0,
		tac5x1x_wideband_text);
static SOC_ENUM_SINGLE_DECL(tac5x1x_dac1_wideband_enum, TAC5X1X_OUT1CFG1, 0,
		tac5x1x_wideband_text);
static SOC_ENUM_SINGLE_DECL(tac5x1x_dac2_wideband_enum, TAC5X1X_OUT2CFG1, 0,
		tac5x1x_wideband_text);

static const char *const tac5x1x_tolerance_text[] = {
	"AC Coupled with 100mVpp",
	"AC/DC Coupled with 1Vpp",
	"AC/DC Coupled with Rail-to-rail",
};

static SOC_ENUM_SINGLE_DECL(tac5x1x_adc1_tolerance_enum, TAC5X1X_ADCCH1C0, 2,
		tac5x1x_tolerance_text);
static SOC_ENUM_SINGLE_DECL(tac5x1x_adc2_tolerance_enum, TAC5X1X_ADCCH2C0, 2,
		tac5x1x_tolerance_text);

/* Output Drive Selection */
static const char *const tac5x1x_output_driver_text[] = {
	"Line-out",
	"Headphone",
	"4 ohm",
	"FD Receiver/Debug",
};

static SOC_ENUM_SINGLE_DECL(out1p_driver_enum, TAC5X1X_OUT1CFG1, 6,
		tac5x1x_output_driver_text);

static SOC_ENUM_SINGLE_DECL(out2p_driver_enum, TAC5X1X_OUT2CFG1, 6,
		tac5x1x_output_driver_text);

static const struct snd_kcontrol_new tac5x1x_dapm_out1_driver_control[] = {
	SOC_DAPM_ENUM("OUT1 driver MUX", out1p_driver_enum),
};

static const struct snd_kcontrol_new tac5x1x_dapm_out2_driver_control[] = {
	SOC_DAPM_ENUM("OUT2 driver MUX", out2p_driver_enum),
};

/* Decimation Filter Selection */
static const char *const decimation_filter_text[] = {
	"Linear Phase", "Low Latency", "Ultra-low Latency"};

static SOC_ENUM_SINGLE_DECL(decimation_filter_record_enum, TAC5X1X_DSP0, 6,
			decimation_filter_text);
static SOC_ENUM_SINGLE_DECL(decimation_filter_playback_enum, TAC5X1X_DSP1, 6,
			    decimation_filter_text);

static const struct snd_kcontrol_new tx_ch1_asi_switch =
	SOC_DAPM_SINGLE("Capture Switch", TAC5X1X_PASITXCH1, 5, 1, 0);
static const struct snd_kcontrol_new tx_ch2_asi_switch =
	SOC_DAPM_SINGLE("Capture Switch", TAC5X1X_PASITXCH2, 5, 1, 0);
static const struct snd_kcontrol_new tx_ch3_asi_switch =
	SOC_DAPM_SINGLE("Capture Switch", TAC5X1X_PASITXCH3, 5, 1, 0);
static const struct snd_kcontrol_new tx_ch4_asi_switch =
	SOC_DAPM_SINGLE("Capture Switch", TAC5X1X_PASITXCH4, 5, 1, 0);

static const struct snd_kcontrol_new rx_ch1_asi_switch =
	SOC_DAPM_SINGLE("Switch", TAC5X1X_PASIRXCH1, 5, 1, 0);
static const struct snd_kcontrol_new rx_ch2_asi_switch =
	SOC_DAPM_SINGLE("Switch", TAC5X1X_PASIRXCH2, 5, 1, 0);

static const char *const rx_ch5_asi_cfg_text[] = {
	"Disable",
	"DAC channel data",
	"ADC channel output loopback",
};

static const char *const rx_ch6_asi_cfg_text[] = {
	"Disable",
	"DAC channel data",
	"ADC channel output loopback",
	"Channel Input to ICLA device",
};

static const char *const tx_ch5_asi_cfg_text[] = {
	"Tristate",
	"Input Channel Loopback data",
	"Echo reference Channel data",
};

static const char *const tx_ch7_asi_cfg_text[] = {
	"Tristate",
	"Vbat_Wlby2,Temp_Wlby2",
	"echo_ref_ch1,echo_ref_ch2",
};

static const char *const tx_ch8_asi_cfg_text[] = {
	"Tristate",
	"ICLA data",
};

static const char *const diag_cfg_text[] = {
	"0mv", "30mv", "60mv", "90mv",
	"120mv", "150mv", "180mv", "210mv",
	"240mv", "270mv", "300mv", "330mv",
	"360mv", "390mv", "420mv", "450mv",
};

static const char *const diag_cfg_gnd_text[] = {
	"0mv", "60mv", "120mv", "180mv",
	"240mv", "300mv", "360mv", "420mv",
	"480mv", "540mv", "600mv", "660mv",
	"720mv", "780mv", "840mv", "900mv",
};

static const char *const tac5x1x_tdm_slot_text[] = {
	"Slot0", "Slot1", "Slot2", "Slot3",
	"Slot4", "Slot5", "Slot6", "Slot7",
	"Slot8", "Slot9", "Slot10", "Slot11",
	"Slot12", "Slot13", "Slot14", "Slot15",
	"Slot16", "Slot17", "Slot18", "Slot19",
	"Slot20", "Slot21", "Slot22", "Slot23",
	"Slot24", "Slot25", "Slot26", "Slot27",
	"Slot28", "Slot29", "Slot30", "Slot31",
};

static SOC_ENUM_SINGLE_DECL(tx_ch5_asi_cfg_enum, TAC5X1X_PASITXCH5, 5,
		tx_ch5_asi_cfg_text);
static SOC_ENUM_SINGLE_DECL(tx_ch6_asi_cfg_enum, TAC5X1X_PASITXCH6, 5,
		tx_ch5_asi_cfg_text);
static SOC_ENUM_SINGLE_DECL(tx_ch7_asi_cfg_enum, TAC5X1X_PASITXCH7, 5,
		tx_ch7_asi_cfg_text);
static SOC_ENUM_SINGLE_DECL(tx_ch8_asi_cfg_enum, TAC5X1X_PASITXCH8, 5,
		tx_ch8_asi_cfg_text);
static SOC_ENUM_SINGLE_DECL(rx_ch5_asi_cfg_enum, TAC5X1X_PASIRXCH5, 5,
		rx_ch5_asi_cfg_text);
static SOC_ENUM_SINGLE_DECL(rx_ch6_asi_cfg_enum, TAC5X1X_PASIRXCH6, 5,
		rx_ch6_asi_cfg_text);
static SOC_ENUM_SINGLE_DECL(rx_ch7_asi_cfg_enum, TAC5X1X_PASIRXCH7, 5,
		rx_ch6_asi_cfg_text);
static SOC_ENUM_SINGLE_DECL(rx_ch8_asi_cfg_enum, TAC5X1X_PASIRXCH8, 5,
		rx_ch6_asi_cfg_text);
static SOC_ENUM_SINGLE_DECL(diag_cfg1_sht_term_enum, TAC5X1X_DIAG_CFG1, 4,
		diag_cfg_text);
static SOC_ENUM_SINGLE_DECL(diag_cfg1_vbat_in_enum, TAC5X1X_DIAG_CFG1, 0,
		diag_cfg_text);
static SOC_ENUM_SINGLE_DECL(diag_cfg2_sht_gnd_enum, TAC5X1X_DIAG_CFG2, 4,
		diag_cfg_gnd_text);
static SOC_ENUM_SINGLE_DECL(diag_cfg2_micbias, TAC5X1X_DIAG_CFG2, 0,
		diag_cfg_text);

static SOC_ENUM_SINGLE_DECL(rx_ch1_slot_enum, TAC5X1X_PASIRXCH1, 0,
		tac5x1x_tdm_slot_text);
static SOC_ENUM_SINGLE_DECL(rx_ch2_slot_enum, TAC5X1X_PASIRXCH2, 0,
		tac5x1x_tdm_slot_text);
static SOC_ENUM_SINGLE_DECL(rx_ch3_slot_enum, TAC5X1X_PASIRXCH3, 0,
		tac5x1x_tdm_slot_text);
static SOC_ENUM_SINGLE_DECL(rx_ch4_slot_enum, TAC5X1X_PASIRXCH4, 0,
		tac5x1x_tdm_slot_text);
static SOC_ENUM_SINGLE_DECL(tx_ch1_slot_enum, TAC5X1X_PASITXCH1, 0,
		tac5x1x_tdm_slot_text);
static SOC_ENUM_SINGLE_DECL(tx_ch2_slot_enum, TAC5X1X_PASITXCH2, 0,
		tac5x1x_tdm_slot_text);
static SOC_ENUM_SINGLE_DECL(tx_ch3_slot_enum, TAC5X1X_PASITXCH3, 0,
		tac5x1x_tdm_slot_text);
static SOC_ENUM_SINGLE_DECL(tx_ch4_slot_enum, TAC5X1X_PASITXCH4, 0,
		tac5x1x_tdm_slot_text);

/* Record */
/* ADC Analog/PDM Selection */
static const char *const tac5x1x_input_source_text[] = {"Analog", "PDM"};

static SOC_ENUM_SINGLE_DECL(tac5x1x_in1_source_enum, TAC5X1X_INTF4, 7,
		tac5x1x_input_source_text);
static SOC_ENUM_SINGLE_DECL(tac5x1x_in2_source_enum, TAC5X1X_INTF4, 6,
		tac5x1x_input_source_text);

static const struct snd_kcontrol_new tac5x1x_dapm_in1_source_control[] = {
	SOC_DAPM_ENUM("CH1 Source MUX", tac5x1x_in1_source_enum),
};

static const struct snd_kcontrol_new tac5x1x_dapm_in2_source_control[] = {
	SOC_DAPM_ENUM("CH2 Source MUX", tac5x1x_in2_source_enum),
};

static const char *const tad5x1x_input_source_text[] = {"Disable", "PDM"};
static SOC_ENUM_SINGLE_DECL(tad5x1x_in1_source_enum, TAC5X1X_INTF4, 7,
		tad5x1x_input_source_text);
static SOC_ENUM_SINGLE_DECL(tad5x1x_in2_source_enum, TAC5X1X_INTF4, 6,
		tad5x1x_input_source_text);

static const struct snd_kcontrol_new tad5x1x_dapm_in1_source_control[] = {
	SOC_DAPM_ENUM("CH1 Source MUX", tad5x1x_in1_source_enum),
};

static const struct snd_kcontrol_new tad5x1x_dapm_in2_source_control[] = {
	SOC_DAPM_ENUM("CH2 Source MUX", tad5x1x_in2_source_enum),
};

/* ADC Analog source Selection */
static const char *const tac5x1x_input_analog_sel_text[] = {
	"Differential",
	"Single-ended",
	"Single-ended mux INxP",
	"Single-ended mux INxM",
};

static const char *const tac5x1x_input_analog2_sel_text[] = {
	"Differential",
	"Single-ended",
};

static SOC_ENUM_SINGLE_DECL(tac5x1x_adc1_config_enum, TAC5X1X_ADCCH1C0, 6,
		tac5x1x_input_analog_sel_text);
static SOC_ENUM_SINGLE_DECL(tac5x1x_adc2_config_enum, TAC5X1X_ADCCH2C0, 6,
		tac5x1x_input_analog2_sel_text);

static const struct snd_kcontrol_new tac5x1x_dapm_adc1_config_control[] = {
	SOC_DAPM_ENUM("ADC1 Analog MUX", tac5x1x_adc1_config_enum),
};

static const struct snd_kcontrol_new tac5x1x_dapm_adc2_config_control[] = {
	SOC_DAPM_ENUM("ADC2 Analog MUX", tac5x1x_adc2_config_enum),
};

static const struct snd_kcontrol_new taa5x1x_controls[] = {
	SOC_ENUM("Record Decimation Filter",
		 decimation_filter_record_enum),
	SOC_ENUM("ADC1 Audio BW", tac5x1x_adc1_wideband_enum),

	SOC_SINGLE_TLV("ADC1 Digital Capture Volume", TAC5X1X_ADCCH1C2,
		       0, 0xff, 0, record_dig_vol_tlv),

	SOC_SINGLE_TLV("ADC1 Fine Capture Volume", TAC5X1X_ADCCH1C3,
		       0, 0xff, 0, record_gain_cali_tlv),

	SOC_SINGLE_RANGE("ADC1 Phase Capture Volume", TAC5X1X_ADCCH1C4,
			 2, 0, 63, 0),

	SOC_ENUM("ASI_TX_CH5_CFG", tx_ch5_asi_cfg_enum),
	SOC_ENUM("ASI_TX_CH6_CFG", tx_ch6_asi_cfg_enum),
	SOC_ENUM("ASI_TX_CH7_CFG", tx_ch7_asi_cfg_enum),
	SOC_ENUM("ASI_TX_CH8_CFG", tx_ch8_asi_cfg_enum),
	SOC_SINGLE("IN_CH1_EN Capture Switch", TAC5X1X_CH_EN, 7, 1, 0),
	SOC_SINGLE("IN_CH2_EN Capture Switch", TAC5X1X_CH_EN, 6, 1, 0),
	SOC_SINGLE("IN_CH3_EN Capture Switch", TAC5X1X_CH_EN, 5, 1, 0),
	SOC_SINGLE("IN_CH4_EN Capture Switch", TAC5X1X_CH_EN, 4, 1, 0),
};

static const struct snd_kcontrol_new tac5x1x_tdm_slot_controls[] = {
	SOC_ENUM("ASI_RX_CH1 Slot", rx_ch1_slot_enum),
	SOC_ENUM("ASI_RX_CH2 Slot", rx_ch2_slot_enum),
	SOC_ENUM("ASI_RX_CH3 Slot", rx_ch3_slot_enum),
	SOC_ENUM("ASI_RX_CH4 Slot", rx_ch4_slot_enum),
	SOC_ENUM("ASI_TX_CH1 Slot", tx_ch1_slot_enum),
	SOC_ENUM("ASI_TX_CH2 Slot", tx_ch2_slot_enum),
	SOC_ENUM("ASI_TX_CH3 Slot", tx_ch3_slot_enum),
	SOC_ENUM("ASI_TX_CH4 Slot", tx_ch4_slot_enum),
};

static const struct snd_kcontrol_new tad5x1x_controls[] = {
	SOC_ENUM("Playback Decimation Filter",
		 decimation_filter_playback_enum),
	SOC_ENUM("DAC1 Audio BW", tac5x1x_dac1_wideband_enum),
	SOC_SINGLE_TLV("OUT1P Analog Level Playback Volume", TAC5X1X_OUT1CFG1,
		       3, 6, 1, playback_analog_level_tlv),
	SOC_SINGLE_TLV("OUT1M Analog Level Playback Volume", TAC5X1X_OUT1CFG2,
		       3, 6, 1, playback_analog_level_tlv),
	SOC_SINGLE_TLV("DAC1 CHA Digital Playback Volume", TAC5X1X_DACCH1A0,
		       0, 0xff, 0, dac_dig_vol_tlv),
	SOC_SINGLE_TLV("DAC1 CHB Digital Playback Volume", TAC5X1X_DACCH1B0,
		       0, 0xff, 0, dac_dig_vol_tlv),
	SOC_SINGLE_TLV("DAC1 CHA Gain Calibration Playback Volume",
		       TAC5X1X_DACCH1A1, 4, 0xf, 0,
		       playback_gain_cali_tlv),
	SOC_SINGLE_TLV("DAC1 CHB Gain Calibration Playback Volume",
		       TAC5X1X_DACCH1B1, 4, 0xf, 0,
		       playback_gain_cali_tlv),

	SOC_SINGLE("ASI_RX_CH3_EN Playback Switch",
		   TAC5X1X_PASIRXCH3, 5, 1, 0),
	SOC_SINGLE("ASI_RX_CH4_EN Playback Switch",
		   TAC5X1X_PASIRXCH4, 5, 1, 0),
	SOC_ENUM("ASI_RX_CH5_EN Playback", rx_ch5_asi_cfg_enum),
	SOC_ENUM("ASI_RX_CH6_EN Playback", rx_ch6_asi_cfg_enum),
	SOC_ENUM("ASI_RX_CH7_EN Playback", rx_ch7_asi_cfg_enum),
	SOC_ENUM("ASI_RX_CH8_EN Playback", rx_ch8_asi_cfg_enum),
	SOC_SINGLE("OUT_CH1_EN Playback Switch", TAC5X1X_CH_EN, 3, 1, 0),
	SOC_SINGLE("OUT_CH2_EN Playback Switch", TAC5X1X_CH_EN, 2, 1, 0),
	SOC_SINGLE("OUT_CH3_EN Playback Switch", TAC5X1X_CH_EN, 1, 1, 0),
	SOC_SINGLE("OUT_CH4_EN Playback Switch", TAC5X1X_CH_EN, 0, 1, 0),
};

static const struct snd_kcontrol_new tac5x11_controls[] = {
	SOC_ENUM("ADC1 Common-mode Tolerance", tac5x1x_adc1_tolerance_enum),
};

static const struct snd_kcontrol_new tad5x12_controls[] = {
	SOC_SINGLE_TLV("OUT2P Analog Level Playback Volume", TAC5X1X_OUT2CFG1,
		       3, 6, 1, playback_analog_level_tlv),
	SOC_SINGLE_TLV("OUT2M Analog Level Playback Volume", TAC5X1X_OUT2CFG2,
		       3, 6, 1, playback_analog_level_tlv),
	SOC_SINGLE_TLV("DAC2 CHA Digital Playback Volume", TAC5X1X_DACCH2A0,
		       0, 0xff, 0, dac_dig_vol_tlv),
	SOC_SINGLE_TLV("DAC2 CHB Digital Playback Volume", TAC5X1X_DACCH2B0,
		       0, 0xff, 0, dac_dig_vol_tlv),
	SOC_SINGLE_TLV("DAC2 CHA Gain Calibration Playback Volume",
		       TAC5X1X_DACCH2A1, 4, 0xf, 0,
			playback_gain_cali_tlv),
	SOC_SINGLE_TLV("DAC2 CHB Gain Calibration Playback Volume",
		       TAC5X1X_DACCH2B1, 4, 0xf, 0,
			playback_gain_cali_tlv),
	SOC_ENUM("DAC2 Audio BW", tac5x1x_dac2_wideband_enum),
};

static const struct snd_kcontrol_new taa5x12_controls[] = {
	SOC_ENUM("ADC2 Audio BW", tac5x1x_adc2_wideband_enum),

	SOC_SINGLE_TLV("ADC2 Digital Capture Volume", TAC5X1X_ADCCH2C2,
		       0, 0xff, 0, record_dig_vol_tlv),

	SOC_SINGLE_TLV("ADC2 Fine Capture Volume", TAC5X1X_ADCCH2C3,
		       0, 0xff, 0, record_gain_cali_tlv),

	SOC_SINGLE_RANGE("ADC2 Phase Capture Volume", TAC5X1X_ADCCH2C4,
			 2, 0, 63, 0),
};

static const struct snd_kcontrol_new tolerance_ctrls[] = {
	SOC_ENUM("ADC1 Common-mode Tolerance", tac5x1x_adc1_tolerance_enum),
	SOC_ENUM("ADC2 Common-mode Tolerance", tac5x1x_adc2_tolerance_enum),
};

static const struct snd_kcontrol_new tac5x1x_pdm_controls[] = {
	SOC_ENUM("PDM Clk Divider", pdmclk_select_enum),

	SOC_SINGLE_TLV("PDM1 Digital Capture Volume", TAC5X1X_ADCCH1C2,
		       0, 0xff, 0, record_dig_vol_tlv),
	SOC_SINGLE_TLV("PDM2 Digital Capture Volume", TAC5X1X_ADCCH2C2,
		       0, 0xff, 0, record_dig_vol_tlv),
	SOC_SINGLE_TLV("PDM1 Fine Capture Volume", TAC5X1X_ADCCH1C3,
		       0, 0xff, 0, record_gain_cali_tlv),
	SOC_SINGLE_TLV("PDM2 Fine Capture Volume", TAC5X1X_ADCCH2C3,
		       0, 0xff, 0, record_gain_cali_tlv),
	SOC_SINGLE_RANGE("PDM1 Phase Capture Volume", TAC5X1X_ADCCH1C4,
			 2, 0, 63, 0),
	SOC_SINGLE_RANGE("PDM2 Phase Capture Volume", TAC5X1X_ADCCH2C4,
			 2, 0, 63, 0),
	SOC_SINGLE_TLV("PDM3 Digital Capture Volume", TAC5X1X_ADCCH3C2,
		       0, 0xff, 0, record_dig_vol_tlv),
	SOC_SINGLE_TLV("PDM4 Digital Capture Volume", TAC5X1X_ADCCH4C2,
		       0, 0xff, 0, record_dig_vol_tlv),
	SOC_SINGLE_TLV("PDM3 Fine Capture Volume", TAC5X1X_ADCCH3C3,
		       0, 0xff, 0, record_gain_cali_tlv),
	SOC_SINGLE_TLV("PDM4 Fine Capture Volume", TAC5X1X_ADCCH4C3,
		       0, 0xff, 0, record_gain_cali_tlv),
	SOC_SINGLE_RANGE("PDM3 Phase Capture Volume", TAC5X1X_ADCCH3C4,
			 2, 0, 63, 0),
	SOC_SINGLE_RANGE("PDM4 Phase Capture Volume", TAC5X1X_ADCCH4C4,
			 2, 0, 63, 0),
};

static const struct snd_kcontrol_new taa_ip_controls[] = {
	SOC_ENUM("DIAG_SHT_TERM", diag_cfg1_sht_term_enum),
	SOC_ENUM("DIAG_SHT_VBAT_IN", diag_cfg1_vbat_in_enum),
	SOC_ENUM("DIAG_SHT_GND", diag_cfg2_sht_gnd_enum),
	SOC_ENUM("DIAG_SHT_MICBIAS", diag_cfg2_micbias),
};

static const struct snd_soc_dapm_widget taa5x1x_dapm_widgets[] = {
	/* ADC1 */
	SND_SOC_DAPM_INPUT("AIN1"),
	SND_SOC_DAPM_MUX("ADC1 Full-Scale", SND_SOC_NOPM, 0, 0,
			 tac5x1x_dapm_adc1_fscale_control),
	SND_SOC_DAPM_MUX("ADC1 Config", SND_SOC_NOPM, 0, 0,
			 tac5x1x_dapm_adc1_config_control),
	SND_SOC_DAPM_ADC_E("CH1_ADC_EN", "CH1 Capture", SND_SOC_NOPM, 0, 0,
			   tac5x1x_adc_event,
			   SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_SWITCH("ASI_TX_CH1_EN", SND_SOC_NOPM, 0, 0,
			    &tx_ch1_asi_switch),
	SND_SOC_DAPM_MICBIAS("Mic Bias", SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_SWITCH("ASI_TX_CH2_EN", SND_SOC_NOPM, 0, 0,
			    &tx_ch2_asi_switch),
};

static const struct snd_soc_dapm_widget tad5xx_dapm_widgets[] = {
	/* pdm capture */
	SND_SOC_DAPM_SWITCH("ASI_TX_CH1_EN", SND_SOC_NOPM, 0, 0,
			    &tx_ch1_asi_switch),
	SND_SOC_DAPM_SWITCH("ASI_TX_CH2_EN", SND_SOC_NOPM, 0, 0,
			    &tx_ch2_asi_switch),
};

static const struct snd_soc_dapm_widget tad5x1x_dapm_widgets[] = {
	/* DAC1 */
	SND_SOC_DAPM_AIF_IN("ASI IN1", "ASI Playback", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_OUTPUT("OUT1"),
	SND_SOC_DAPM_MUX("OUT1x Source", SND_SOC_NOPM, 0, 0,
			 tac5x1x_dapm_out1_source_control),
	SND_SOC_DAPM_MUX("OUT1x Config", SND_SOC_NOPM, 0, 0,
			 tac5x1x_dapm_out1_config_control),
	SND_SOC_DAPM_MUX("OUT1x Driver", SND_SOC_NOPM, 0, 0,
			 tac5x1x_dapm_out1_driver_control),
	SND_SOC_DAPM_DAC_E("Left DAC Enable", "ASI Playback", SND_SOC_NOPM, 0, 0,
			   tac5x1x_dac_event,
			   SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_SWITCH("ASI_RX_CH1_EN", SND_SOC_NOPM, 0, 0,
			    &rx_ch1_asi_switch),
};

static const struct snd_soc_dapm_widget taa5x12_dapm_widgets[] = {
	/* ADC2 */
	SND_SOC_DAPM_INPUT("AIN2"),
	SND_SOC_DAPM_MUX("ADC2 Full-Scale", SND_SOC_NOPM, 0, 0,
			 tac5x1x_dapm_adc2_fscale_control),
	SND_SOC_DAPM_MUX("ADC2 Config", SND_SOC_NOPM, 0, 0,
			 tac5x1x_dapm_adc2_config_control),
	SND_SOC_DAPM_ADC_E("CH2_ADC_EN", "CH2 Capture", SND_SOC_NOPM, 1, 0,
			   tac5x1x_adc_event,
			   SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
};

static const struct snd_soc_dapm_widget tad5x12_dapm_widgets[] = {
	/* DAC2 */
	SND_SOC_DAPM_OUTPUT("OUT2"),

	SND_SOC_DAPM_AIF_IN("ASI IN2", "ASI Playback", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_MUX("OUT2x Source", SND_SOC_NOPM, 0, 0,
			 tac5x1x_dapm_out2_source_control),
	SND_SOC_DAPM_MUX("OUT2x Config", SND_SOC_NOPM, 0, 0,
			 tac5x1x_dapm_out2_config_control),
	SND_SOC_DAPM_MUX("OUT2x Driver", SND_SOC_NOPM, 0, 0,
			 tac5x1x_dapm_out2_driver_control),
	SND_SOC_DAPM_DAC_E("Right DAC Enable", "ASI Playback", SND_SOC_NOPM, 1, 0,
			   tac5x1x_dac_event,
			   SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_SWITCH("ASI_RX_CH2_EN", SND_SOC_NOPM, 0, 0,
			    &rx_ch2_asi_switch),
};

static const struct snd_soc_dapm_widget tac5x1x_pdm_widgets[] = {
	/* PDM - no event handler, power is managed via adc_event + post_powerup_work */
	SND_SOC_DAPM_INPUT("DIN1"),
	SND_SOC_DAPM_INPUT("DIN2"),
	SND_SOC_DAPM_INPUT("DIN3"),
	SND_SOC_DAPM_INPUT("DIN4"),

	SND_SOC_DAPM_ADC_E("CH1_PDM_EN", "PDM CH1 Capture", SND_SOC_NOPM, 0, 0,
			   tac5x1x_adc_event,
			   SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_ADC_E("CH2_PDM_EN", "PDM CH2 Capture", SND_SOC_NOPM, 1, 0,
			   tac5x1x_adc_event,
			   SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_ADC_E("CH3_PDM_EN", "PDM CH3 Capture", SND_SOC_NOPM, 2, 0,
			   tac5x1x_adc_event,
			   SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_ADC_E("CH4_PDM_EN", "PDM CH4 Capture", SND_SOC_NOPM, 3, 0,
			   tac5x1x_adc_event,
			   SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),

	SND_SOC_DAPM_SWITCH("ASI_TX_CH3_EN", SND_SOC_NOPM, 0, 0,
			    &tx_ch3_asi_switch),
	SND_SOC_DAPM_SWITCH("ASI_TX_CH4_EN", SND_SOC_NOPM, 0, 0,
			    &tx_ch4_asi_switch),
};

static const struct snd_soc_dapm_widget tac5x1x_common_widgets[] = {
	SND_SOC_DAPM_MUX("IN1 Source Mux", SND_SOC_NOPM, 0, 0,
			 tac5x1x_dapm_in1_source_control),
	SND_SOC_DAPM_MUX("IN2 Source Mux", SND_SOC_NOPM, 0, 0,
			 tac5x1x_dapm_in2_source_control),
	SND_SOC_DAPM_AIF_OUT("AIF OUT", "ASI Capture", 0, SND_SOC_NOPM, 0, 0),
};

static const struct snd_soc_dapm_widget tad_common_widgets[] = {
	SND_SOC_DAPM_MUX("IN1 Source Mux", SND_SOC_NOPM, 0, 0,
			 tad5x1x_dapm_in1_source_control),
	SND_SOC_DAPM_MUX("IN2 Source Mux", SND_SOC_NOPM, 0, 0,
			 tad5x1x_dapm_in2_source_control),
	SND_SOC_DAPM_AIF_OUT("AIF OUT", "ASI Capture", 0, SND_SOC_NOPM, 0, 0),
};

static const struct snd_soc_dapm_route taa5x1x_dapm_routes[] = {
	/*
	 * ADC channel1
	 * ADC path: AIN1 -> CH1_ADC_EN -> ADC1 Config ->
	 *           ADC1 Full-Scale -> Mic Bias -> IN1 Source Mux (Analog) ->
	 *           ASI_TX_CH1_EN -> AIF OUT
	 * PDM path: DIN1 -> IN1 Source Mux (PDM) ->
	 *           ASI_TX_CH1_EN -> CH1_PDM_EN -> AIF OUT
	 *
	 * Note: The route {"IN1 Source Mux", "Analog", "Mic Bias"} is in
	 * tac_common_routes because IN1 Source Mux widget is created later.
	 */
	{"CH1_ADC_EN", NULL, "AIN1"},
	{"ADC1 Config", "Differential", "CH1_ADC_EN"},
	{"ADC1 Config", "Single-ended", "CH1_ADC_EN"},
	{"ADC1 Config", "Single-ended mux INxP", "CH1_ADC_EN"},
	{"ADC1 Config", "Single-ended mux INxM", "CH1_ADC_EN"},

	{"ADC1 Full-Scale", "2/10-VRMS", "ADC1 Config"},
	{"ADC1 Full-Scale", "4/5-VRMS", "ADC1 Config"},
	{"Mic Bias", NULL, "ADC1 Full-Scale"},

};

static const struct snd_soc_dapm_route tad5x1x_dapm_routes[] = {
	/* Left Output */
	{"ASI_RX_CH1_EN", "Switch", "ASI IN1"},

	{"OUT1x Source", "DAC + Analog Bypass Mix", "ASI_RX_CH1_EN"},
	{"OUT1x Source", "DAC -> OUTxP, INxP -> OUTxM", "ASI_RX_CH1_EN"},
	{"OUT1x Source", "INxM -> OUTxP, DAC -> OUTxM", "ASI_RX_CH1_EN"},

	{"OUT1x Config", "Differential", "OUT1x Source"},
	// {"OUT1x Config", "Stereo Single-ended", "OUT1x Source"},
	{"OUT1x Config", "Mono Single-ended at OUTxP only", "OUT1x Source"},
	{"OUT1x Config", "Mono Single-ended at OUTxM only", "OUT1x Source"},
	{"OUT1x Config", "Pseudo differential with OUTxM as VCOM",
		"OUT1x Source"},
	{"OUT1x Config", "Pseudo differential with OUTxM as external sensing",
		"OUT1x Source"},
	{"OUT1x Config", "Pseudo differential with OUTxP as VCOM",
		"OUT1x Source"},

	{"OUT1x Driver", "Line-out", "OUT1x Config"},
	{"OUT1x Driver", "Headphone", "OUT1x Config"},

	{"Left DAC Enable", NULL, "OUT1x Driver"},
	{"OUT1", NULL, "Left DAC Enable"},
};

static const struct snd_soc_dapm_route taa5x12_dapm_routes[] = {
	/*
	 * ADC channel2
	 * Same design as ADC channel1.
	 * Note: The route {"IN2 Source Mux", "Analog", "Mic Bias"} is in
	 * tac_common_routes because IN2 Source Mux widget is created later.
	 */
	{"CH2_ADC_EN", NULL, "AIN2"},
	{"ADC2 Config", "Differential", "CH2_ADC_EN"},
	{"ADC2 Config", "Single-ended", "CH2_ADC_EN"},
	{"ADC2 Full-Scale", "2/10-VRMS", "ADC2 Config"},
	{"ADC2 Full-Scale", "4/5-VRMS", "ADC2 Config"},

	{"Mic Bias", NULL, "ADC2 Full-Scale"},
};

static const struct snd_soc_dapm_route tad5x12_dapm_routes[] = {
	/* Right Output */
	{"ASI_RX_CH2_EN", "Switch", "ASI IN2"},

	{"OUT2x Source", "DAC + Analog Bypass Mix", "ASI_RX_CH1_EN"},
	{"OUT2x Source", "DAC -> OUTxP, INxP -> OUTxM", "ASI_RX_CH1_EN"},
	{"OUT2x Source", "INxM -> OUTxP, DAC -> OUTxM", "ASI_RX_CH1_EN"},

	{"OUT2x Config", "Differential", "OUT2x Source"},
	// {"OUT2x Config", "Stereo Single-ended", "OUT2x Source"},
	{"OUT2x Config", "Mono Single-ended at OUTxP only", "OUT2x Source"},
	{"OUT2x Config", "Mono Single-ended at OUTxM only", "OUT2x Source"},
	{"OUT2x Config", "Pseudo differential with OUTxM as VCOM",
		"OUT2x Source"},
	{"OUT2x Config", "Pseudo differential with OUTxP as VCOM",
		"OUT2x Source"},
	{"OUT2x Driver", "Line-out", "OUT2x Config"},
	{"OUT2x Driver", "Headphone", "OUT2x Config"},
	{"Right DAC Enable", NULL, "OUT2x Driver"},
	{"OUT2", NULL, "Right DAC Enable"},
};

static const struct snd_soc_dapm_route tac5x1x_pdm_routes[] = {
	/* PDM channel1 & Channel2
	 * PDM path directly from inputs through source mux
	 */
	{"IN1 Source Mux", "PDM", "DIN1"},
	{"IN2 Source Mux", "PDM", "DIN2"},

	{"ASI_TX_CH1_EN", "Capture Switch", "IN1 Source Mux"},
	{"ASI_TX_CH2_EN", "Capture Switch", "IN2 Source Mux"},

	{"CH1_PDM_EN", NULL, "ASI_TX_CH1_EN"},
	{"CH2_PDM_EN", NULL, "ASI_TX_CH2_EN"},

	{"AIF OUT", NULL, "CH1_PDM_EN"},
	{"AIF OUT", NULL, "CH2_PDM_EN"},

	/* PDM channel3 & Channel4 */
	{"ASI_TX_CH3_EN", "Capture Switch", "DIN3"},
	{"ASI_TX_CH4_EN", "Capture Switch", "DIN4"},

	{"CH3_PDM_EN", NULL, "ASI_TX_CH3_EN"},
	{"CH4_PDM_EN", NULL, "ASI_TX_CH4_EN"},

	{"AIF OUT", NULL, "CH3_PDM_EN"},
	{"AIF OUT", NULL, "CH4_PDM_EN"},
};

static const struct snd_soc_dapm_route tac_common_routes[] = {
	/*
	 * Analog ADC path - routed through Source Mux
	 * These routes must be here because IN1/IN2 Source Mux widgets
	 * are created in tac5x1x_common_widgets which is added after
	 * device-specific routes. The Analog path is already gated by
	 * CH1_Enable/CH2_Enable through the ADC routes in device-specific files.
	 */
	{"IN1 Source Mux", "Analog", "Mic Bias"},
	{"IN2 Source Mux", "Analog", "Mic Bias"},

	/* Connect Source Mux to ASI_TX_EN for ADC capture */
	{"ASI_TX_CH1_EN", "Capture Switch", "IN1 Source Mux"},
	{"ASI_TX_CH2_EN", "Capture Switch", "IN2 Source Mux"},

	/*
	 * Direct routes from ASI_TX_CHx_EN to AIF OUT for ADC path.
	 * PDM path goes through CHx_PDM_EN widgets which have their own
	 * routes to AIF OUT. This allows ADC and PDM to share ASI_TX_CHx_EN
	 * while maintaining separate event handlers.
	 */
	{"AIF OUT", NULL, "ASI_TX_CH1_EN"},
	{"AIF OUT", NULL, "ASI_TX_CH2_EN"},
};

/* TAD routes - PDM only, no ADC/Mic Bias */
static const struct snd_soc_dapm_route tad_common_routes[] = {
	/* Connect Source Mux to ASI_TX_EN for PDM capture */
	{"ASI_TX_CH1_EN", "Capture Switch", "IN1 Source Mux"},
	{"ASI_TX_CH2_EN", "Capture Switch", "IN2 Source Mux"},

	/* Routes from ASI_TX_CHx_EN to AIF OUT for PDM path */
	{"AIF OUT", NULL, "ASI_TX_CH1_EN"},
	{"AIF OUT", NULL, "ASI_TX_CH2_EN"},
};

static bool is_stereo_device(struct tac5x1x *tac5x1x)
{
	switch (tac5x1x->codec_type) {
	case TAA5212:
	case TAA5412:
	case TAC5112:
	case TAC5212:
	case TAC5312:
	case TAC5412:
	case TAD5112:
	case TAD5212:
		return true;
	default:
		return false;
	}
}

static s32 tac5x1x_pwr_ctrl(struct snd_soc_component *component,
			    bool power_state)
{
	struct tac5x1x_priv *tac5x1x_priv =
		snd_soc_component_get_drvdata(component);
	struct tac5x1x *tac5x1x = tac5x1x_priv->tac5x1x;
	struct device *parent = component->dev->parent;
	s32 active_ctrl, ret;
	s32 pwr_ctrl = 0;

	/* Ensure parent device is active before accessing registers */
	ret = pm_runtime_resume_and_get(parent);
	if (ret < 0) {
		dev_err(component->dev,
			"Failed to resume parent: %d\n", ret);
		return ret;
	}

	if (power_state) {
		active_ctrl = TAC5X1X_VREF_SLEEP_ACTIVE_MASK;
		snd_soc_component_update_bits(component, TAC5X1X_VREFCFG,
					      TAC5X1X_VREFCFG_MICBIAS_VAL_MASK,
					      tac5x1x->micbias_vg << 2);
		snd_soc_component_update_bits(component, TAC5X1X_VREFCFG,
					      TAC5X1X_VREFCFG_VREF_FSCALE_MASK,
					      tac5x1x->vref_vg);

		if (tac5x1x->uad_en)
			pwr_ctrl |= TAC5X1X_PWR_CFG_UAD_EN;
		if (tac5x1x->vad_en)
			pwr_ctrl |= TAC5X1X_PWR_CFG_VAD_EN;
		if (tac5x1x->uag_en)
			pwr_ctrl |= TAC5X1X_PWR_CFG_UAG_EN;
	} else {
		active_ctrl = 0x0;
	}

	ret = snd_soc_component_update_bits(component, TAC5X1X_VREF,
					    TAC5X1X_VREF_SLEEP_EXIT_VREF_EN |
					    TAC5X1X_VREF_SLEEP_ACTIVE_MASK,
					    active_ctrl);
	if (ret < 0) {
		dev_err(tac5x1x->dev,
			"%s, device active or sleep failed!, ret %d/n",
			__func__, ret);
		goto out;
	}

	ret = snd_soc_component_update_bits(component, TAC5X1X_PWR_CFG,
					    TAC5X1X_PWR_CFG_UAD_EN |
					    TAC5X1X_PWR_CFG_UAG_EN |
					    TAC5X1X_PWR_CFG_VAD_EN, pwr_ctrl);
	if (ret < 0)
		dev_err(tac5x1x->dev,
			"%s, Power control set failed!, ret %d/n",
			__func__, ret);

out:
	/* Release parent PM reference, autosuspend will handle delay */
	pm_runtime_mark_last_busy(parent);
	pm_runtime_put_autosuspend(parent);

	return ret;
}

static s32 tac5x1x_set_dai_fmt(struct snd_soc_dai *codec_dai, u32 fmt)
{
	struct snd_soc_component *component = codec_dai->component;
	s32 iface_reg_1 = 0;
	s32 iface_reg_2 = 0;
	s32 iface_reg_3 = 0;
	s32 clk_pol = 0;
	bool set_slot_positions = false;
	int right_slot = 1;

	switch (fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) {
	case SND_SOC_DAIFMT_CBP_CFP:
		iface_reg_1 |= TAC5X1X_PASI_MODE_MASK;
		break;
	case SND_SOC_DAIFMT_CBC_CFC:
		break;
	default:
		dev_err(component->dev,
			"%s: invalid DAI master/slave interface\n",
			__func__);
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
		iface_reg_2 |= TAC5X1X_PASI_FMT_I2S;
		right_slot = 16;
		set_slot_positions = true;
		break;
	case SND_SOC_DAIFMT_DSP_A:
		iface_reg_2 |= TAC5X1X_PASI_FMT_TDM;
		iface_reg_3 |= BIT(0); /* add offset 1 */
		break;
	case SND_SOC_DAIFMT_DSP_B:
		iface_reg_2 |= TAC5X1X_PASI_FMT_TDM;
		break;
	case SND_SOC_DAIFMT_LEFT_J:
		iface_reg_2 |= TAC5X1X_PASI_FMT_LJ;
		right_slot = 16;
		set_slot_positions = true;
		break;
	default:
		dev_err(component->dev,
			"%s: invalid DAI interface format\n", __func__);
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_NB_NF:
		break;
	case SND_SOC_DAIFMT_NB_IF:
		clk_pol |= TAC5X1X_PASI_FSYNC_POL;
		break;
	case SND_SOC_DAIFMT_IB_NF:
		clk_pol |= TAC5X1X_PASI_BCLK_POL;
		break;
	case SND_SOC_DAIFMT_IB_IF:
		clk_pol |= TAC5X1X_PASI_BCLK_POL | TAC5X1X_PASI_FSYNC_POL;
		break;
	default:
		dev_err(component->dev,
			"%s: invalid DAI clock polarity\n", __func__);
		return -EINVAL;
	}

	/* Clock provider mode */
	snd_soc_component_update_bits(component, TAC5X1X_CNTCLK2,
				      TAC5X1X_PASI_MODE_MASK, iface_reg_1);

	/* ASI format */
	snd_soc_component_update_bits(component, TAC5X1X_PASI0,
				      TAC5X1X_PASI_FMT_MASK, iface_reg_2);

	/* BCLK/FSYNC polarity */
	snd_soc_component_update_bits(component, TAC5X1X_PASI0,
				      TAC5X1X_PASI_BCLK_POL |
				      TAC5X1X_PASI_FSYNC_POL, clk_pol);

	/* TX slot offset */
	snd_soc_component_update_bits(component, TAC5X1X_PASITX1,
				      TAC5X1X_PASITX_OFFSET_MASK, iface_reg_3);

	/* RX slot offset */
	snd_soc_component_update_bits(component, TAC5X1X_PASIRX0,
				      TAC5X1X_PASIRX_OFFSET_MASK, iface_reg_3);

	/* Set slot positions only for I2S mode (left=0, right=16) */
	if (set_slot_positions) {
		snd_soc_component_update_bits(component, TAC5X1X_PASIRXCH1,
					      TAC5X1X_PASIRX_OFFSET_MASK, 0);
		snd_soc_component_update_bits(component, TAC5X1X_PASITXCH1,
					      TAC5X1X_PASITX_OFFSET_MASK, 0);
		snd_soc_component_update_bits(component, TAC5X1X_PASIRXCH2,
					      TAC5X1X_PASIRX_OFFSET_MASK, right_slot);
		snd_soc_component_update_bits(component, TAC5X1X_PASITXCH2,
					      TAC5X1X_PASITX_OFFSET_MASK, right_slot);
	}

	return 0;
}

static s32 tac5x1x_hw_params(struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *params,
			     struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	s32 sample_rate, word_length = 0;

	switch (params_rate(params)) {
	case 24000:
		sample_rate = 25;
		break;
	case 32000:
		sample_rate = 23;
		break;
	case 44100:
	case 48000:
		sample_rate = 20;
		break;
	case 64000:
		sample_rate = 18;
		break;
	case 96000:
		sample_rate = 15;
		break;
	case 192000:
		sample_rate = 10;
		break;
	default:
		/* Auto detect sample rate */
		sample_rate = 0;
		break;
	}

	switch (params_physical_width(params)) {
	case 16:
		word_length |= TAC5X1X_WORD_LEN_16BITS;
		break;
	case 20:
		word_length |= TAC5X1X_WORD_LEN_20BITS;
		break;
	case 24:
		word_length |= TAC5X1X_WORD_LEN_24BITS;
		break;
	case 32:
		word_length |= TAC5X1X_WORD_LEN_32BITS;
		break;
	default:
		dev_err(component->dev, "%s, set word length failed\n",
			__func__);
		return -EINVAL;
	}

	dev_dbg(component->dev, "sample rate: %d, word length: %d\n",
		sample_rate, word_length);

	snd_soc_component_update_bits(component, TAC5X1X_PASI0,
				      TAC5X1X_PASI_DATALEN_MASK, word_length);

	tac5x1x_pwr_ctrl(component, true);
	return 0;
}

static s32 tac5x1x_set_bias_level(struct snd_soc_component *component,
				  enum snd_soc_bias_level level)
{
	s32 ret = 0;

	switch (level) {
	case SND_SOC_BIAS_ON:
		ret = tac5x1x_pwr_ctrl(component, true);
		if (ret < 0)
			dev_err(component->dev,
				"%s, power up failed!\n", __func__);
		break;
	case SND_SOC_BIAS_PREPARE:
		break;
	case SND_SOC_BIAS_STANDBY:
		break;
	case SND_SOC_BIAS_OFF:
		ret = tac5x1x_pwr_ctrl(component, false);
		if (ret < 0)
			dev_err(component->dev,
				"%s, power down failed!\n", __func__);
		break;
	}

	return ret;
}

static const struct snd_soc_dai_ops tac5x1x_ops = {
	.hw_params = tac5x1x_hw_params,
	.set_fmt = tac5x1x_set_dai_fmt,
	.no_capture_mute = 1,
};

static struct snd_soc_dai_driver tac5x1x_dai = {
	.name = "tac5x1x-hifi",
	.playback = {
			.stream_name = "ASI Playback",
			.channels_min = 1,
			.channels_max = 4,
			.rates = TAC5X1X_RATES,
			.formats = TAC5X1X_FORMATS,},
	.capture = {
			.stream_name = "ASI Capture",
			.channels_min = 1,
			.channels_max = 4,
			.rates = TAC5X1X_RATES,
			.formats = TAC5X1X_FORMATS,
			},
	.ops = &tac5x1x_ops,
	.symmetric_rate = 1,
};

static struct snd_soc_dai_driver taa5x1x_dai = {
	.name = "taa5x1x-hifi",
	.capture = {
			.stream_name = "ASI Capture",
			.channels_min = 1,
			.channels_max = 4,
			.rates = TAC5X1X_RATES,
			.formats = TAC5X1X_FORMATS,
			},
	.ops = &tac5x1x_ops,
};

static struct snd_soc_dai_driver tad5x1x_dai = {
	.name = "tad5x1x-hifi",
	.playback = {
			.stream_name = "ASI Playback",
			.channels_min = 1,
			.channels_max = 4,
			.rates = TAC5X1X_RATES,
			.formats = TAC5X1X_FORMATS,
			},
	.capture = {
			.stream_name = "ASI Capture",
			.channels_min = 1,
			.channels_max = 4,
			.rates = TAC5X1X_RATES,
			.formats = TAC5X1X_FORMATS,
			},
	.ops = &tac5x1x_ops,
	.symmetric_rate = 1,
};

static s32 tac5x1x_add_ip_diag_controls(struct snd_soc_component *component)
{
	struct tac5x1x_priv *tac5x1x_priv =
	    snd_soc_component_get_drvdata(component);
	struct tac5x1x *tac5x1x = tac5x1x_priv->tac5x1x;
	struct tac5x1x_input_diag_config *input_diag_config =
	    &tac5x1x_priv->tac5x1x->input_diag_config;
	s32 ret = 0;

	switch (tac5x1x->codec_type) {
	case TAA5212:
		break;
	case TAA5412:
	case TAC5301:
	case TAC5311:
	case TAC5312:
	case TAC5411:
	case TAC5412:
		if (input_diag_config->in_ch_en) {
			ret = snd_soc_add_component_controls(component,
							     taa_ip_controls,
							     ARRAY_SIZE(taa_ip_controls));
			if (ret)
				return ret;
			snd_soc_component_update_bits(component,
						      TAC5X1X_DIAG_CFG0,
						      TAC5X1X_IN_CH_DIAG_EN_MASK,
						      TAC5X1X_IN_CH_DIAG_EN_MASK);
		}
		if (input_diag_config->out_ch_en)
			snd_soc_component_update_bits(component, TAC5X1X_DIAG_CFG0,
						      TAC5X1X_OUT1P_DIAG_EN_MASK,
						      TAC5X1X_OUT1P_DIAG_EN_MASK);

		if (input_diag_config->incl_se_inm)
			snd_soc_component_update_bits(component, TAC5X1X_DIAG_CFG0,
						      TAC5X1X_INCL_SE_INM_MASK,
						      TAC5X1X_INCL_SE_INM_MASK);
		if (input_diag_config->incl_ac_coup)
			snd_soc_component_update_bits(component, TAC5X1X_DIAG_CFG0,
						      TAC5X1X_INCL_AC_COUP_MASK,
						      TAC5X1X_INCL_AC_COUP_MASK);

		if (tac5x1x->micbias_thr[0] > 0 && tac5x1x->micbias_thr[1] > 0) {
			snd_soc_component_update_bits(component, TAC5X1X_DIAG_CFG7,
						      0xff,
						      tac5x1x->micbias_thr[0]);
			snd_soc_component_update_bits(component, TAC5X1X_DIAG_CFG6,
						      0xff,
						      tac5x1x->micbias_thr[1]);
		}

		ret = tac5x1x_register_interrupt(tac5x1x_priv);
		if (ret < 0)
			dev_warn(tac5x1x->dev,
				 "ignore IRQ registration failure\n");

		fallthrough;
	case TAC5111:
	case TAC5112:
	case TAC5211:
	case TAC5212:
	case TAD5112:
	case TAD5212:
		snd_soc_component_update_bits(component, TAC5X1X_DIAG_CFG9,
					      0xff, tac5x1x->gpa_threshold[0]);
		snd_soc_component_update_bits(component, TAC5X1X_DIAG_CFG8,
					      0xff, tac5x1x->gpa_threshold[1]);
		break;
	default:
		break;
	}

	return ret;
};

static s32 tac5x1x_add_controls(struct snd_soc_component *component)
{
	struct tac5x1x_priv *tac5x1x =
		snd_soc_component_get_drvdata(component);
	s32 ret;

	switch (tac5x1x->tac5x1x->codec_type) {
	case TAA5212:
		fallthrough;
	case TAA5412:
		ret =
		    snd_soc_add_component_controls(component, tolerance_ctrls,
						   ARRAY_SIZE(tolerance_ctrls));
		if (ret)
			return ret;
		ret =
		    snd_soc_add_component_controls(component,
						   taa_ip_controls,
						   ARRAY_SIZE(taa_ip_controls));
		if (ret)
			return ret;
		break;
	case TAC5111:
	case TAC5211:
		ret =
		    snd_soc_add_component_controls(component,
						   tac5x11_controls,
						   ARRAY_SIZE(tac5x11_controls));
		if (ret)
			return ret;
		fallthrough;
	case TAC5301:
	case TAC5311:
	case TAC5411:
		ret =
		    snd_soc_add_component_controls(component, tad5x1x_controls,
						   ARRAY_SIZE(tad5x1x_controls));
		if (ret)
			return ret;
		break;
	case TAC5112:
	case TAC5212:
		fallthrough;
	case TAC5312:
	case TAC5412:
		ret =
		    snd_soc_add_component_controls(component, tolerance_ctrls,
						   ARRAY_SIZE(tolerance_ctrls));
		if (ret)
			return ret;
		ret =
		    snd_soc_add_component_controls(component, tad5x1x_controls,
						   ARRAY_SIZE(tad5x1x_controls));
		if (ret)
			return ret;

		/* Only add stereo ADC controls for stereo devices
		 * Mono devices should not have CH2 ADC controls
		 */
		if (is_stereo_device(tac5x1x->tac5x1x)) {
			ret =
			    snd_soc_add_component_controls(component, taa5x12_controls,
							   ARRAY_SIZE(taa5x12_controls));
			if (ret)
				return ret;
		}

		ret =
		    snd_soc_add_component_controls(component, tad5x12_controls,
						   ARRAY_SIZE(tad5x12_controls));
		if (ret)
			return ret;
		break;
	case TAD5212:
	case TAD5112:
		ret = snd_soc_add_component_controls(component, tad5x12_controls,
						     ARRAY_SIZE(tad5x12_controls));
		if (ret)
			return ret;
		break;
	default:
		break;
	}

	/* Always add PDM controls - Source Mux controls handle routing */
	ret = snd_soc_add_component_controls(component,
					     tac5x1x_pdm_controls,
					     ARRAY_SIZE(tac5x1x_pdm_controls));
	if (ret)
		return ret;

	ret = snd_soc_add_component_controls(component, tac5x1x_tdm_slot_controls,
					     ARRAY_SIZE(tac5x1x_tdm_slot_controls));
	if (ret)
		return ret;

	return 0;
}

static s32 tac5x1x_add_widgets(struct snd_soc_component *component)
{
	struct tac5x1x_priv *tac5x1x_priv =
	    snd_soc_component_get_drvdata(component);
	struct tac5x1x *tac5x1x = tac5x1x_priv->tac5x1x;
	struct snd_soc_dapm_context *dapm =
	    snd_soc_component_to_dapm(component);
	s32 ret;

	switch (tac5x1x->codec_type) {
	case TAC5111:
	case TAC5211:
	case TAC5301:
	case TAC5311:
	case TAC5411:
		ret =
		    snd_soc_dapm_new_controls(dapm, tad5x1x_dapm_widgets,
					      ARRAY_SIZE(tad5x1x_dapm_widgets));
		if (ret)
			return ret;
		ret = snd_soc_dapm_add_routes(dapm, tad5x1x_dapm_routes,
					      ARRAY_SIZE(tad5x1x_dapm_routes));
		if (ret)
			return ret;
		break;
	case TAC5112:
	case TAC5212:
	case TAC5312:
	case TAC5412:
		ret =
		    snd_soc_dapm_new_controls(dapm, tad5x1x_dapm_widgets,
					      ARRAY_SIZE(tad5x1x_dapm_widgets));
		if (ret)
			return ret;
		ret = snd_soc_dapm_add_routes(dapm, tad5x1x_dapm_routes,
					      ARRAY_SIZE(tad5x1x_dapm_routes));
		if (ret)
			return ret;
		ret =
		    snd_soc_dapm_new_controls(dapm, tad5x12_dapm_widgets,
					      ARRAY_SIZE(tad5x12_dapm_widgets));
		if (ret)
			return ret;
		ret = snd_soc_dapm_add_routes(dapm, tad5x12_dapm_routes,
					      ARRAY_SIZE(tad5x12_dapm_routes));
		if (ret)
			return ret;
		fallthrough;
	case TAA5212:
	case TAA5412:
		/* Add stereo ADC widgets/routes for stereo devices */
		if (is_stereo_device(tac5x1x)) {
			ret =
			    snd_soc_dapm_new_controls(dapm, taa5x12_dapm_widgets,
						      ARRAY_SIZE(taa5x12_dapm_widgets));
			if (ret)
				return ret;
			ret = snd_soc_dapm_add_routes(dapm, taa5x12_dapm_routes,
						      ARRAY_SIZE(taa5x12_dapm_routes));
			if (ret)
				return ret;
		}
		break;
	case TAD5212:
	case TAD5112:
		ret =
		    snd_soc_dapm_new_controls(dapm, tad5xx_dapm_widgets,
					      ARRAY_SIZE(tad5xx_dapm_widgets));
		if (ret)
			return ret;

		ret =
		    snd_soc_dapm_new_controls(dapm, tad5x12_dapm_widgets,
					      ARRAY_SIZE(tad5x12_dapm_widgets));
		if (ret)
			return ret;
		ret = snd_soc_dapm_add_routes(dapm, tad5x12_dapm_routes,
					      ARRAY_SIZE(tad5x12_dapm_routes));
		if (ret)
			return ret;

		break;
	default:
		break;
	}

	switch (tac5x1x->codec_type) {
	case TAD5212:
	case TAD5112:
		ret =
		    snd_soc_dapm_new_controls(dapm, tad_common_widgets,
					      ARRAY_SIZE(tad_common_widgets));
		if (ret)
			return ret;
		/* TAD devices only have PDM capture, use PDM-only routes */
		ret = snd_soc_dapm_add_routes(dapm, tad_common_routes,
					      ARRAY_SIZE(tad_common_routes));
		if (ret)
			return ret;
		break;
	default:
		ret =
		    snd_soc_dapm_new_controls(dapm, tac5x1x_common_widgets,
					      ARRAY_SIZE(tac5x1x_common_widgets));
		if (ret)
			return ret;

		ret = snd_soc_dapm_add_routes(dapm, tac_common_routes,
					      ARRAY_SIZE(tac_common_routes));
		if (ret)
			return ret;
		break;
	}

	/* Always add PDM widgets - Source Mux controls handle routing */
	ret = snd_soc_dapm_new_controls(dapm, tac5x1x_pdm_widgets,
					ARRAY_SIZE(tac5x1x_pdm_widgets));
	if (ret)
		return ret;

	ret = snd_soc_dapm_add_routes(dapm, tac5x1x_pdm_routes,
				      ARRAY_SIZE(tac5x1x_pdm_routes));
	if (ret)
		return ret;

	return 0;
}

static int tac5x1x_setup_adc_impedance(struct device *dev,
				       struct tac5x1x_priv *tac5x1x_priv)
{
	struct tac5x1x *tac5x1x = tac5x1x_priv->tac5x1x;

	if (tac5x1x->adc_impedance[0] != -1)
		snd_soc_component_update_bits(tac5x1x_priv->component,
					      TAC5X1X_ADCCH1C0,
					      TAC5X1X_ADCCH1C0_IMPEDANCE_MASK,
					      tac5x1x->adc_impedance[0] << 4);

	if (tac5x1x->adc_impedance[1] != -1)
		snd_soc_component_update_bits(tac5x1x_priv->component,
					      TAC5X1X_ADCCH2C0,
					      TAC5X1X_ADCCH2C0_IMPEDANCE_MASK,
					      tac5x1x->adc_impedance[1] << 4);

	return 0;
}

static s32 tac5x1x_component_probe(struct snd_soc_component *component)
{
	s32 ret;
	struct tac5x1x_priv *tac5x1x_priv =
				snd_soc_component_get_drvdata(component);
	struct tac5x1x *tac5x1x = dev_get_drvdata(component->dev->parent);
	struct device *dev = tac5x1x->dev;

	snd_soc_component_init_regmap(component, tac5x1x->regmap);
	tac5x1x_priv->component = component;

	/* Initialize power sequence handling */
	mutex_init(&tac5x1x_priv->ch_lock);
	tac5x1x_priv->pwr_up_done = false;
	INIT_DELAYED_WORK(&tac5x1x_priv->powerup_work, post_powerup_work);

	ret = tac5x1x_add_controls(component);
	if (ret < 0) {
		dev_err(dev, "create control failed\n");
		return ret;
	}

	ret = tac5x1x_add_widgets(component);
	if (ret < 0) {
		dev_err(dev, "device widget addition failed\n");
		return ret;
	}

	tac5x1x_setup_adc_impedance(dev, tac5x1x_priv);

	ret = tac5x1x_add_ip_diag_controls(component);
	if (ret < 0)
		dev_warn(dev, "add diag control failed\n");

	return 0;
}

static int tac5x1x_soc_suspend(struct snd_soc_component *component)
{
	struct tac5x1x_priv *tac5x1x = snd_soc_component_get_drvdata(component);

	/* Cancel any pending power management work before suspend */
	cancel_delayed_work_sync(&tac5x1x->powerup_work);

	return 0;
}

static int tac5x1x_soc_resume(struct snd_soc_component *component)
{
	return 0;
}

static const struct snd_soc_component_driver component_tac5x1x = {
	.probe = tac5x1x_component_probe,
	.set_bias_level = tac5x1x_set_bias_level,
	.suspend = tac5x1x_soc_suspend,
	.resume = tac5x1x_soc_resume,
	.controls = taa5x1x_controls,
	.num_controls = ARRAY_SIZE(taa5x1x_controls),
	.dapm_widgets = taa5x1x_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(taa5x1x_dapm_widgets),
	.dapm_routes = taa5x1x_dapm_routes,
	.num_dapm_routes = ARRAY_SIZE(taa5x1x_dapm_routes),
	.suspend_bias_off = 1,
	.idle_bias_on = 0,
	.use_pmdown_time = 1,
	.endianness = 1,
};

static const struct snd_soc_component_driver component_taa5x1x = {
	.probe = tac5x1x_component_probe,
	.set_bias_level = tac5x1x_set_bias_level,
	.suspend = tac5x1x_soc_suspend,
	.resume = tac5x1x_soc_resume,
	.controls = taa5x1x_controls,
	.num_controls = ARRAY_SIZE(taa5x1x_controls),
	.dapm_widgets = taa5x1x_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(taa5x1x_dapm_widgets),
	.dapm_routes = taa5x1x_dapm_routes,
	.num_dapm_routes = ARRAY_SIZE(taa5x1x_dapm_routes),
	.suspend_bias_off = 1,
	.idle_bias_on = 0,
	.use_pmdown_time = 1,
	.endianness = 1,
};

static const struct snd_soc_component_driver component_tad5x1x = {
	.probe = tac5x1x_component_probe,
	.set_bias_level = tac5x1x_set_bias_level,
	.suspend = tac5x1x_soc_suspend,
	.resume = tac5x1x_soc_resume,
	.controls = tad5x1x_controls,
	.num_controls = ARRAY_SIZE(tad5x1x_controls),
	.dapm_widgets = tad5x1x_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(tad5x1x_dapm_widgets),
	.dapm_routes = tad5x1x_dapm_routes,
	.num_dapm_routes = ARRAY_SIZE(tad5x1x_dapm_routes),
	.suspend_bias_off = 1,
	.idle_bias_on = 0,
	.use_pmdown_time = 1,
	.endianness = 1,
};

static int tac5x1x_probe(struct platform_device *pdev)
{
	s32 ret;
	struct tac5x1x_priv *tac5x1x_priv;
	struct device *dev = &pdev->dev;
	struct i2c_client *i2c = to_i2c_client(dev->parent);
	struct snd_soc_dai_driver *dai;

	tac5x1x_priv = devm_kzalloc(&pdev->dev, sizeof(struct tac5x1x_priv),
				    GFP_KERNEL);
	if (!tac5x1x_priv)
		return -ENOMEM;

	platform_set_drvdata(pdev, tac5x1x_priv);
	tac5x1x_priv->tac5x1x = dev_get_drvdata(pdev->dev.parent);

	/* Allocate unique DAI for each codec instance to avoid naming conflicts */
	dai = devm_kzalloc(dev, sizeof(*dai), GFP_KERNEL);
	if (!dai)
		return -ENOMEM;

	switch (tac5x1x_priv->tac5x1x->codec_type) {
	case TAA5212:
	case TAA5412:
		/* Copy from TAA template */
		memcpy(dai, &taa5x1x_dai, sizeof(*dai));
		dai->name = devm_kasprintf(dev, GFP_KERNEL, "taa5x1x-hifi-%d-%02x",
					   i2c->adapter->nr, i2c->addr);
		if (!dai->name)
			return -ENOMEM;
		ret = devm_snd_soc_register_component(dev, &component_taa5x1x,
						      dai, 1);
		break;
	case TAC5111:
	case TAC5112:
	case TAC5211:
	case TAC5212:
	case TAC5301:
	case TAC5311:
	case TAC5312:
	case TAC5411:
	case TAC5412:
		/* Copy from TAC template */
		memcpy(dai, &tac5x1x_dai, sizeof(*dai));
		dai->name = devm_kasprintf(dev, GFP_KERNEL, "tac5x1x-hifi-%d-%02x",
					   i2c->adapter->nr,
					   i2c->addr);
		if (!dai->name)
			return -ENOMEM;
		ret = devm_snd_soc_register_component(dev, &component_tac5x1x,
						      dai, 1);
		break;
	case TAD5112:
	case TAD5212:
		/* Copy from TAD template */
		memcpy(dai, &tad5x1x_dai, sizeof(*dai));
		dai->name = devm_kasprintf(dev, GFP_KERNEL, "tad5x1x-hifi-%d-%02x",
					   i2c->adapter->nr, i2c->addr);
		if (!dai->name)
			return -ENOMEM;
		ret = devm_snd_soc_register_component(dev, &component_tad5x1x,
						      dai, 1);
		break;
	default:
		ret = -EINVAL;
	}

	if (ret)
		dev_err(dev, "Failed to register component\n");

	return ret;
}

static void tac5x1x_remove(struct platform_device *pdev)
{
	struct tac5x1x_priv *tac5x1x_priv = platform_get_drvdata(pdev);

	/* Cancel any pending power management work before removal */
	cancel_delayed_work_sync(&tac5x1x_priv->powerup_work);
}

static struct platform_driver tac5x1x_codec_driver = {
	.driver = {
		.name = "tac5x1x-codec",
	},
	.probe = tac5x1x_probe,
	.remove = tac5x1x_remove,
};
module_platform_driver(tac5x1x_codec_driver);

MODULE_DESCRIPTION("ASoC tac5x1x driver");
MODULE_AUTHOR("Niranjan H Y <niranjan.hy@ti.com>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:tac5x1x-codec");
MODULE_SOFTDEP("pre: tac5x1x-pinctrl");
