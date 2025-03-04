/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2021 MediaTek Inc.
 * Author: Argus Lin <argus.lin@mediatek.com>
 */

#ifndef _ACCDET_H_
#define _ACCDET_H_

#include <linux/ctype.h>
#include <linux/string.h>

#define ACCDET_DEVNAME "accdet"

#define MT6359_ACCDET_NUM_BUTTONS 4
#define MT6359_ACCDET_JACK_MASK (SND_JACK_HEADPHONE | \
				SND_JACK_HEADSET | \
				SND_JACK_BTN_0 | \
				SND_JACK_BTN_1 | \
				SND_JACK_BTN_2 | \
				SND_JACK_BTN_3)
#define MT6359_ACCDET_BTN_MASK (SND_JACK_BTN_0 | \
				SND_JACK_BTN_1 | \
				SND_JACK_BTN_2 | \
				SND_JACK_BTN_3)

enum eint_moisture_status {
	M_PLUG_IN =		0,
	M_WATER_IN =		1,
	M_HP_PLUG_IN =		2,
	M_PLUG_OUT =		3,
	M_NO_ACT =		4,
	M_UNKNOWN =		5,
};

enum {
	accdet_state000 = 0,
	accdet_state001,
	accdet_state010,
	accdet_state011,
	accdet_auxadc,
	eint_state000,
	eint_state001,
	eint_state010,
	eint_state011,
	eint_inverter_state000,
};

struct dts_data {
	bool hp_eint_high;
};

struct mt6359_accdet {
	struct snd_soc_jack *jack;
	struct device *dev;
	struct regmap *regmap;
	struct dts_data *data;
	unsigned int caps;
	int accdet_irq;
	int accdet_eint0;
	struct mutex res_lock; /* lock protection */
	bool jack_plugged;
	unsigned int jack_type;
	unsigned int accdet_status;
	unsigned int pre_accdet_status;
	unsigned int jd_sts;
	struct work_struct accdet_work;
	struct workqueue_struct *accdet_workqueue;
	struct work_struct jd_work;
	struct workqueue_struct *jd_workqueue;
};

int mt6359_accdet_enable_jack_detect(struct snd_soc_component *component,
				     struct snd_soc_jack *jack);
#endif
