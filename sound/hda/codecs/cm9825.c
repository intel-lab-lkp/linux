// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * CM9825 HD-audio codec
 */

#include <linux/init.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <sound/core.h>
#include <sound/hda_codec.h>
#include "hda_local.h"
#include "hda_auto_parser.h"
#include "hda_jack.h"
#include "generic.h"

enum {
	QUIRK_CM_STD = 0x0,
	QUIRK_NCR_SSID = 0x104388f0,
	QUIRK_ASL051_SSID = 0x15bd3274,
	QUIRK_IBP_SSID = 0x15bd3275,
	QUIRK_ARH171_SSID = 0x15bd3276,
	QUIRK_GENE_TWL7_SSID = 0x160dc000,
};

/* CM9825 Offset Definitions */

#define CM9825_VERB_SET_HPF_1 0x781
#define CM9825_VERB_SET_HPF_2 0x785
#define CM9825_VERB_SET_PLL 0x7a0
#define CM9825_VERB_SET_NEG 0x7a1
#define CM9825_VERB_SET_ADCL 0x7a2
#define CM9825_VERB_SET_DACL 0x7a3
#define CM9825_VERB_SET_MBIAS 0x7a4
#define CM9825_VERB_SET_WIGCAP 0x7a6
#define CM9825_VERB_SET_VNEG 0x7a8
#define CM9825_VERB_SET_D2S 0x7a9
#define CM9825_VERB_SET_DACTRL 0x7aa
#define CM9825_VERB_SET_P37CAP 0x7ab
#define CM9825_VERB_SET_PDNEG 0x7ac
#define CM9825_VERB_SET_VDO 0x7ad
#define CM9825_VERB_SET_BYPASSDIGGAIN 0x7ae
#define CM9825_VERB_SET_CDALR 0x7b0
#define CM9825_VERB_SET_MTCBA 0x7b1
#define CM9825_VERB_SET_OTP 0x7b2
#define CM9825_VERB_SET_OCP 0x7b3
#define CM9825_VERB_SET_GAD 0x7b4
#define CM9825_VERB_SET_TMOD 0x7b5
#define CM9825_VERB_SET_SNR 0x7b6
#define CM9825_VERB_SET_VW11TSMODE 0x7b7

struct cmi_spec {
	struct hda_gen_spec gen;
	const struct hda_verb *chip_d0_verbs;
	const struct hda_verb *chip_d3_verbs;
	const struct hda_verb *chip_playback_start_verbs;
	const struct hda_verb *chip_playback_stop_verbs;
	const struct hda_verb *chip_0x34_playback_start_verbs;
	const struct hda_verb *chip_0x34_playback_stop_verbs;
	const struct hda_verb *chip_0x34_present_verbs;
	const struct hda_verb *chip_0x34_remove_verbs;
	const struct hda_verb *chip_0x3b_present_verbs;
	const struct hda_verb *chip_0x3b_remove_verbs;
	const struct hda_verb *chip_0x36_present_verbs;
	const struct hda_verb *chip_0x36_remove_verbs;
	const struct hda_verb *chip_0x37_present_verbs;
	const struct hda_verb *chip_0x37_remove_verbs;
	const struct hda_verb *chip_lineout_retasking_trig_verbs;
	const struct hda_verb *chip_lineout_retasking_remove_verbs;
	const struct hda_verb *chip_linein_retasking_trig_verbs;
	const struct hda_verb *chip_linein_retasking_remove_verbs;
	const struct hda_verb *chip_hp_retasking_trig_verbs;
	const struct hda_verb *chip_hp_retasking_remove_verbs;
	const struct hda_verb *chip_mic_retasking_trig_verbs;
	const struct hda_verb *chip_mic_retasking_remove_verbs;
	struct hda_codec *codec;
	struct delayed_work unsol_line_work;
	struct delayed_work unsol_hp_work;
	int quirk;
	unsigned int playback_started:1;
	unsigned int capture_started:1;
	unsigned int retasking_line:1;
	unsigned int retasking_hp:1;
	hda_nid_t dacs[AUTO_CFG_MAX_OUTS];
	struct snd_pcm_substream *substream;
};

static const struct snd_kcontrol_new arh171_mixer[] = {
	HDA_CODEC_VOLUME("Capture Volume", 0x33, 0, HDA_INPUT),
	HDA_CODEC_MUTE("Capture Switch", 0x33, 0, HDA_INPUT),
	{}			/* end */
};

static const struct hda_verb cm9825_std_d3_verbs[] = {
	/* chip sleep verbs */
	{0x43, CM9825_VERB_SET_D2S, 0x62},	/* depop */
	{0x43, CM9825_VERB_SET_PLL, 0x01},	/* PLL set */
	{0x43, CM9825_VERB_SET_NEG, 0xc2},	/* NEG set */
	{0x43, CM9825_VERB_SET_ADCL, 0x00},	/* ADC */
	{0x43, CM9825_VERB_SET_DACL, 0x02},	/* DACL */
	{0x43, CM9825_VERB_SET_VNEG, 0x50},	/* VOL NEG */
	{0x43, CM9825_VERB_SET_MBIAS, 0x00},	/* MBIAS */
	{0x43, CM9825_VERB_SET_PDNEG, 0x04},	/* SEL OSC */
	{0x43, CM9825_VERB_SET_CDALR, 0xf6},	/* Class D */
	{0x43, CM9825_VERB_SET_OTP, 0xcd},	/* OTP set */
	{}
};

static const struct hda_verb cm9825_std_d0_verbs[] = {
	/* chip init verbs */
	{0x34, AC_VERB_SET_EAPD_BTLENABLE, 0x02},	/* EAPD set */
	{0x43, CM9825_VERB_SET_SNR, 0x30},	/* SNR set */
	{0x43, CM9825_VERB_SET_PLL, 0x00},	/* PLL set */
	{0x43, CM9825_VERB_SET_ADCL, 0x00},	/* ADC */
	{0x43, CM9825_VERB_SET_DACL, 0x02},	/* DACL */
	{0x43, CM9825_VERB_SET_MBIAS, 0x00},	/* MBIAS */
	{0x43, CM9825_VERB_SET_VNEG, 0x56},	/* VOL NEG */
	{0x43, CM9825_VERB_SET_D2S, 0x62},	/* depop */
	{0x43, CM9825_VERB_SET_DACTRL, 0x00},	/* DACTRL set */
	{0x43, CM9825_VERB_SET_PDNEG, 0x0c},	/* SEL OSC */
	{0x43, CM9825_VERB_SET_VDO, 0x80},	/* VDO set */
	{0x43, CM9825_VERB_SET_CDALR, 0xf4},	/* Class D */
	{0x43, CM9825_VERB_SET_OTP, 0xcd},	/* OTP set */
	{0x43, CM9825_VERB_SET_MTCBA, 0x61},	/* SR set */
	{0x43, CM9825_VERB_SET_OCP, 0x33},	/* OTP set */
	{0x43, CM9825_VERB_SET_GAD, 0x07},	/* ADC -3db */
	{0x43, CM9825_VERB_SET_TMOD, 0x26},	/* Class D clk */
	{0x3c, AC_VERB_SET_AMP_GAIN_MUTE | 0xa0, 0x2d},	/* Gain set */
	{0x3c, AC_VERB_SET_AMP_GAIN_MUTE | 0x90, 0x2d},	/* Gain set */
	{0x43, CM9825_VERB_SET_HPF_1, 0x40},	/* HPF set */
	{0x43, CM9825_VERB_SET_HPF_2, 0x40},	/* HPF set */
	{}
};

static const struct hda_verb cm9825_ncr_d3_verbs[] = {
	{0x43, CM9825_VERB_SET_D2S, 0x62},
	{0x43, CM9825_VERB_SET_PLL, 0x01},
	{0x43, CM9825_VERB_SET_NEG, 0xc2},
	{0x43, CM9825_VERB_SET_ADCL, 0x00},
	{0x43, CM9825_VERB_SET_DACL, 0x02},
	{0x43, CM9825_VERB_SET_VNEG, 0x50},
	{0x43, CM9825_VERB_SET_MBIAS, 0x00},
	{0x43, CM9825_VERB_SET_PDNEG, 0x04},
	{0x43, CM9825_VERB_SET_CDALR, 0xf6},
	{0x43, CM9825_VERB_SET_OTP, 0xcd},
	{}
};

static const struct hda_verb cm9825_ncr_d0_verbs[] = {
	{0x34, AC_VERB_SET_EAPD_BTLENABLE, 0x02},
	{0x43, CM9825_VERB_SET_SNR, 0x30},
	{0x43, CM9825_VERB_SET_PLL, 0x00},
	{0x43, CM9825_VERB_SET_ADCL, 0x00},
	{0x43, CM9825_VERB_SET_DACL, 0x02},
	{0x43, CM9825_VERB_SET_MBIAS, 0x00},
	{0x43, CM9825_VERB_SET_VNEG, 0x56},
	{0x43, CM9825_VERB_SET_D2S, 0x62},
	{0x43, CM9825_VERB_SET_DACTRL, 0x00},
	{0x43, CM9825_VERB_SET_PDNEG, 0x0c},
	{0x43, CM9825_VERB_SET_VDO, 0xc4},
	{0x43, CM9825_VERB_SET_CDALR, 0xf4},
	{0x43, CM9825_VERB_SET_OTP, 0xcd},
	{0x43, CM9825_VERB_SET_MTCBA, 0x61},
	{0x43, CM9825_VERB_SET_OCP, 0x33},
	{0x43, CM9825_VERB_SET_GAD, 0x07},
	{0x43, CM9825_VERB_SET_TMOD, 0x26},
	{0x3c, AC_VERB_SET_AMP_GAIN_MUTE | 0xa0, 0x2d},
	{0x3c, AC_VERB_SET_AMP_GAIN_MUTE | 0x90, 0x2d},
	{0x43, CM9825_VERB_SET_HPF_1, 0x40},
	{0x43, CM9825_VERB_SET_HPF_2, 0x40},
	{0x3c, AC_VERB_SET_AMP_GAIN_MUTE | 0x41, 0x80},
	{0x01, 0x720, 0xf0},
	{0x01, 0x721, 0x88},
	{0x01, AC_VERB_SET_GPIO_DATA, 0x01},
	{0x01, AC_VERB_SET_GPIO_DIRECTION, 0x01},
	{0x3c, AC_VERB_SET_AMP_GAIN_MUTE | 0x40, 0x00},
	{}
};

static const struct hda_verb cm9825_ibp_d3_verbs[] = {
	{0x43, CM9825_VERB_SET_D2S, 0x62},	/* depop */
	{0x43, CM9825_VERB_SET_PLL, 0x01},	/* PLL set */
	{0x43, CM9825_VERB_SET_NEG, 0xc2},	/* NEG set */
	{0x43, CM9825_VERB_SET_ADCL, 0x00},	/* ADC */
	{0x43, CM9825_VERB_SET_DACL, 0x02},	/* DACL */
	{0x43, CM9825_VERB_SET_VNEG, 0x50},	/* VOL NEG */
	{0x43, CM9825_VERB_SET_MBIAS, 0x00},	/* MBIAS */
	{0x43, CM9825_VERB_SET_PDNEG, 0x04},	/* SEL OSC */
	{0x43, CM9825_VERB_SET_CDALR, 0xf6},	/* Class D */
	{0x43, CM9825_VERB_SET_OTP, 0xcd},	/* OTP set */
	{}
};

static const struct hda_verb cm9825_ibp_d0_verbs[] = {
	{0x34, AC_VERB_SET_EAPD_BTLENABLE, 0x02},	/* EAPD set */
	{0x43, CM9825_VERB_SET_SNR, 0x38},	/* SNR set */
	{0x43, CM9825_VERB_SET_PLL, 0x00},	/* PLL set */
	{0x43, CM9825_VERB_SET_ADCL, 0x00},	/* ADC */
	{0x43, CM9825_VERB_SET_DACL, 0x02},	/* DACL */
	{0x43, CM9825_VERB_SET_MBIAS, 0x00},	/* MBIAS */
	{0x43, CM9825_VERB_SET_VNEG, 0x56},	/* VOL NEG */
	{0x43, CM9825_VERB_SET_D2S, 0x62},	/* depop */
	{0x43, CM9825_VERB_SET_DACTRL, 0x00},	/* DACTRL set */
	{0x43, CM9825_VERB_SET_PDNEG, 0x0c},	/* SEL OSC */
	{0x43, CM9825_VERB_SET_CDALR, 0xf4},	/* Class D */
	{0x43, CM9825_VERB_SET_OTP, 0xcd},	/* OTP set */
	{0x43, CM9825_VERB_SET_MTCBA, 0x61},	/* SR set */
	{0x43, CM9825_VERB_SET_OCP, 0x33},	/* OTP set */
	{0x43, CM9825_VERB_SET_GAD, 0x07},	/* ADC -3db */
	{0x43, CM9825_VERB_SET_TMOD, 0x26},	/* Class D clk */
	{0x3c, AC_VERB_SET_AMP_GAIN_MUTE | 0xa0, 0x2d},	/* Gain set */
	{0x3c, AC_VERB_SET_AMP_GAIN_MUTE | 0x90, 0x2d},	/* Gain set */
	{0x43, CM9825_VERB_SET_HPF_1, 0x40},	/* HPF set */
	{0x43, CM9825_VERB_SET_HPF_2, 0x40},	/* HPF set */
	{}
};

static const struct hda_verb cm9825_asl051_d3_verbs[] = {
	{0x43, CM9825_VERB_SET_D2S, 0x62},	/* depop */
	{0x43, CM9825_VERB_SET_PLL, 0x01},	/* PLL set */
	{0x43, CM9825_VERB_SET_NEG, 0xc2},	/* NEG set */
	{0x43, CM9825_VERB_SET_ADCL, 0x00},	/* ADC */
	{0x43, CM9825_VERB_SET_DACL, 0x02},	/* DACL */
	{0x43, CM9825_VERB_SET_VNEG, 0x50},	/* VOL NEG */
	{0x43, CM9825_VERB_SET_MBIAS, 0x00},	/* MBIAS */
	{0x43, CM9825_VERB_SET_PDNEG, 0x04},	/* SEL OSC */
	{0x43, CM9825_VERB_SET_CDALR, 0xf6},	/* Class D */
	{0x43, CM9825_VERB_SET_OTP, 0xcd},	/* OTP set */
	{}
};

static const struct hda_verb cm9825_asl051_d0_verbs[] = {
	{0x34, AC_VERB_SET_EAPD_BTLENABLE, 0x02},	/* EAPD set */
	{0x43, CM9825_VERB_SET_SNR, 0x38},	/* SNR set */
	{0x43, CM9825_VERB_SET_PLL, 0x00},	/* PLL set */
	{0x43, CM9825_VERB_SET_ADCL, 0x00},	/* ADC */
	{0x43, CM9825_VERB_SET_DACL, 0x02},	/* DACL */
	{0x43, CM9825_VERB_SET_MBIAS, 0x00},	/* MBIAS */
	{0x43, CM9825_VERB_SET_VNEG, 0x56},	/* VOL NEG */
	{0x43, CM9825_VERB_SET_D2S, 0x62},	/* depop */
	{0x43, CM9825_VERB_SET_DACTRL, 0x00},	/* DACTRL set */
	{0x43, CM9825_VERB_SET_PDNEG, 0x0c},	/* SEL OSC */
	{0x43, CM9825_VERB_SET_CDALR, 0xf4},	/* Class D */
	{0x43, CM9825_VERB_SET_OTP, 0xcd},	/* OTP set */
	{0x43, CM9825_VERB_SET_MTCBA, 0x61},	/* SR set */
	{0x43, CM9825_VERB_SET_OCP, 0x33},	/* OTP set */
	{0x43, CM9825_VERB_SET_GAD, 0x07},	/* ADC -3db */
	{0x43, CM9825_VERB_SET_TMOD, 0x26},	/* Class D clk */
	{0x3c, AC_VERB_SET_AMP_GAIN_MUTE | 0xa0, 0x2d},	/* Gain set */
	{0x3c, AC_VERB_SET_AMP_GAIN_MUTE | 0x90, 0x2d},	/* Gain set */
	{0x43, CM9825_VERB_SET_HPF_1, 0x40},	/* HPF set */
	{0x43, CM9825_VERB_SET_HPF_2, 0x40},	/* HPF set */
	{0x3a, AC_VERB_SET_AMP_GAIN_MUTE | 0x20, 0x11},
	{0x3a, AC_VERB_SET_AMP_GAIN_MUTE | 0x10, 0x11},
	{0x39, AC_VERB_SET_AMP_GAIN_MUTE | 0x20, 0x11},
	{0x39, AC_VERB_SET_AMP_GAIN_MUTE | 0x10, 0x11},
	{0x43, CM9825_VERB_SET_BYPASSDIGGAIN, 0x10},
	{}
};

static const struct hda_verb cm9825_arh171_tp_verbs[] = {
	{0x46, CM9825_VERB_SET_WIGCAP, 0x8b},
	{0x46, CM9825_VERB_SET_NEG, 0x10},
	{0x01, AC_VERB_SET_GPIO_DIRECTION, 0x01},
	{0x3a, AC_VERB_SET_AMP_GAIN_MUTE | 0x20, 0x11},
	{0x3a, AC_VERB_SET_AMP_GAIN_MUTE | 0x10, 0x11},
	{0x43, CM9825_VERB_SET_BYPASSDIGGAIN, 0x10},
	{}
};

static const struct hda_verb cm9825_arh171_d3_verbs[] = {
	{0x43, CM9825_VERB_SET_D2S, 0x62},
	{0x43, CM9825_VERB_SET_VDO, 0x90},
	{0x43, CM9825_VERB_SET_VW11TSMODE, 0x00},
	{0x43, CM9825_VERB_SET_DACTRL, 0x00},
	{0x43, CM9825_VERB_SET_PLL, 0x01},
	{0x43, CM9825_VERB_SET_NEG, 0xc2},
	{0x43, CM9825_VERB_SET_ADCL, 0x00},
	{0x43, CM9825_VERB_SET_DACL, 0x02},
	{0x43, CM9825_VERB_SET_MBIAS, 0x00},
	{0x43, CM9825_VERB_SET_VNEG, 0x50},
	{0x43, CM9825_VERB_SET_PDNEG, 0x04},
	{0x43, CM9825_VERB_SET_CDALR, 0xf6},
	{0x43, CM9825_VERB_SET_OTP, 0xcd},
	{}
};

static const struct hda_verb cm9825_arh171_d0_verbs[] = {
	{0x34, AC_VERB_SET_EAPD_BTLENABLE, 0x02},
	{0x43, CM9825_VERB_SET_SNR, 0x30},
	{0x43, CM9825_VERB_SET_VW11TSMODE, 0x08},
	{0x43, CM9825_VERB_SET_PLL, 0x00},
	{0x43, CM9825_VERB_SET_ADCL, 0x00},
	{0x43, CM9825_VERB_SET_DACL, 0x02},
	{0x43, CM9825_VERB_SET_MBIAS, 0x00},
	{0x43, CM9825_VERB_SET_VNEG, 0x56},
	{0x43, CM9825_VERB_SET_D2S, 0x62},
	{0x43, CM9825_VERB_SET_DACTRL, 0x00},
	{0x43, CM9825_VERB_SET_PDNEG, 0x0c},
	{0x43, CM9825_VERB_SET_VDO, 0x90},
	{0x43, CM9825_VERB_SET_CDALR, 0xf4},
	{0x43, CM9825_VERB_SET_OTP, 0xcd},
	{0x43, CM9825_VERB_SET_MTCBA, 0x61},
	{0x43, CM9825_VERB_SET_OCP, 0x33},
	{0x43, CM9825_VERB_SET_GAD, 0x07},
	{0x43, CM9825_VERB_SET_TMOD, 0x26},
	{0x3c, AC_VERB_SET_AMP_GAIN_MUTE | 0xa0, 0x2d},
	{0x3c, AC_VERB_SET_AMP_GAIN_MUTE | 0x90, 0x2d},
	{0x43, CM9825_VERB_SET_HPF_1, 0x40},
	{0x43, CM9825_VERB_SET_HPF_2, 0x40},
	{}
};

static const struct hda_verb cm9825_gene_twl7_d3_verbs[] = {
	{0x43, CM9825_VERB_SET_D2S, 0x62},
	{0x43, CM9825_VERB_SET_PLL, 0x01},
	{0x43, CM9825_VERB_SET_NEG, 0xc2},
	{0x43, CM9825_VERB_SET_ADCL, 0x00},
	{0x43, CM9825_VERB_SET_DACL, 0x02},
	{0x43, CM9825_VERB_SET_MBIAS, 0x00},
	{0x43, CM9825_VERB_SET_VNEG, 0x50},
	{0x43, CM9825_VERB_SET_PDNEG, 0x04},
	{0x43, CM9825_VERB_SET_CDALR, 0xf6},
	{0x43, CM9825_VERB_SET_OTP, 0xcd},
	{}
};

static const struct hda_verb cm9825_gene_twl7_d0_verbs[] = {
	{0x34, AC_VERB_SET_EAPD_BTLENABLE, 0x02},
	{0x43, CM9825_VERB_SET_SNR, 0x38},
	{0x43, CM9825_VERB_SET_PLL, 0x00},
	{0x43, CM9825_VERB_SET_ADCL, 0xcf},
	{0x43, CM9825_VERB_SET_DACL, 0xaa},
	{0x43, CM9825_VERB_SET_MBIAS, 0x1c},
	{0x43, CM9825_VERB_SET_VNEG, 0x56},
	{0x43, CM9825_VERB_SET_D2S, 0x62},
	{0x43, CM9825_VERB_SET_DACTRL, 0x00},
	{0x43, CM9825_VERB_SET_PDNEG, 0x0c},
	{0x43, CM9825_VERB_SET_CDALR, 0xf4},
	{0x43, CM9825_VERB_SET_OTP, 0xcd},
	{0x43, CM9825_VERB_SET_MTCBA, 0x61},
	{0x43, CM9825_VERB_SET_OCP, 0x33},
	{0x43, CM9825_VERB_SET_GAD, 0x07},
	{0x43, CM9825_VERB_SET_TMOD, 0x26},
	{0x43, CM9825_VERB_SET_HPF_1, 0x40},
	{0x43, CM9825_VERB_SET_HPF_2, 0x40},
	{0x40, AC_VERB_SET_CONNECT_SEL, 0x00},
	{0x3d, AC_VERB_SET_CONNECT_SEL, 0x01},
	{0x46, CM9825_VERB_SET_P37CAP, 0x20},
	{}
};

static const struct hda_verb cm9825_arh171_hp_present_verbs[] = {
	{0x01, AC_VERB_SET_GPIO_DATA, 0x00},
	{}
};

static const struct hda_verb cm9825_arh171_hp_remove_verbs[] = {
	{}
};

static const struct hda_verb cm9825_arh171_micin_present_verbs[] = {
	{0x40, AC_VERB_SET_CONNECT_SEL, 0x00},
	{0x3a, AC_VERB_SET_CONNECT_SEL, 0x01},
	{}
};

static const struct hda_verb cm9825_arh171_micin_remove_verbs[] = {
	{0x40, AC_VERB_SET_CONNECT_SEL, 0x01},
	{0x3a, AC_VERB_SET_CONNECT_SEL, 0x01},
	{}
};

static const struct hda_verb cm9825_arh171_spk_playback_start_verbs[] = {
	{0x01, AC_VERB_SET_GPIO_DATA, 0x01},
	{0x43, CM9825_VERB_SET_DACTRL, 0xe0},
	{}
};

static const struct hda_verb cm9825_arh171_spk_playback_stop_verbs[] = {
	{0x01, AC_VERB_SET_GPIO_DATA, 0x00},
	{0x43, CM9825_VERB_SET_DACTRL, 0x00},
	{}
};

static const struct hda_verb cm9825_arh171_hp_playback_start_verbs[] = {
	{0x43, CM9825_VERB_SET_D2S, 0xf2},
	{0x43, CM9825_VERB_SET_VDO, 0xd4},
	{}
};

static const struct hda_verb cm9825_arh171_hp_playback_stop_verbs[] = {
	{0x43, CM9825_VERB_SET_VDO, 0xc0},
	{0x43, CM9825_VERB_SET_D2S, 0x62},
	{0x43, CM9825_VERB_SET_VDO, 0x90},
	{}
};

static const struct hda_verb cm9825_ncr_playback_start_verbs[] = {
	{0x43, CM9825_VERB_SET_DACL, 0xAE},
	{0x43, CM9825_VERB_SET_D2S, 0xF2},
	{0x43, CM9825_VERB_SET_VDO, 0xC4},
	{0x43, CM9825_VERB_SET_DACTRL, 0xE0},
	{}
};

static const struct hda_verb cm9825_ncr_playback_stop_verbs[] = {
	{0x43, CM9825_VERB_SET_VDO, 0xC0},
	{0x43, CM9825_VERB_SET_D2S, 0x62},
	{0x43, CM9825_VERB_SET_VDO, 0x80},
	{0x43, CM9825_VERB_SET_DACTRL, 0x00},
	{0x43, CM9825_VERB_SET_DACL, 0x02},
	{}
};

static const struct hda_verb cm9825_ibp_playback_start_verbs[] = {
	{0x43, CM9825_VERB_SET_DACL, 0xaa},
	{0x43, CM9825_VERB_SET_D2S, 0xf2},
	{0x43, CM9825_VERB_SET_VDO, 0xc4},
	{0x43, CM9825_VERB_SET_SNR, 0x30},
	{}
};

static const struct hda_verb cm9825_ibp_playback_stop_verbs[] = {
	{0x43, CM9825_VERB_SET_VDO, 0xc0},
	{0x43, CM9825_VERB_SET_DACL, 0x02},
	{0x43, CM9825_VERB_SET_D2S, 0x62},
	{0x43, CM9825_VERB_SET_VDO, 0x80},
	{0x43, CM9825_VERB_SET_SNR, 0x38},
	{}
};

static const struct hda_verb cm9825_gene_twl7_playback_start_verbs[] = {
	{0x43, CM9825_VERB_SET_D2S, 0xf2},
	{0x43, CM9825_VERB_SET_VDO, 0xd4},
	{0x43, CM9825_VERB_SET_SNR, 0x30},
	{}
};

static const struct hda_verb cm9825_gene_twl7_playback_stop_verbs[] = {
	{0x43, CM9825_VERB_SET_VDO, 0xc0},
	{0x43, CM9825_VERB_SET_D2S, 0x62},
	{0x43, CM9825_VERB_SET_VDO, 0xd0},
	{0x43, CM9825_VERB_SET_SNR, 0x38},
	{}
};

static const struct hda_verb cm9825_hp_present_verbs[] = {
	{0x42, AC_VERB_SET_PIN_WIDGET_CONTROL, 0x00},	/* PIN off */
	{0x43, CM9825_VERB_SET_ADCL, 0x88},	/* ADC */
	{0x43, CM9825_VERB_SET_DACL, 0xaa},	/* DACL */
	{0x43, CM9825_VERB_SET_MBIAS, 0x10},	/* MBIAS */
	{0x43, CM9825_VERB_SET_D2S, 0xf2},	/* depop */
	{0x43, CM9825_VERB_SET_DACTRL, 0x00},	/* DACTRL set */
	{0x43, CM9825_VERB_SET_VDO, 0xc4},	/* VDO set */
	{}
};

static const struct hda_verb cm9825_hp_remove_verbs[] = {
	{0x43, CM9825_VERB_SET_ADCL, 0x00},	/* ADC */
	{0x43, CM9825_VERB_SET_DACL, 0x56},	/* DACL */
	{0x43, CM9825_VERB_SET_MBIAS, 0x00},	/* MBIAS */
	{0x43, CM9825_VERB_SET_D2S, 0x62},	/* depop */
	{0x43, CM9825_VERB_SET_DACTRL, 0xe0},	/* DACTRL set */
	{0x43, CM9825_VERB_SET_VDO, 0x80},	/* VDO set */
	{0x42, AC_VERB_SET_PIN_WIDGET_CONTROL, 0x40},	/* PIN on */
	{}
};

static const struct hda_verb cm9825_ibp_hp_present_verbs[] = {
	{0x43, CM9825_VERB_SET_ADCL, 0x8c},
	{0x43, CM9825_VERB_SET_MBIAS, 0x10},
	{}
};

static const struct hda_verb cm9825_ibp_hp_remove_verbs[] = {
	{0x43, CM9825_VERB_SET_ADCL, 0x00},
	{0x43, CM9825_VERB_SET_MBIAS, 0x00},
	{}
};

static const struct hda_verb asl051_retasking_lineout_trig_verbs[] = {
	{0x43, CM9825_VERB_SET_DACTRL, 0xe0},
	{0x43, CM9825_VERB_SET_SNR, 0x30},
	{}
};

static const struct hda_verb asl051_retasking_lineout_remove_verbs[] = {
	{0x43, CM9825_VERB_SET_DACTRL, 0x00},
	{0x43, CM9825_VERB_SET_SNR, 0x38},
	{}
};

static const struct hda_verb asl051_retasking_linein_trig_verbs[] = {
	{0x40, AC_VERB_SET_CONNECT_SEL, 0x00},
	{0x43, CM9825_VERB_SET_DACTRL, 0x00},
	{}
};

static const struct hda_verb asl051_retasking_linein_remove_verbs[] = {
	{0x40, AC_VERB_SET_CONNECT_SEL, 0x01},
	{}
};

static const struct hda_verb asl051_retasking_hp_trig_verbs[] = {
	{0x43, CM9825_VERB_SET_D2S, 0xf2},
	{0x43, CM9825_VERB_SET_VDO, 0xc4},
	{0x43, CM9825_VERB_SET_SNR, 0x30},
	{}
};

static const struct hda_verb asl051_retasking_hp_remove_verbs[] = {
	{0x43, CM9825_VERB_SET_VDO, 0xc0},
	{0x43, CM9825_VERB_SET_D2S, 0x62},
	{0x43, CM9825_VERB_SET_VDO, 0x80},
	{0x43, CM9825_VERB_SET_SNR, 0x38},
	{}
};

static const struct hda_verb asl051_retasking_mic_trig_verbs[] = {
	{0x3d, AC_VERB_SET_CONNECT_SEL, 0x01},
	{0x43, CM9825_VERB_SET_VDO, 0x90},
	{}
};

static const struct hda_verb asl051_retasking_mic_remove_verbs[] = {
	{0x3d, AC_VERB_SET_CONNECT_SEL, 0x00},
	{0x43, CM9825_VERB_SET_VDO, 0x80},
	{}
};

static struct hda_multi_out asl051_multi_out = {
	.num_dacs = 2,
	.dac_nids = (hda_nid_t[]){0x31, 0x30},
	.max_channels = 2,
	.dig_out_nid = 0,
	.share_spdif = false,
};

static void asl051_retasking_bias2_sw(struct hda_codec *codec, int en)
{
	unsigned int val;

	if (en) {
		val = snd_hda_codec_read(codec, 0x43, 0, 0xfa4, 0x0);
		val = (val & 0xff) | 0x08;
	} else {
		val = snd_hda_codec_read(codec, 0x43, 0, 0xfa4, 0x0);
		val = (val & 0xf7);
	}

	snd_hda_codec_write(codec, 0x43, 0, CM9825_VERB_SET_MBIAS, val);
}

static void asl051_retasking_bias1_sw(struct hda_codec *codec, int en)
{
	unsigned int val;

	if (en) {
		val = snd_hda_codec_read(codec, 0x43, 0, 0xfa4, 0x0);
		val = (val & 0xff) | 0x10;
	} else {
		val = snd_hda_codec_read(codec, 0x43, 0, 0xfa4, 0x0);
		val = val & 0xfb;
	}

	snd_hda_codec_write(codec, 0x43, 0, CM9825_VERB_SET_MBIAS, val);
}

static void asl051_retasking_mic(struct hda_codec *codec, int trig)
{
	struct cmi_spec *spec = codec->spec;
	unsigned int val;

	codec_dbg(codec, "%s trig %d\n", __func__, trig);

	if (trig) {
		val = snd_hda_codec_read(codec, 0x43, 0, 0xfa0, 0x0);
		val = ((val >> 16) & 0xff) | 0x8c;
		snd_hda_codec_write(codec, 0x43, 0, CM9825_VERB_SET_ADCL, val);
		asl051_retasking_bias2_sw(codec, 1);
		snd_hda_sequence_write(codec,
				       spec->chip_mic_retasking_trig_verbs);
	} else {
		snd_hda_sequence_write(codec,
				       spec->chip_mic_retasking_remove_verbs);
		val = snd_hda_codec_read(codec, 0x43, 0, 0xfa0, 0x0);
		val = (val >> 16) & 0x73;
		snd_hda_codec_write(codec, 0x43, 0, CM9825_VERB_SET_ADCL, val);
		asl051_retasking_bias2_sw(codec, 0);
	}
}

static void asl051_retasking_hp(struct hda_codec *codec, int trig)
{
	struct cmi_spec *spec = codec->spec;
	unsigned int val;

	if (trig) {
		asl051_retasking_bias2_sw(codec, 0);
		val = snd_hda_codec_read(codec, 0x43, 0, 0xfa0, 0x0);
		val = (val >> 24) | 0xa8;
		snd_hda_codec_write(codec, 0x43, 0, CM9825_VERB_SET_DACL, val);
		snd_hda_sequence_write(codec,
				       spec->chip_hp_retasking_trig_verbs);
	} else {
		snd_hda_sequence_write(codec,
				       spec->chip_hp_retasking_remove_verbs);
		val = snd_hda_codec_read(codec, 0x43, 0, 0xfa0, 0x0);
		val = (val >> 24) & 0x57;
		snd_hda_codec_write(codec, 0x43, 0, CM9825_VERB_SET_DACL, val);
	}
}

static void asl051_retasking_lineout(struct hda_codec *codec, int trig)
{
	struct cmi_spec *spec = codec->spec;
	unsigned int val;

	if (trig) {
		asl051_retasking_bias1_sw(codec, 0);
		val = snd_hda_codec_read(codec, 0x43, 0, 0xfa0, 0x0);
		val = (val >> 16) & 0xbc;
		snd_hda_codec_write(codec, 0x43, 0, CM9825_VERB_SET_ADCL, val);
		val = snd_hda_codec_read(codec, 0x43, 0, 0xfa0, 0x0);
		val = (val >> 24) | 0x54;
		snd_hda_codec_write(codec, 0x43, 0, CM9825_VERB_SET_DACL, val);
		snd_hda_sequence_write(codec,
				       spec->chip_lineout_retasking_trig_verbs);
	} else {
		snd_hda_sequence_write(codec,
				       spec->chip_lineout_retasking_remove_verbs);
		val = snd_hda_codec_read(codec, 0x43, 0, 0xfa0, 0x0);
		val = (val >> 24) & 0xab;
		snd_hda_codec_write(codec, 0x43, 0, CM9825_VERB_SET_DACL, val);
	}
}

static void asl051_retasking_linein(struct hda_codec *codec, int trig)
{
	struct cmi_spec *spec = codec->spec;
	unsigned int val;

	if (trig) {
		val = snd_hda_codec_read(codec, 0x43, 0, 0xfa0, 0x0);
		val = (val >> 24) | 0x54;
		snd_hda_codec_write(codec, 0x43, 0, CM9825_VERB_SET_DACL, val);
		val = snd_hda_codec_read(codec, 0x43, 0, 0xfa0, 0x0);
		val = ((val >> 16) & 0xff) | 0x43;
		snd_hda_codec_write(codec, 0x43, 0, CM9825_VERB_SET_ADCL, val);
		snd_hda_sequence_write(codec,
				       spec->chip_linein_retasking_trig_verbs);
		asl051_retasking_bias1_sw(codec, 1);
	} else {
		val = snd_hda_codec_read(codec, 0x43, 0, 0xfa0, 0x0);
		val = (val >> 16) & 0xbc;
		snd_hda_codec_write(codec, 0x43, 0, CM9825_VERB_SET_ADCL, val);
		snd_hda_sequence_write(codec,
				       spec->chip_linein_retasking_remove_verbs);
		asl051_retasking_bias1_sw(codec, 0);
	}
}

static void cm9825_unsol_hp_delayed(struct work_struct *work)
{
	struct cmi_spec *spec =
	    container_of(to_delayed_work(work), struct cmi_spec, unsol_hp_work);
	struct hda_jack_tbl *jack;
	hda_nid_t hp_pin = 0x36;
	bool hp_jack_plugin = false;
	int err = 0;

	hp_jack_plugin = snd_hda_jack_detect(spec->codec, hp_pin);

	codec_dbg(spec->codec, "hp_jack_plugin %d, hp_pin 0x%X\n",
		  (int)hp_jack_plugin, hp_pin);

	if (!hp_jack_plugin) {
		spec->gen.hp_jack_present = false;

		err =
		    snd_hda_codec_write(spec->codec, 0x42, 0,
					AC_VERB_SET_PIN_WIDGET_CONTROL, 0x40);
		if (err)
			codec_dbg(spec->codec, "codec_write err %d\n", err);

		if (spec->codec->core.subsystem_id != QUIRK_ASL051_SSID) {
			if (spec->chip_0x36_remove_verbs) {
				snd_hda_sequence_write(spec->codec,
						       spec->chip_0x36_remove_verbs);
			}
		}
	} else {
		if (spec->codec->core.subsystem_id != QUIRK_ASL051_SSID) {
			if (spec->chip_0x36_present_verbs) {
				snd_hda_sequence_write(spec->codec,
						       spec->chip_0x36_present_verbs);
			}
		}
	}

	jack = snd_hda_jack_tbl_get(spec->codec, hp_pin);
	if (jack) {
		jack->block_report = 0;
		jack->jack_dirty = 1;
		jack->gated_jack = 1;
		snd_hda_jack_report_sync(spec->codec);
	}

	snd_hda_gen_update_outputs(spec->codec);

	if (spec->playback_started)
		if (spec->substream)
			snd_pcm_stop_xrun(spec->substream);
}

static void hp_callback(struct hda_codec *codec, struct hda_jack_callback *cb)
{
	struct cmi_spec *spec = codec->spec;
	struct hda_jack_tbl *tbl;

	/* Delay enabling the HP amp, to let the mic-detection
	 * state machine run.
	 */

	codec_dbg(spec->codec, "cb->nid 0x%X\n", cb->nid);

	tbl = snd_hda_jack_tbl_get(codec, cb->nid);
	if (tbl)
		tbl->block_report = 1;
	schedule_delayed_work(&spec->unsol_hp_work, msecs_to_jiffies(200));
}

static void cm9825_unsol_line_delayed(struct work_struct *work)
{
	struct cmi_spec *spec =
	    container_of(to_delayed_work(work), struct cmi_spec,
			 unsol_line_work);
	struct hda_jack_tbl *jack;
	hda_nid_t lineout_pin = 0x3b;
	bool lineout_jack_plugin = false;
	unsigned int val = 0;

	lineout_jack_plugin = snd_hda_jack_detect(spec->codec, 0x3b);

	if (!lineout_jack_plugin) {
		spec->gen.line_jack_present = false;
		if (spec->codec->core.subsystem_id == QUIRK_ARH171_SSID) {
			val =
			    snd_hda_codec_read(spec->codec, 0x43, 0, 0xfa0,
					       0x0);
			val = (val >> 16) & 0xbc;	// adc2 off<6,1,0>=0
			snd_hda_codec_write(spec->codec, 0x43, 0,
					    CM9825_VERB_SET_ADCL, val);
			val =
			    snd_hda_codec_read(spec->codec, 0x43, 0, 0xfa4,
					       0x0);
			val = (val & 0xff) & 0xed;	// bias1 off<4>=0
			snd_hda_codec_write(spec->codec, 0x43, 0,
					    CM9825_VERB_SET_MBIAS, val);
			if (spec->chip_0x37_remove_verbs) {
				snd_hda_sequence_write(spec->codec,
						       spec->chip_0x37_remove_verbs);
			}
		}
	} else {
		spec->gen.line_jack_present = true;
		if (spec->codec->core.subsystem_id == QUIRK_ARH171_SSID) {
			val =
			    snd_hda_codec_read(spec->codec, 0x43, 0, 0xfa0,
					       0x0);
			val = ((val >> 16) & 0xff) | 0x43;	// <6,1,0>=1
			snd_hda_codec_write(spec->codec, 0x43, 0,
					    CM9825_VERB_SET_ADCL, val);
			val =
			    snd_hda_codec_read(spec->codec, 0x43, 0, 0xfa4,
					       0x0);
			val = (val & 0xff) | 0x10;	// BIAS1 on<4>=1
			val = val & 0xbc;	// <6,1,0> = 0
			snd_hda_codec_write(spec->codec, 0x43, 0,
					    CM9825_VERB_SET_MBIAS, val);
			if (spec->chip_0x37_present_verbs) {
				snd_hda_sequence_write(spec->codec,
						       spec->chip_0x37_present_verbs);
			}
		}
	}

	jack = snd_hda_jack_tbl_get(spec->codec, lineout_pin);

	if (jack) {
		jack->block_report = 0;
		jack->jack_dirty = 1;
		jack->gated_jack = 1;
		snd_hda_jack_report_sync(spec->codec);
	}

	snd_hda_gen_update_outputs(spec->codec);
}

static void line_callback(struct hda_codec *codec, struct hda_jack_callback *cb)
{
	struct cmi_spec *spec = codec->spec;
	struct hda_jack_tbl *tbl;

	/* Delay enabling the lineout amp, to let the linein-detection
	 * state machine run.
	 */

	codec_dbg(codec, "%s, cb->nid 0x%X, line%d\n", __func__,
		  (int)cb->nid, __LINE__);

	tbl = snd_hda_jack_tbl_get(codec, cb->nid);
	if (tbl)
		tbl->block_report = 1;
	schedule_delayed_work(&spec->unsol_line_work, msecs_to_jiffies(200));
}

static void cm9825_setup_unsol(struct hda_codec *codec)
{
	struct cmi_spec *spec = codec->spec;

	hda_nid_t pin0x36 = 0x36;

	snd_hda_jack_detect_enable_callback(codec, pin0x36, hp_callback);

	if (codec->core.subsystem_id == QUIRK_ASL051_SSID) {
		hda_nid_t lineout_pin = spec->gen.autocfg.line_out_pins[0];

		snd_hda_jack_detect_enable_callback(codec, lineout_pin,
						    line_callback);
	} else if (codec->core.subsystem_id == QUIRK_ARH171_SSID
		   || codec->core.subsystem_id == QUIRK_GENE_TWL7_SSID) {
		hda_nid_t linein_pin = 0x3b;

		snd_hda_jack_detect_enable_callback(codec, linein_pin,
						    line_callback);
	}
}

static void cm9825_init_hook(struct hda_codec *codec)
{
	unsigned int val;

	codec_dbg(codec, "init hook\n");

	/* OMTP */
	val = snd_hda_codec_read(codec, 0x46, 0, 0xfec, 0x0);
	snd_hda_codec_write(codec, 0x46, 0, 0x7ef, (val >> 24) & 0x7f);

	// link reset
	if (codec->core.subsystem_id == QUIRK_ASL051_SSID) {
		snd_hda_codec_write(codec, 0x3a, 0, AC_VERB_SET_CONNECT_SEL,
				    0x02);
		snd_hda_codec_write(codec, 0x40, 0, AC_VERB_SET_CONNECT_SEL,
				    0x40);
		asl051_retasking_bias1_sw(codec, 0);
		asl051_retasking_bias2_sw(codec, 0);
	} else if (codec->core.subsystem_id == QUIRK_ARH171_SSID) {
		snd_hda_codec_write(codec, 0x3a, 0, AC_VERB_SET_CONNECT_SEL,
				    0x02);
		snd_hda_codec_write(codec, 0x40, 0, AC_VERB_SET_CONNECT_SEL,
				    0x40);
		snd_hda_sequence_write(codec, cm9825_arh171_tp_verbs);
		snd_hda_sequence_write(codec, cm9825_arh171_d0_verbs);
	} else if (codec->core.subsystem_id == QUIRK_GENE_TWL7_SSID) {
		snd_hda_sequence_write(codec, cm9825_gene_twl7_d0_verbs);
		snd_hda_codec_write(codec, 0x46, 0, CM9825_VERB_SET_P37CAP,
				    0x20);
	}
}

static void cm9825_playback_pcm_hook(struct hda_pcm_stream *hinfo,
				     struct hda_codec *codec,
				     struct snd_pcm_substream *substream,
				     int action)
{
	struct cmi_spec *spec = codec->spec;
	unsigned int val;

	switch (action) {
	case HDA_GEN_PCM_ACT_PREPARE:
		spec->playback_started = 1;
		spec->substream = substream;
		if (codec->core.subsystem_id == QUIRK_ASL051_SSID) {
			if (spec->retasking_line == 0)
				asl051_retasking_lineout(codec, 1);

			if (spec->retasking_hp == 0)
				asl051_retasking_hp(codec, 1);
		} else if (codec->core.subsystem_id == QUIRK_ARH171_SSID) {
			if (!spec->gen.hp_jack_present) {
				hinfo->nid = (hda_nid_t) 0x31;
				snd_hda_codec_write(codec, 0x01, 0,
						    AC_VERB_SET_GPIO_DIRECTION,
						    0x01);
				val =
				    snd_hda_codec_read(codec, 0x43, 0, 0xfa0,
						       0x0);
				val = (val >> 24) | 0x54;	// <6,4,2>=1
				snd_hda_codec_write(codec, 0x43, 0,
						    CM9825_VERB_SET_DACL, val);
				snd_hda_sequence_write(spec->codec,
						       cm9825_arh171_spk_playback_start_verbs);
			} else {
				hinfo->nid = (hda_nid_t) 0x30;
				val =
				    snd_hda_codec_read(codec, 0x43, 0, 0xfa0,
						       0x0);
				val = (val >> 24) | 0xa8;	// <7,5,3>=1
				snd_hda_codec_write(codec, 0x43, 0,
						    CM9825_VERB_SET_DACL, val);
				snd_hda_sequence_write(spec->codec,
						       cm9825_arh171_hp_playback_start_verbs);
			}
		} else if (codec->core.subsystem_id == QUIRK_GENE_TWL7_SSID) {
			snd_hda_sequence_write(spec->codec,
					       cm9825_gene_twl7_playback_start_verbs);
		}
		break;
	case HDA_GEN_PCM_ACT_CLEANUP:
		spec->playback_started = 0;
		if (codec->core.subsystem_id == QUIRK_ARH171_SSID) {
			if (!spec->gen.hp_jack_present) {
				hinfo->nid = (hda_nid_t) 0x31;
				val =
				    snd_hda_codec_read(codec, 0x43, 0, 0xfa0,
						       0x0);
				val = (val >> 24) & 0xab;	// <6,4,2>=0
				snd_hda_codec_write(codec, 0x43, 0,
						    CM9825_VERB_SET_DACL, val);
				snd_hda_sequence_write(spec->codec,
						       cm9825_arh171_spk_playback_stop_verbs);
			} else {
				hinfo->nid = (hda_nid_t) 0x30;
				val =
				    snd_hda_codec_read(codec, 0x43, 0, 0xfa0,
						       0x0);
				val = (val >> 24) & 0x57;	// <7,5,3>=0
				snd_hda_codec_write(codec, 0x43, 0,
						    CM9825_VERB_SET_DACL, val);
				snd_hda_sequence_write(spec->codec,
						       cm9825_arh171_hp_playback_stop_verbs);
			}
		} else if (codec->core.subsystem_id == QUIRK_GENE_TWL7_SSID) {
			snd_hda_sequence_write(spec->codec,
					       cm9825_gene_twl7_playback_stop_verbs);
		}
		break;
	default:
		return;
	}
}

static void cm9825_capture_pcm_hook(struct hda_pcm_stream *hinfo,
				    struct hda_codec *codec,
				    struct snd_pcm_substream *substream,
				    int action)
{
	struct cmi_spec *spec = codec->spec;

	switch (action) {
	case HDA_GEN_PCM_ACT_PREPARE:
		spec->capture_started = 1;
		if (codec->core.subsystem_id == QUIRK_ASL051_SSID) {
			if (spec->retasking_line == 1)
				asl051_retasking_linein(codec, 1);

			if (spec->retasking_hp == 1)
				asl051_retasking_mic(codec, 1);
		}
		if (codec->core.subsystem_id == QUIRK_ARH171_SSID) {
			if (spec->chip_0x37_present_verbs)
				snd_hda_sequence_write(spec->codec,
						       spec->chip_0x37_present_verbs);
		}
		break;
	case HDA_GEN_PCM_ACT_CLEANUP:
		spec->capture_started = 0;
		break;
	default:
		return;
	}
}

static int cm9825_init(struct hda_codec *codec)
{
	snd_hda_gen_init(codec);
	snd_hda_apply_fixup(codec, HDA_FIXUP_ACT_INIT);

	return 0;
}

static void cm9825_remove(struct hda_codec *codec)
{
	struct cmi_spec *spec = codec->spec;

	cancel_delayed_work_sync(&spec->unsol_hp_work);
	cancel_delayed_work_sync(&spec->unsol_line_work);
	snd_hda_gen_remove(codec);
}

static int cm9825_ncr_resume(struct hda_codec *codec)
{
	snd_hda_regmap_sync(codec);
	hda_call_check_power_status(codec, 0x01);

	return 0;
}

static int cm9825_ncr_suspend(struct hda_codec *codec)
{
	struct cmi_spec *spec = codec->spec;

	snd_hda_sequence_write(codec, spec->chip_d3_verbs);

	return 0;
}

static int cm9825_suspend(struct hda_codec *codec)
{
	struct cmi_spec *spec = codec->spec;

	if (codec->core.subsystem_id == QUIRK_NCR_SSID) {
		cm9825_ncr_suspend(codec);
		return 0;
	}

	cancel_delayed_work_sync(&spec->unsol_hp_work);

	snd_hda_sequence_write(codec, spec->chip_d3_verbs);

	if (codec->core.subsystem_id == QUIRK_ASL051_SSID) {
		asl051_retasking_bias1_sw(codec, 0);
		asl051_retasking_bias2_sw(codec, 0);

		cancel_delayed_work_sync(&spec->unsol_line_work);

		//link reset
		snd_hda_codec_write(codec, 0x3a, 0, AC_VERB_SET_CONNECT_SEL,
				    0x02);
		snd_hda_codec_write(codec, 0x40, 0, AC_VERB_SET_CONNECT_SEL,
				    0x40);
	} else if (codec->core.subsystem_id == QUIRK_ARH171_SSID
		   || codec->core.subsystem_id == QUIRK_GENE_TWL7_SSID) {
		cancel_delayed_work_sync(&spec->unsol_line_work);
	}

	return 0;
}

static int cm9825_resume(struct hda_codec *codec)
{
	struct cmi_spec *spec = codec->spec;
	hda_nid_t hp_pin = 0;
	bool hp_jack_plugin = false;
	bool line_jack_plugin = false;
	int err;

	if (codec->core.subsystem_id == QUIRK_NCR_SSID) {
		cm9825_ncr_resume(codec);
		return 0;
	}

	err =
	    snd_hda_codec_write(spec->codec, 0x42, 0,
				AC_VERB_SET_PIN_WIDGET_CONTROL, 0x00);
	if (err)
		codec_dbg(codec, "codec_write err %d\n", err);

	msleep(150);		/* for depop noise */

	snd_hda_sequence_write(codec, spec->chip_d0_verbs);

	hp_pin = spec->gen.autocfg.hp_pins[0];
	hp_jack_plugin = snd_hda_jack_detect(spec->codec, hp_pin);

	codec_dbg(spec->codec, "hp_jack_plugin %d, hp_pin 0x%X\n",
		  (int)hp_jack_plugin, hp_pin);

	if (!hp_jack_plugin) {
		err =
		    snd_hda_codec_write(spec->codec, 0x42, 0,
					AC_VERB_SET_PIN_WIDGET_CONTROL, 0x40);

		if (err)
			codec_dbg(codec, "codec_write err %d\n", err);

		if (codec->core.subsystem_id != QUIRK_ASL051_SSID)
			if (spec->chip_0x36_remove_verbs != NULL)
				snd_hda_sequence_write(codec,
						       spec->chip_0x36_remove_verbs);
	}

	if (codec->core.subsystem_id == QUIRK_ASL051_SSID) {
		if (!spec->retasking_line)
			asl051_retasking_bias1_sw(codec, 0);

		if (!spec->retasking_hp)
			asl051_retasking_bias2_sw(codec, 0);

		line_jack_plugin = snd_hda_jack_detect(spec->codec, 0x3b);

		if (!line_jack_plugin) {
			if (spec->retasking_line == 1)
				asl051_retasking_linein(spec->codec, 0);
			else
				asl051_retasking_lineout(spec->codec, 0);

			if (spec->retasking_hp == 1)
				asl051_retasking_mic(spec->codec, 0);
			else
				asl051_retasking_hp(spec->codec, 0);
		}
	}

	if (codec->core.subsystem_id == QUIRK_ARH171_SSID) {
		line_jack_plugin = snd_hda_jack_detect(spec->codec, 0x3b);

		if (spec->chip_0x3b_present_verbs != NULL) {
			if (line_jack_plugin)
				snd_hda_sequence_write(codec,
						       spec->chip_0x3b_present_verbs);
			else
				snd_hda_sequence_write(codec,
						       spec->chip_0x3b_remove_verbs);
		}
	}

	snd_hda_regmap_sync(codec);
	hda_call_check_power_status(codec, 0x01);

	return 0;
}

static u32 get_amp_max_value(struct hda_codec *codec, hda_nid_t nid, int dir,
			     unsigned int ofs)
{
	u32 caps = query_amp_caps(codec, nid, dir);
	/* get num steps */
	caps = (caps & AC_AMPCAP_NUM_STEPS) >> AC_AMPCAP_NUM_STEPS_SHIFT;
	if (ofs < caps)
		caps -= ofs;
	return caps;
}

static inline int
update_amp_value(struct hda_codec *codec, hda_nid_t nid,
		 int ch, int dir, int idx, unsigned int ofs, unsigned int val)
{
	unsigned int maxval;

	if (val > 0)
		val += ofs;
	/* ofs = 0: raw max value */
	maxval = get_amp_max_value(codec, nid, dir, 0);
	if (val > maxval)
		val = maxval;
	return snd_hda_codec_amp_update(codec, nid, ch, dir, idx,
					HDA_AMP_VOLMASK, val);
}

static int cm9825_ncr_spk_vol_put(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	hda_nid_t nid = get_amp_nid(kcontrol);
	int chs = get_amp_channels(kcontrol);
	int dir = get_amp_direction(kcontrol);
	int idx = get_amp_index(kcontrol);
	unsigned int ofs = get_amp_offset(kcontrol);
	long *valp = ucontrol->value.integer.value;
	int change = 0;

	codec_dbg(codec, "nid 0x%X, chs %d, dir %d, *valp %ld\n",
		  nid, chs, dir, *valp);

	if (chs & 1) {
		change = update_amp_value(codec, nid, 0, dir, idx, ofs, *valp);
		update_amp_value(codec, 0x38, 0, dir, idx, ofs, *valp);
		valp++;
	}
	if (chs & 2) {
		change |= update_amp_value(codec, nid, 1, dir, idx, ofs, *valp);
		update_amp_value(codec, 0x38, 1, dir, idx, ofs, *valp);
	}
	return change;
}

static int cm9825_ncr_switch_put(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	hda_nid_t nid = get_amp_nid(kcontrol);
	int chs = get_amp_channels(kcontrol);
	int dir = get_amp_direction(kcontrol);
	int idx = get_amp_index(kcontrol);
	long *valp = ucontrol->value.integer.value;
	int change = 0;

	codec_dbg(codec, "nid 0x%X, chs %d, dir %d, *valp %ld\n",
		  nid, chs, dir, *valp);

	if (chs & 1) {
		change = snd_hda_codec_amp_update(codec, nid, 0, dir, idx,
						  HDA_AMP_MUTE,
						  *valp ? 0 : HDA_AMP_MUTE);
		snd_hda_codec_amp_update(codec, 0x38, 0, dir, idx,
					 HDA_AMP_MUTE,
					 *valp ? 0 : HDA_AMP_MUTE);
		valp++;
	}
	if (chs & 2) {
		change |= snd_hda_codec_amp_update(codec, nid, 1, dir, idx,
						   HDA_AMP_MUTE,
						   *valp ? 0 : HDA_AMP_MUTE);
		snd_hda_codec_amp_update(codec, 0x38, 1, dir, idx,
					 HDA_AMP_MUTE,
					 *valp ? 0 : HDA_AMP_MUTE);
	}
	hda_call_check_power_status(codec, nid);
	return change;
}

#define CM9825_NCR_CODEC_VOL(xname, nid, channel, dir) \
	{ .iface = SNDRV_CTL_ELEM_IFACE_MIXER, \
	  .name = xname, \
	  .subdevice = HDA_SUBDEV_AMP_FLAG, \
	  .access = SNDRV_CTL_ELEM_ACCESS_READWRITE | \
			SNDRV_CTL_ELEM_ACCESS_TLV_READ | \
			SNDRV_CTL_ELEM_ACCESS_TLV_CALLBACK, \
	  .info = snd_hda_mixer_amp_volume_info, \
	  .get = snd_hda_mixer_amp_volume_get, \
	  .put = cm9825_ncr_spk_vol_put, \
	  .tlv = { .c = snd_hda_mixer_amp_tlv }, \
	  .private_value = HDA_COMPOSE_AMP_VAL(nid, channel, 0, dir) }

#define CM9825_NCR_CODEC_MUTE(xname, nid, channel, dir) \
	{ .iface = SNDRV_CTL_ELEM_IFACE_MIXER, \
	  .name = xname, \
	  .subdevice = HDA_SUBDEV_AMP_FLAG, \
	  .info = snd_hda_mixer_amp_switch_info, \
	  .get = snd_hda_mixer_amp_switch_get, \
	  .put = cm9825_ncr_switch_put, \
	  .private_value = HDA_COMPOSE_AMP_VAL(nid, channel, 0, dir) }

static const struct snd_kcontrol_new cm9825_ncr_mixer[] = {
	CM9825_NCR_CODEC_VOL("Master Playback Volume", 0x3c, 3, HDA_OUTPUT),
	CM9825_NCR_CODEC_MUTE("Master Playback Switch", 0x3c, 3, HDA_OUTPUT),
	{}
};

static int asl051_retasking_hp_ctl_info(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 1;
	return 0;
}

static int asl051_retasking_hp_ctl_get(struct snd_kcontrol *kcontrol,
				       struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	struct cmi_spec *spec = codec->spec;

	ucontrol->value.integer.value[0] = spec->retasking_hp;

	return 0;
}

static int asl051_retasking_hp_ctl_put(struct snd_kcontrol *kcontrol,
				       struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	struct cmi_spec *spec = codec->spec;

	spec->retasking_hp = ucontrol->value.integer.value[0];

	if (spec->retasking_hp == 1)
		asl051_retasking_mic(codec, 1);
	else
		asl051_retasking_hp(codec, 1);

	snd_hda_gen_update_outputs(spec->codec);

	return 0;
}

static int asl051_retasking_line_ctl_info(struct snd_kcontrol *kcontrol,
					  struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 1;
	return 0;
}

static int asl051_retasking_line_ctl_get(struct snd_kcontrol *kcontrol,
					 struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	struct cmi_spec *spec = codec->spec;

	ucontrol->value.integer.value[0] = spec->retasking_line;

	return 0;
}

static int asl051_retasking_line_ctl_put(struct snd_kcontrol *kcontrol,
					 struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	struct cmi_spec *spec = codec->spec;

	spec->retasking_line = ucontrol->value.integer.value[0];

	if (spec->retasking_line)
		asl051_retasking_linein(codec, 1);
	else
		asl051_retasking_lineout(codec, 1);

	snd_hda_gen_update_outputs(spec->codec);

	return 0;
}

#define ASL051_RETASKING_HP_CTRL(xname) \
	{ .iface = SNDRV_CTL_ELEM_IFACE_MIXER, \
	  .name = xname, \
	  .subdevice = HDA_SUBDEV_NID_FLAG, \
	  .access = SNDRV_CTL_ELEM_ACCESS_READWRITE, \
	  .info = asl051_retasking_hp_ctl_info, \
	  .get = asl051_retasking_hp_ctl_get, \
	  .put = asl051_retasking_hp_ctl_put, \
	  .private_value = AC_JACK_HP_OUT }

#define ASL051_RETASKING_LINE_CTRL(xname) \
	{ .iface = SNDRV_CTL_ELEM_IFACE_MIXER, \
	  .name = xname, \
	  .subdevice = HDA_SUBDEV_NID_FLAG, \
	  .access = SNDRV_CTL_ELEM_ACCESS_READWRITE, \
	  .info = asl051_retasking_line_ctl_info, \
	  .get = asl051_retasking_line_ctl_get, \
	  .put = asl051_retasking_line_ctl_put, \
	  .private_value = AC_JACK_LINE_OUT }

static const struct snd_kcontrol_new cm9825_asl051_ctl[] = {
	ASL051_RETASKING_HP_CTRL("ASL051 HP Retasking"),
	ASL051_RETASKING_LINE_CTRL("ASL051 LINE Retasking"),
	{}
};

static int cm9825_probe(struct hda_codec *codec, const struct hda_device_id *id)
{
	struct cmi_spec *spec;
	struct auto_pin_cfg *cfg;
	int err, i = 0;

	spec = kzalloc(sizeof(*spec), GFP_KERNEL);
	if (spec == NULL)
		return -ENOMEM;

	INIT_DELAYED_WORK(&spec->unsol_hp_work, cm9825_unsol_hp_delayed);
	codec->spec = spec;
	spec->codec = codec;
	cfg = &spec->gen.autocfg;
	spec->gen.init_hook = cm9825_init_hook;
	snd_hda_gen_spec_init(&spec->gen);
	spec->chip_d0_verbs = cm9825_std_d0_verbs;
	spec->chip_d3_verbs = cm9825_std_d3_verbs;
	spec->chip_0x36_present_verbs = cm9825_hp_present_verbs;
	spec->chip_0x36_remove_verbs = cm9825_hp_remove_verbs;

	codec_info(codec, "subsystem_id: 0x%X\n", codec->core.subsystem_id);

	switch (codec->core.subsystem_id) {
	case QUIRK_CM_STD:
		snd_hda_codec_set_name(codec, "CM9825 STD");
		spec->chip_d0_verbs = cm9825_std_d0_verbs;
		spec->chip_d3_verbs = cm9825_std_d3_verbs;
		spec->chip_0x36_present_verbs = cm9825_hp_present_verbs;
		spec->chip_0x36_remove_verbs = cm9825_hp_remove_verbs;
		break;
	case QUIRK_NCR_SSID:
		snd_hda_codec_set_name(codec, "CM9825 NCR");
		spec->gen.pcm_playback_hook = cm9825_playback_pcm_hook;
		spec->chip_d0_verbs = cm9825_ncr_d0_verbs;
		spec->chip_d3_verbs = cm9825_ncr_d3_verbs;
		spec->chip_playback_start_verbs =
		    cm9825_ncr_playback_start_verbs;
		spec->chip_playback_stop_verbs = cm9825_ncr_playback_stop_verbs;

		for (i = 0; i < ARRAY_SIZE(cm9825_ncr_mixer); i++) {
			err = snd_hda_add_new_ctls(codec, &cm9825_ncr_mixer[i]);
			if (err < 0) {
				codec_info(codec, "add new ctls fail: %d\n",
					   err);
				goto error;
			}
		}
		break;
	case QUIRK_IBP_SSID:
		snd_hda_codec_set_name(codec, "CM9825 IBP");
		spec->gen.pcm_playback_hook = cm9825_playback_pcm_hook;
		spec->chip_d0_verbs = cm9825_ibp_d0_verbs;
		spec->chip_d3_verbs = cm9825_ibp_d3_verbs;
		spec->chip_0x36_present_verbs = cm9825_ibp_hp_present_verbs;
		spec->chip_0x36_remove_verbs = cm9825_ibp_hp_remove_verbs;
		spec->chip_playback_start_verbs =
		    cm9825_ibp_playback_start_verbs;
		spec->chip_playback_stop_verbs = cm9825_ibp_playback_stop_verbs;
		spec->gen.autocfg.hp_pins[0] =
		    spec->gen.autocfg.line_out_pins[0];
		break;
	case QUIRK_ASL051_SSID:
		snd_hda_codec_set_name(codec, "CM9825 ASL051");
		INIT_DELAYED_WORK(&spec->unsol_line_work,
				  cm9825_unsol_line_delayed);
		spec->gen.suppress_vmaster = 1;
		spec->gen.indep_hp = 1;
		spec->gen.indep_hp_enabled = 1;
		spec->gen.dyn_adc_switch = 1;
		spec->gen.detect_lo = 1;
		spec->gen.detect_hp = 1;
		spec->gen.automute_lo = 1;
		spec->gen.automute_speaker = 1;
		spec->gen.multiout.hp_out_nid[0] = 0x30;
		spec->gen.alt_dac_nid = 0x30;
		spec->gen.multiout = asl051_multi_out;
		spec->gen.pcm_playback_hook = cm9825_playback_pcm_hook;
		spec->gen.pcm_capture_hook = cm9825_capture_pcm_hook;
		spec->chip_d0_verbs = cm9825_asl051_d0_verbs;
		spec->chip_d3_verbs = cm9825_asl051_d3_verbs;
		spec->chip_lineout_retasking_trig_verbs =
		    asl051_retasking_lineout_trig_verbs;
		spec->chip_lineout_retasking_remove_verbs =
		    asl051_retasking_lineout_remove_verbs;
		spec->chip_linein_retasking_trig_verbs =
		    asl051_retasking_linein_trig_verbs;
		spec->chip_linein_retasking_remove_verbs =
		    asl051_retasking_linein_remove_verbs;
		spec->chip_hp_retasking_trig_verbs =
		    asl051_retasking_hp_trig_verbs;
		spec->chip_hp_retasking_remove_verbs =
		    asl051_retasking_hp_remove_verbs;
		spec->chip_mic_retasking_trig_verbs =
		    asl051_retasking_mic_trig_verbs;
		spec->chip_mic_retasking_remove_verbs =
		    asl051_retasking_mic_remove_verbs;

		for (int i = 0; i < ARRAY_SIZE(cm9825_asl051_ctl); i++) {
			err =
			    snd_hda_add_new_ctls(codec, &cm9825_asl051_ctl[i]);
			if (err < 0) {
				codec_info(codec, "add new ctls fail: %d\n",
					   err);
				goto error;
			}
		}

		snd_hda_jack_set_gating_jack(codec, 0x37, 0x36);
		snd_hda_jack_set_gating_jack(codec, 0x34, 0x3b);
		break;
	case QUIRK_ARH171_SSID:
		snd_hda_codec_set_name(codec, "CM9825 ARH171");
		INIT_DELAYED_WORK(&spec->unsol_line_work,
				  cm9825_unsol_line_delayed);
		spec->gen.autocfg.hp_pins[0] = 0x36;
		spec->gen.autocfg.hp_outs = 1;
		spec->gen.automute_speaker = 1;
		spec->gen.autocfg.num_inputs = 1;
		cfg->inputs[0].pin = 0x3b;
		cfg->inputs[0].type = AUTO_PIN_MIC;
		cfg->inputs[0].is_headphone_mic = 1;
		hda_nid_t dac_nids[] = { 0x31 };

		spec->gen.multiout.dac_nids = dac_nids;
		spec->gen.multiout.num_dacs = 1;
		spec->gen.multiout.hp_nid = 0x30;
		spec->gen.indep_hp = 1;
		spec->gen.indep_hp_enabled = 1;
		spec->gen.out_vol_mask |= (1ULL << 0x3c) | (1ULL << 0x38);
		spec->gen.num_adc_nids = 1;
		spec->gen.adc_nids[0] = 0x33;
		spec->gen.pcm_playback_hook = cm9825_playback_pcm_hook;
		spec->gen.pcm_capture_hook = cm9825_capture_pcm_hook;
		spec->chip_d0_verbs = cm9825_arh171_d0_verbs;
		spec->chip_d3_verbs = cm9825_arh171_d3_verbs;
		spec->chip_0x36_present_verbs = cm9825_arh171_hp_present_verbs;
		spec->chip_0x36_remove_verbs = cm9825_arh171_hp_remove_verbs;
		spec->chip_0x3b_present_verbs =
		    cm9825_arh171_micin_present_verbs;
		spec->chip_0x3b_remove_verbs = cm9825_arh171_micin_remove_verbs;
		spec->chip_d0_verbs = cm9825_arh171_d0_verbs;
		spec->chip_d3_verbs = cm9825_arh171_d3_verbs;

		for (i = 0; i < ARRAY_SIZE(arh171_mixer); i++) {
			err = snd_hda_add_new_ctls(codec, &arh171_mixer[i]);
			if (err < 0) {
				codec_info(codec, "add new ctls fail: %d\n",
					   err);
				goto error;
			}
		}
		break;
	case QUIRK_GENE_TWL7_SSID:
		snd_hda_codec_set_name(codec, "CM9825 GENE_TWL7");
		INIT_DELAYED_WORK(&spec->unsol_line_work,
				  cm9825_unsol_line_delayed);
		spec->gen.hp_mic = 0;
		cfg->line_outs = 1;
		cfg->line_out_pins[0] = 0x36;
		cfg->line_out_type = AUTO_PIN_LINE_OUT;
		cfg->num_inputs = 2;
		cfg->inputs[0].pin = 0x3b;
		cfg->inputs[0].type = AUTO_PIN_LINE_IN;
		cfg->inputs[1].pin = 0x37;
		cfg->inputs[1].type = AUTO_PIN_MIC;
		cfg->inputs[1].is_headphone_mic = 1;
		spec->chip_d0_verbs = cm9825_gene_twl7_d0_verbs;
		spec->chip_d3_verbs = cm9825_gene_twl7_d3_verbs;
		spec->gen.pcm_playback_hook = cm9825_playback_pcm_hook;
		snd_hda_codec_set_pincfg(codec, 0x37, 0x24A70100);
		break;
	default:
		spec->chip_d0_verbs = cm9825_std_d0_verbs;
		spec->chip_d3_verbs = cm9825_std_d3_verbs;
		spec->chip_0x36_present_verbs = cm9825_hp_present_verbs;
		spec->chip_0x36_remove_verbs = cm9825_hp_remove_verbs;
		break;
	}

	snd_hda_sequence_write(codec, spec->chip_d0_verbs);

	err =
	    snd_hda_parse_pin_defcfg(codec, cfg, NULL,
				     codec->core.subsystem_id ==
				     QUIRK_ASL051_SSID ?
				     HDA_PINCFG_HEADPHONE_MIC : 0);
	if (err < 0)
		goto error;
	err = snd_hda_gen_parse_auto_config(codec, cfg);
	if (err < 0)
		goto error;

	cm9825_setup_unsol(codec);

	return 0;

 error:
	cm9825_remove(codec);

	codec_info(codec, "Enter err %d\n", err);

	return err;
}

static const struct hda_codec_ops cm9825_codec_ops = {
	.probe = cm9825_probe,
	.remove = cm9825_remove,
	.build_controls = snd_hda_gen_build_controls,
	.build_pcms = snd_hda_gen_build_pcms,
	.init = cm9825_init,
	.unsol_event = snd_hda_jack_unsol_event,
	.suspend = cm9825_suspend,
	.resume = cm9825_resume,
	.check_power_status = snd_hda_gen_check_power_status,
	.stream_pm = snd_hda_gen_stream_pm,
};

/*
 * driver entries
 */
static const struct hda_device_id snd_hda_id_cm9825[] = {
	HDA_CODEC_ID(0x13f69825, "CM9825"),
	{}			/* terminator */
};

MODULE_DEVICE_TABLE(hdaudio, snd_hda_id_cm9825);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("CM9825 HD-audio codec");

static struct hda_codec_driver cm9825_driver = {
	.id = snd_hda_id_cm9825,
	.ops = &cm9825_codec_ops,
};

module_hda_codec_driver(cm9825_driver);
