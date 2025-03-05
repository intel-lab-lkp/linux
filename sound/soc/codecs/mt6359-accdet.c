// SPDX-License-Identifier: GPL-2.0
//
// mt6359-accdet.c  --  ALSA SoC mt6359 accdet driver
//
// Copyright (C) 2021 MediaTek Inc.
// Author: Argus Lin <argus.lin@mediatek.com>
//

#include <linux/of.h>
#include <linux/input.h>
#include <linux/kthread.h>
#include <linux/io.h>
#include <linux/sched/clock.h>
#include <linux/workqueue.h>
#include <linux/timer.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/init.h>
#include <linux/irqdomain.h>
#include <linux/irq.h>
#include <linux/regmap.h>
#include <sound/soc.h>
#include <sound/jack.h>
#include <linux/mfd/mt6397/core.h>

#include "mt6359-accdet.h"
#include "mt6359.h"

/* global variable definitions */
#define REGISTER_VAL(x)	((x) - 1)

/* mt6359 accdet capability */
#define ACCDET_PMIC_RSV_EINT		BIT(7)

#define ACCDET_RSV_KEY			BIT(11)

#define ACCDET_ANALOG_FASTDISCHARGE	BIT(12)
#define ACCDET_DIGITAL_FASTDISCHARGE	BIT(13)
#define ACCDET_AD_FASTDISCHRAGE		BIT(14)

/* Debounce time = (DEBOUNCE/32768) sec */
#define ACCDET_DEBOUNCE0_625MS 0x800
#define ACCDET_DEBOUNCE1_625MS 0x800
#define ACCDET_DEBOUNCE3_976US 0x20
#define ACCDET_DEBOUNCE_AUXADC_2MS 0x44
/* PWM frequency = 32768/(PWM_WIDTH + 1) Hz */
#define ACCDET_PWM_WIDTH_25_580HZ 0x500
/* Duty cycle = (PWM_THRESH + 1) / (PWM_WIDTH + 1) */
#define ACCDET_PWM_THRESH_100PERCENT ACCDET_PWM_WIDTH_25_580HZ
#define ACCDET_RISE_DELAY 0x1f0
#define ACCDET_FALL_DELAY 1
#define ACCDET_EINT_DEBOUNCE0_1MS 5
#define ACCDET_EINT_DEBOUNCE1_900US 3
#define ACCDET_EINT_DEBOUNCE2_900US 3
#define ACCDET_EINT_DEBOUNCE3_1MS 5
#define ACCDET_EINT_INVERTER_DEBOUNCE_256MS 0xe
#define ACCDET_EINT_CMPMEN_PWM_WIDTH_400MS 4
#define ACCDET_EINT_CMPMEN_PWM_THRESH_2MS 1
#define ACCDET_EINT_COMP_VTH_1600MV 2

static struct platform_driver mt6359_accdet_driver;
static const struct snd_soc_component_driver mt6359_accdet_soc_driver;

/* local function declaration */
static void accdet_set_debounce(struct mt6359_accdet *priv, int state,
				unsigned int debounce);
static unsigned int adjust_eint_analog_setting(struct mt6359_accdet *priv);
static void config_digital_init_by_mode(struct mt6359_accdet *priv);
static void config_eint_init_by_mode(struct mt6359_accdet *priv);
static inline void mt6359_accdet_init(struct mt6359_accdet *priv);
static unsigned int mt6359_accdet_jd_setting(struct mt6359_accdet *priv);
static void mt6359_accdet_recover_jd_setting(struct mt6359_accdet *priv);
static void mt6359_accdet_jack_report(struct mt6359_accdet *priv);
static void recover_eint_analog_setting(struct mt6359_accdet *priv);
static void recover_eint_digital_setting(struct mt6359_accdet *priv);
static void recover_eint_setting(struct mt6359_accdet *priv);

static unsigned int adjust_eint_analog_setting(struct mt6359_accdet *priv)
{
	/* ESD switches off */
	regmap_update_bits(priv->regmap, RG_ACCDETSPARE_ADDR, 1 << 8, 0);
	/* enable RG_EINT0CONFIGACCDET */
	regmap_update_bits(priv->regmap, RG_EINT0CONFIGACCDET_ADDR,
			   RG_EINT0CONFIGACCDET_MASK_SFT,
			   BIT(RG_EINT0CONFIGACCDET_SFT));
	/*select 500k, use internal resistor */
	regmap_update_bits(priv->regmap, RG_EINT0HIRENB_ADDR,
			   RG_EINT0HIRENB_MASK_SFT, BIT(RG_EINT0HIRENB_SFT));
	return 0;
}

static unsigned int adjust_eint_digital_setting(struct mt6359_accdet *priv)
{
	/* disable inverter */
	regmap_update_bits(priv->regmap, ACCDET_EINT0_INVERTER_SW_EN_ADDR,
			   ACCDET_EINT0_INVERTER_SW_EN_MASK_SFT, 0);

	/* set DA stable signal */
	regmap_update_bits(priv->regmap, ACCDET_DA_STABLE_ADDR,
			   ACCDET_EINT0_CEN_STABLE_MASK_SFT, 0);
	return 0;
}

static unsigned int mt6359_accdet_jd_setting(struct mt6359_accdet *priv)
{
	if (priv->jd_sts == M_PLUG_IN) {
		/* adjust digital setting */
		adjust_eint_digital_setting(priv);
		/* adjust analog setting */
		adjust_eint_analog_setting(priv);
	} else if (priv->jd_sts == M_PLUG_OUT) {
		/* set debounce to 1ms */
		accdet_set_debounce(priv, eint_state000,
				    ACCDET_EINT_DEBOUNCE0_1MS);
	} else {
		dev_dbg(priv->dev, "should not be here %s()\n", __func__);
	}

	return 0;
}

static void recover_eint_analog_setting(struct mt6359_accdet *priv)
{
	/* ESD switches on */
	regmap_update_bits(priv->regmap, RG_ACCDETSPARE_ADDR, 1 << 8, 1 << 8);
	/* disable RG_EINT0CONFIGACCDET */
	regmap_update_bits(priv->regmap, RG_EINT0CONFIGACCDET_ADDR,
			   RG_EINT0CONFIGACCDET_MASK_SFT, 0);
	regmap_update_bits(priv->regmap, RG_EINT0HIRENB_ADDR,
			   RG_EINT0HIRENB_MASK_SFT, 0);
}

static void recover_eint_digital_setting(struct mt6359_accdet *priv)
{
	regmap_update_bits(priv->regmap, ACCDET_EINT0_M_SW_EN_ADDR,
			   ACCDET_EINT0_M_SW_EN_MASK_SFT, 0);
	/* enable eint0cen */
	regmap_update_bits(priv->regmap, ACCDET_DA_STABLE_ADDR,
			   ACCDET_EINT0_CEN_STABLE_MASK_SFT,
			   BIT(ACCDET_EINT0_CEN_STABLE_SFT));

	/* enable inverter */
	regmap_update_bits(priv->regmap, ACCDET_EINT0_INVERTER_SW_EN_ADDR,
			   ACCDET_EINT0_INVERTER_SW_EN_MASK_SFT,
			   BIT(ACCDET_EINT0_INVERTER_SW_EN_SFT));
}

static void recover_eint_setting(struct mt6359_accdet *priv)
{
	if (priv->jd_sts == M_PLUG_OUT) {
		recover_eint_analog_setting(priv);
		recover_eint_digital_setting(priv);
	}
}

static void mt6359_accdet_recover_jd_setting(struct mt6359_accdet *priv)
{
	int ret;
	unsigned int value = 0;

	regmap_update_bits(priv->regmap, ACCDET_IRQ_ADDR,
			   ACCDET_IRQ_CLR_MASK_SFT, BIT(ACCDET_IRQ_CLR_SFT));
	usleep_range(200, 300);
	ret = regmap_read_poll_timeout(priv->regmap,
				       ACCDET_IRQ_ADDR,
				       value,
				       (value & ACCDET_IRQ_MASK_SFT) == 0,
				       0,
				       1000);
	if (ret)
		dev_warn(priv->dev, "%s(), ret %d\n", __func__, ret);
	/* clear accdet int, modify  for fix interrupt trigger twice error */
	regmap_update_bits(priv->regmap, ACCDET_IRQ_ADDR,
			   ACCDET_IRQ_CLR_MASK_SFT, 0);
	regmap_update_bits(priv->regmap, RG_INT_STATUS_ACCDET_ADDR,
			   RG_INT_STATUS_ACCDET_MASK_SFT,
			   BIT(RG_INT_STATUS_ACCDET_SFT));

	/* recover accdet debounce0,3 */
	accdet_set_debounce(priv, accdet_state000,
			    ACCDET_DEBOUNCE0_625MS);
	accdet_set_debounce(priv, accdet_state001,
			    ACCDET_DEBOUNCE1_625MS);
	accdet_set_debounce(priv, accdet_state011,
			    ACCDET_DEBOUNCE3_976US);

	priv->jack_type = 0;
	priv->accdet_status = 0x3;
	mt6359_accdet_jack_report(priv);
}

static void accdet_set_debounce(struct mt6359_accdet *priv, int state,
				unsigned int debounce)
{
	switch (state) {
	case accdet_state000:
		regmap_write(priv->regmap, ACCDET_DEBOUNCE0_ADDR, debounce);
		break;
	case accdet_state001:
		regmap_write(priv->regmap, ACCDET_DEBOUNCE1_ADDR, debounce);
		break;
	case accdet_state010:
		regmap_write(priv->regmap, ACCDET_DEBOUNCE2_ADDR, debounce);
		break;
	case accdet_state011:
		regmap_write(priv->regmap, ACCDET_DEBOUNCE3_ADDR, debounce);
		break;
	case accdet_auxadc:
		regmap_write(priv->regmap,
			     ACCDET_CONNECT_AUXADC_TIME_DIG_ADDR, debounce);
		break;
	case eint_state000:
		regmap_update_bits(priv->regmap, ACCDET_EINT_DEBOUNCE0_ADDR,
				   0xF << ACCDET_EINT_DEBOUNCE0_SFT,
				   debounce << ACCDET_EINT_DEBOUNCE0_SFT);
		break;
	case eint_state001:
		regmap_update_bits(priv->regmap, ACCDET_EINT_DEBOUNCE1_ADDR,
				   0xF << ACCDET_EINT_DEBOUNCE1_SFT,
				   debounce << ACCDET_EINT_DEBOUNCE1_SFT);
		break;
	case eint_state010:
		regmap_update_bits(priv->regmap, ACCDET_EINT_DEBOUNCE2_ADDR,
				   0xF << ACCDET_EINT_DEBOUNCE2_SFT,
				   debounce << ACCDET_EINT_DEBOUNCE2_SFT);
		break;
	case eint_state011:
		regmap_update_bits(priv->regmap, ACCDET_EINT_DEBOUNCE3_ADDR,
				   0xF << ACCDET_EINT_DEBOUNCE3_SFT,
				   debounce << ACCDET_EINT_DEBOUNCE3_SFT);
		break;
	case eint_inverter_state000:
		regmap_write(priv->regmap, ACCDET_EINT_INVERTER_DEBOUNCE_ADDR,
			     debounce);
		break;
	default:
		dev_warn(priv->dev, "Error: %s error state (%d)\n", __func__,
			 state);
		break;
	}
}

static void mt6359_accdet_jack_report(struct mt6359_accdet *priv)
{
	int report = 0;

	if (!priv->jack)
		return;

	report = priv->jack_type;
	snd_soc_jack_report(priv->jack, report, MT6359_ACCDET_JACK_MASK);
}

static inline void check_jack_btn_type(struct mt6359_accdet *priv)
{
	unsigned int val = 0;

	regmap_read(priv->regmap, ACCDET_MEM_IN_ADDR, &val);

	priv->accdet_status =
		(val >> ACCDET_STATE_MEM_IN_OFFSET) & ACCDET_STATE_AB_MASK;

	switch (priv->accdet_status) {
	case 0:
		if (priv->jack_type != SND_JACK_HEADSET)
			priv->jack_type = SND_JACK_HEADPHONE;
		break;
	case 1:
		if (priv->jack_type != SND_JACK_HEADSET) {
			priv->jack_type = SND_JACK_HEADSET;
			accdet_set_debounce(priv, eint_state011, 0x1);
		}
		break;
	case 3:
	default:
		priv->jack_type = 0;
		break;
	}
}

static void mt6359_accdet_work(struct work_struct *work)
{
	struct mt6359_accdet *priv =
		container_of(work, struct mt6359_accdet, accdet_work);

	mutex_lock(&priv->res_lock);
	priv->pre_accdet_status = priv->accdet_status;
	check_jack_btn_type(priv);

	if (priv->jack_plugged &&
	    priv->pre_accdet_status != priv->accdet_status)
		mt6359_accdet_jack_report(priv);
	mutex_unlock(&priv->res_lock);
}

static void mt6359_accdet_jd_work(struct work_struct *work)
{
	int ret;
	unsigned int value = 0;

	struct mt6359_accdet *priv =
		container_of(work, struct mt6359_accdet, jd_work);

	mutex_lock(&priv->res_lock);
	if (priv->jd_sts == M_PLUG_IN) {
		priv->jack_plugged = true;

		/* set and clear initial bit every eint interrupt */
		regmap_update_bits(priv->regmap, ACCDET_SEQ_INIT_ADDR,
				   ACCDET_SEQ_INIT_MASK_SFT,
				   BIT(ACCDET_SEQ_INIT_SFT));
		regmap_update_bits(priv->regmap, ACCDET_SEQ_INIT_ADDR,
				   ACCDET_SEQ_INIT_MASK_SFT, 0);
		ret = regmap_read_poll_timeout(priv->regmap,
					       ACCDET_SEQ_INIT_ADDR,
					       value,
					       (value & ACCDET_SEQ_INIT_MASK_SFT) == 0,
					       0,
					       1000);
		if (ret)
			dev_err(priv->dev, "%s(), ret %d\n", __func__, ret);

		/* enable ACCDET unit */
		regmap_update_bits(priv->regmap, ACCDET_SW_EN_ADDR,
				   ACCDET_SW_EN_MASK_SFT, BIT(ACCDET_SW_EN_SFT));
	} else if (priv->jd_sts == M_PLUG_OUT) {
		priv->jack_plugged = false;

		accdet_set_debounce(priv, accdet_state011,
				    ACCDET_DEBOUNCE3_976US);
		regmap_update_bits(priv->regmap, ACCDET_SW_EN_ADDR,
				   ACCDET_SW_EN_MASK_SFT, 0);
		mt6359_accdet_recover_jd_setting(priv);
	}

	recover_eint_setting(priv);
	mutex_unlock(&priv->res_lock);
}

static irqreturn_t mt6359_accdet_irq(int irq, void *data)
{
	struct mt6359_accdet *priv = data;
	unsigned int irq_val = 0, val = 0, value = 0;
	int ret;

	mutex_lock(&priv->res_lock);
	regmap_read(priv->regmap, ACCDET_IRQ_ADDR, &irq_val);

	if (irq_val & ACCDET_IRQ_MASK_SFT) {
		regmap_update_bits(priv->regmap, ACCDET_IRQ_ADDR,
				   ACCDET_IRQ_CLR_MASK_SFT,
				   BIT(ACCDET_IRQ_CLR_SFT));
		ret = regmap_read_poll_timeout(priv->regmap,
					       ACCDET_IRQ_ADDR,
					       value,
					       (value & ACCDET_IRQ_MASK_SFT) == 0,
					       0,
					       1000);
		if (ret) {
			dev_err(priv->dev, "%s(), ret %d\n", __func__, ret);
			mutex_unlock(&priv->res_lock);
			return IRQ_NONE;
		}
		regmap_update_bits(priv->regmap, ACCDET_IRQ_ADDR,
				   ACCDET_IRQ_CLR_MASK_SFT, 0);
		regmap_update_bits(priv->regmap, RG_INT_STATUS_ACCDET_ADDR,
				   RG_INT_STATUS_ACCDET_MASK_SFT,
				   BIT(RG_INT_STATUS_ACCDET_SFT));

		queue_work(priv->accdet_workqueue, &priv->accdet_work);
	} else {
		if (irq_val & ACCDET_EINT0_IRQ_MASK_SFT) {
			regmap_update_bits(priv->regmap, ACCDET_IRQ_ADDR,
					   ACCDET_EINT0_IRQ_CLR_MASK_SFT,
					   BIT(ACCDET_EINT0_IRQ_CLR_SFT));
			ret = regmap_read_poll_timeout(priv->regmap,
						       ACCDET_IRQ_ADDR,
						       value,
						       (value & ACCDET_EINT0_IRQ_MASK_SFT) == 0,
						       0,
						       1000);
			if (ret) {
				dev_err(priv->dev, "%s(), ret %d\n", __func__,
					ret);
				mutex_unlock(&priv->res_lock);
				return IRQ_NONE;
			}
			regmap_update_bits(priv->regmap, ACCDET_IRQ_ADDR,
					   ACCDET_EINT0_IRQ_CLR_MASK_SFT, 0);
			regmap_update_bits(priv->regmap,
					   RG_INT_STATUS_ACCDET_ADDR,
					   RG_INT_STATUS_ACCDET_EINT0_MASK_SFT,
					   BIT(RG_INT_STATUS_ACCDET_EINT0_SFT));
		}
		if (irq_val & ACCDET_EINT1_IRQ_MASK_SFT) {
			regmap_update_bits(priv->regmap, ACCDET_IRQ_ADDR,
					   ACCDET_EINT1_IRQ_CLR_MASK_SFT,
					   BIT(ACCDET_EINT1_IRQ_CLR_SFT));
			ret = regmap_read_poll_timeout(priv->regmap,
						       ACCDET_IRQ_ADDR,
						       value,
						       (value & ACCDET_EINT1_IRQ_MASK_SFT) == 0,
						       0,
						       1000);
			if (ret) {
				dev_err(priv->dev, "%s(), ret %d\n", __func__,
					ret);
				mutex_unlock(&priv->res_lock);
				return IRQ_NONE;
			}
			regmap_update_bits(priv->regmap, ACCDET_IRQ_ADDR,
					   ACCDET_EINT1_IRQ_CLR_MASK_SFT, 0);
			regmap_update_bits(priv->regmap,
					   RG_INT_STATUS_ACCDET_ADDR,
					   RG_INT_STATUS_ACCDET_EINT1_MASK_SFT,
					   BIT(RG_INT_STATUS_ACCDET_EINT1_SFT));
		}
		/* get jack detection status */
		regmap_read(priv->regmap, ACCDET_EINT0_MEM_IN_ADDR, &val);
		priv->jd_sts = ((val >> ACCDET_EINT0_MEM_IN_SFT) &
				   ACCDET_EINT0_MEM_IN_MASK);
		/* adjust eint digital/analog setting */
		mt6359_accdet_jd_setting(priv);

		queue_work(priv->jd_workqueue, &priv->jd_work);
	}
	mutex_unlock(&priv->res_lock);

	return IRQ_HANDLED;
}

static int mt6359_accdet_parse_dt(struct mt6359_accdet *priv)
{
	struct device *dev = priv->dev;
	struct device_node *node = NULL;

	node = of_get_child_by_name(dev->parent->of_node, "accdet");
	if (!node)
		return -EINVAL;

	priv->data->hp_eint_high = of_property_read_bool(node, "mediatek,hp-eint-high");

	of_node_put(node);
	dev_warn(priv->dev, "accdet caps=%x\n", priv->caps);

	return 0;
}

static void config_digital_init_by_mode(struct mt6359_accdet *priv)
{
	/* enable eint cmpmem pwm */
	regmap_write(priv->regmap, ACCDET_EINT_CMPMEN_PWM_THRESH_ADDR,
		     (ACCDET_EINT_CMPMEN_PWM_WIDTH_400MS << 4 |
		      ACCDET_EINT_CMPMEN_PWM_THRESH_2MS));
	/* DA signal stable */
	regmap_write(priv->regmap, ACCDET_DA_STABLE_ADDR,
		     ACCDET_EINT0_STABLE_VAL);
	/* after receive n+1 number, interrupt issued. */
	regmap_update_bits(priv->regmap, ACCDET_EINT_M_PLUG_IN_NUM_ADDR,
			   ACCDET_EINT_M_PLUG_IN_NUM_MASK_SFT,
			   BIT(ACCDET_EINT_M_PLUG_IN_NUM_SFT));
	/* setting HW mode, enable digital fast discharge
	 * if use EINT0 & EINT1 detection, please modify
	 * ACCDET_HWMODE_EN_ADDR[2:1]
	 */
	regmap_write(priv->regmap, ACCDET_HWMODE_EN_ADDR, 0x100);

	regmap_update_bits(priv->regmap, ACCDET_EINT_M_DETECT_EN_ADDR,
			   ACCDET_EINT_M_DETECT_EN_MASK_SFT, 0);

	/* enable PWM */
	regmap_write(priv->regmap, ACCDET_CMP_PWM_EN_ADDR, 0x67);
	/* enable inverter detection */
	regmap_update_bits(priv->regmap, ACCDET_EINT0_INVERTER_SW_EN_ADDR,
			   ACCDET_EINT0_INVERTER_SW_EN_MASK_SFT,
			   BIT(ACCDET_EINT0_INVERTER_SW_EN_SFT));

	if (priv->data->hp_eint_high) {
		/* EINT polarity inverse */
		regmap_update_bits(priv->regmap, ACCDET_EINT_IN_INVERSE_ADDR,
				   ACCDET_EINT_IN_INVERSE_MASK_SFT,
				   BIT(ACCDET_EINT_IN_INVERSE_SFT));
	} else {
		/* EINT polarity normal */
		regmap_update_bits(priv->regmap, ACCDET_EINT_IN_INVERSE_ADDR,
				   ACCDET_EINT_IN_INVERSE_MASK_SFT, 0);
	}
}

static void config_eint_init_by_mode(struct mt6359_accdet *priv)
{
	unsigned int val = 0;

	regmap_update_bits(priv->regmap, RG_EINT0EN_ADDR, RG_EINT0EN_MASK_SFT,
			   BIT(RG_EINT0EN_SFT));
	/* ESD switches on */
	regmap_update_bits(priv->regmap, RG_ACCDETSPARE_ADDR,
			   1 << 8, 1 << 8);
	/* before playback, set NCP pull low before nagative voltage */
	regmap_update_bits(priv->regmap, RG_NCP_PDDIS_EN_ADDR,
			   RG_NCP_PDDIS_EN_MASK_SFT, BIT(RG_NCP_PDDIS_EN_SFT));

	/* current detect set 0.25uA */
	regmap_update_bits(priv->regmap, RG_ACCDETSPARE_ADDR,
			   0x3 << RG_ACCDETSPARE_SFT,
			   0x3 << RG_ACCDETSPARE_SFT);
	regmap_write(priv->regmap, RG_EINTCOMPVTH_ADDR,
		     val | ACCDET_EINT_COMP_VTH_1600MV << RG_EINTCOMPVTH_SFT);
}

static void mt6359_accdet_init(struct mt6359_accdet *priv)
{
	unsigned int reg = 0;

	regmap_update_bits(priv->regmap, ACCDET_SEQ_INIT_ADDR,
			   ACCDET_SEQ_INIT_MASK_SFT, BIT(ACCDET_SEQ_INIT_SFT));
	mdelay(2);
	regmap_update_bits(priv->regmap, ACCDET_SEQ_INIT_ADDR,
			   ACCDET_SEQ_INIT_MASK_SFT, 0);
	mdelay(1);
	/* init the debounce time (debounce/32768)sec */
	accdet_set_debounce(priv, accdet_state000,
			    ACCDET_DEBOUNCE0_625MS);
	accdet_set_debounce(priv, accdet_state001,
			    ACCDET_DEBOUNCE1_625MS);
	accdet_set_debounce(priv, accdet_state011,
			    ACCDET_DEBOUNCE3_976US);
	accdet_set_debounce(priv, accdet_auxadc,
			    ACCDET_DEBOUNCE_AUXADC_2MS);

	accdet_set_debounce(priv, eint_state000,
			    ACCDET_EINT_DEBOUNCE0_1MS);
	accdet_set_debounce(priv, eint_state001,
			    ACCDET_EINT_DEBOUNCE1_900US);
	accdet_set_debounce(priv, eint_state011,
			    ACCDET_EINT_DEBOUNCE3_1MS);
	accdet_set_debounce(priv, eint_inverter_state000,
			    ACCDET_EINT_INVERTER_DEBOUNCE_256MS);

	regmap_update_bits(priv->regmap, RG_ACCDET_RST_ADDR,
			   RG_ACCDET_RST_MASK_SFT, BIT(RG_ACCDET_RST_SFT));
	regmap_update_bits(priv->regmap, RG_ACCDET_RST_ADDR,
			   RG_ACCDET_RST_MASK_SFT, 0);

	/* clear high micbias1 voltage setting */
	regmap_update_bits(priv->regmap, RG_AUDPWDBMICBIAS1_ADDR,
			   0x3 << RG_AUDMICBIAS1HVEN_SFT, 0);
	regmap_update_bits(priv->regmap, RG_AUDPWDBMICBIAS1_ADDR,
			   0x7 << RG_AUDMICBIAS1VREF_SFT, 0);

	/* init pwm frequency, duty & rise/falling delay */
	regmap_write(priv->regmap, ACCDET_PWM_WIDTH_ADDR,
		     REGISTER_VAL(ACCDET_PWM_WIDTH_25_580HZ));
	regmap_write(priv->regmap, ACCDET_PWM_THRESH_ADDR,
		     REGISTER_VAL(ACCDET_PWM_THRESH_100PERCENT));
	regmap_write(priv->regmap, ACCDET_RISE_DELAY_ADDR,
		     (ACCDET_FALL_DELAY << 15 |
		      ACCDET_RISE_DELAY));

	regmap_read(priv->regmap, RG_AUDPWDBMICBIAS1_ADDR, &reg);
	/* micbias1 = 2.8v */
	regmap_write(priv->regmap, RG_AUDPWDBMICBIAS1_ADDR,
		     reg | (3 << RG_AUDMICBIAS1HVEN_SFT) |
			     RG_AUDMICBIAS1LOWPEN_MASK_SFT);
	/* mic mode setting */
	regmap_read(priv->regmap, RG_AUDACCDETMICBIAS0PULLLOW_ADDR, &reg);
	/* DCC mode Low cost mode without internal bias */
	regmap_write(priv->regmap, RG_AUDACCDETMICBIAS0PULLLOW_ADDR,
		     reg | RG_ACCDET_MODE_ANA11_MODE2);
	/* enable analog fast discharge */
	regmap_update_bits(priv->regmap, RG_ANALOGFDEN_ADDR,
			   0x3 << RG_ANALOGFDEN_SFT, 0x3 << RG_ANALOGFDEN_SFT);

	config_eint_init_by_mode(priv);
	config_digital_init_by_mode(priv);
}

int mt6359_accdet_enable_jack_detect(struct snd_soc_component *component,
				     struct snd_soc_jack *jack)
{
	struct mt6359_accdet *priv =
		snd_soc_component_get_drvdata(component);

	snd_jack_set_key(jack->jack, SND_JACK_BTN_0, KEY_PLAYPAUSE);
	snd_jack_set_key(jack->jack, SND_JACK_BTN_1, KEY_VOLUMEDOWN);
	snd_jack_set_key(jack->jack, SND_JACK_BTN_2, KEY_VOLUMEUP);
	snd_jack_set_key(jack->jack, SND_JACK_BTN_3, KEY_VOICECOMMAND);

	priv->jack = jack;

	mt6359_accdet_jack_report(priv);

	return 0;
}
EXPORT_SYMBOL_GPL(mt6359_accdet_enable_jack_detect);

static int mt6359_accdet_probe(struct platform_device *pdev)
{
	struct mt6359_accdet *priv;
	struct mt6397_chip *mt6397 = dev_get_drvdata(pdev->dev.parent);
	int ret;

	dev_dbg(&pdev->dev, "%s(), dev name %s\n",
		__func__, dev_name(&pdev->dev));

	priv = devm_kzalloc(&pdev->dev, sizeof(struct mt6359_accdet),
			    GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->data = devm_kzalloc(&pdev->dev, sizeof(struct dts_data),
				  GFP_KERNEL);
	if (!priv->data)
		return -ENOMEM;

	priv->regmap = mt6397->regmap;
	if (IS_ERR(priv->regmap)) {
		ret = PTR_ERR(priv->regmap);
		dev_err(&pdev->dev, "Failed to allocate register map: %d\n",
			ret);
		return ret;
	}
	priv->dev = &pdev->dev;

	ret = mt6359_accdet_parse_dt(priv);
	if (ret) {
		dev_err(&pdev->dev, "Failed to parse dts\n");
		return ret;
	}
	mutex_init(&priv->res_lock);

	priv->accdet_irq = platform_get_irq(pdev, 0);
	if (priv->accdet_irq >= 0) {
		ret = devm_request_threaded_irq(&pdev->dev, priv->accdet_irq,
						NULL, mt6359_accdet_irq,
						IRQF_TRIGGER_HIGH | IRQF_ONESHOT,
						"ACCDET_IRQ", priv);
		if (ret) {
			dev_err(&pdev->dev,
				"Failed to request IRQ: (%d)\n", ret);
			return ret;
		}
	}

	priv->accdet_eint0 = platform_get_irq(pdev, 1);
	if (priv->accdet_eint0 >= 0) {
		ret = devm_request_threaded_irq(&pdev->dev,
						priv->accdet_eint0,
						NULL, mt6359_accdet_irq,
						IRQF_TRIGGER_HIGH | IRQF_ONESHOT,
						"ACCDET_EINT0", priv);
		if (ret) {
			dev_err(&pdev->dev,
				"Failed to request eint0 IRQ (%d)\n",
				ret);
			return ret;
		}
	}

	priv->accdet_workqueue = create_singlethread_workqueue("accdet");
	INIT_WORK(&priv->accdet_work, mt6359_accdet_work);
	if (!priv->accdet_workqueue) {
		dev_err(&pdev->dev, "Failed to create accdet workqueue\n");
		ret = -1;
		goto err_accdet_wq;
	}

	priv->jd_workqueue = create_singlethread_workqueue("mt6359_accdet_jd");
	INIT_WORK(&priv->jd_work, mt6359_accdet_jd_work);
	if (!priv->jd_workqueue) {
		dev_err(&pdev->dev, "Failed to create jack detect workqueue\n");
		ret = -1;
		goto err_eint_wq;
	}

	platform_set_drvdata(pdev, priv);
	ret = devm_snd_soc_register_component(&pdev->dev,
					      &mt6359_accdet_soc_driver,
					      NULL, 0);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register component\n");
		return ret;
	}

	priv->jd_sts = M_PLUG_OUT;
	priv->jack_type = 0;
	priv->accdet_status = 0x3;
	mt6359_accdet_init(priv);

	mt6359_accdet_jack_report(priv);

	return 0;

err_eint_wq:
	destroy_workqueue(priv->accdet_workqueue);
err_accdet_wq:
	dev_err(&pdev->dev, "%s error. now exit.!\n", __func__);
	return ret;
}

const struct of_device_id accdet_of_match[] = {
	{ .compatible = "mediatek,mt6359-accdet", },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, accdet_of_match);

static struct platform_driver mt6359_accdet_driver = {
	.driver = {
		.name = "pmic-codec-accdet",
		.of_match_table = accdet_of_match,
	},
	.probe = mt6359_accdet_probe,
};

module_platform_driver(mt6359_accdet_driver)

/* Module information */
MODULE_DESCRIPTION("MT6359 ALSA SoC codec jack driver");
MODULE_AUTHOR("Argus Lin <argus.lin@mediatek.com>");
MODULE_LICENSE("GPL v2");
