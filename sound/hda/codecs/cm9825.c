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
	QUIRK_GENE_TWL7_SSID = 0x160dc000
};

/* CM9825 Offset Definitions */

#define CM9825_VERB_SET_HPF_1 0x781
#define CM9825_VERB_SET_HPF_2 0x785
#define CM9825_VERB_SET_PLL 0x7a0
#define CM9825_VERB_SET_NEG 0x7a1
#define CM9825_VERB_SET_ADCL 0x7a2
#define CM9825_VERB_SET_DACL 0x7a3
#define CM9825_VERB_SET_MBIAS 0x7a4
#define CM9825_VERB_SET_VNEG 0x7a8
#define CM9825_VERB_SET_D2S 0x7a9
#define CM9825_VERB_SET_DACTRL 0x7aa
#define CM9825_VERB_SET_P3BCP 0x7ab
#define CM9825_VERB_SET_PDNEG 0x7ac
#define CM9825_VERB_SET_VDO 0x7ad
#define CM9825_VERB_SET_CDALR 0x7b0
#define CM9825_VERB_SET_MTCBA 0x7b1
#define CM9825_VERB_SET_OTP 0x7b2
#define CM9825_VERB_SET_OCP 0x7b3
#define CM9825_VERB_SET_GAD 0x7b4
#define CM9825_VERB_SET_TMOD 0x7b5
#define CM9825_VERB_SET_SNR 0x7b6

struct cmi_spec {
	struct hda_gen_spec gen;
	const struct hda_verb *chip_d0_verbs;
	const struct hda_verb *chip_d3_verbs;
	const struct hda_verb *chip_hp_present_verbs;
	const struct hda_verb *chip_hp_remove_verbs;
	struct hda_codec *codec;
	struct delayed_work unsol_linein_work;
	struct delayed_work unsol_lineout_work;
	struct delayed_work unsol_hp_work;
	int quirk;
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

/*
 * To save power, AD/CLK is turned off.
 */
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

/*
 * Enable CLK/ADC to start recording.
 */
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
	{0x46, CM9825_VERB_SET_P3BCP, 0x20},
	{}
};

/*
 * Enable DAC to start playback.
 */
static const struct hda_verb cm9825_gene_twl7_playback_start_verbs[] = {
	{0x43, CM9825_VERB_SET_D2S, 0xf2},
	{0x43, CM9825_VERB_SET_VDO, 0xd4},
	{0x43, CM9825_VERB_SET_SNR, 0x30},
	{}
};

/*
 * Disable DAC and enable de-pop noise mechanism.
 */
static const struct hda_verb cm9825_gene_twl7_playback_stop_verbs[] = {
	{0x43, CM9825_VERB_SET_VDO, 0xc0},
	{0x43, CM9825_VERB_SET_D2S, 0x62},
	{0x43, CM9825_VERB_SET_VDO, 0xd0},
	{0x43, CM9825_VERB_SET_SNR, 0x38},
	{}
};

static void cm9825_unsol_hp_delayed(struct work_struct *work)
{
	struct cmi_spec *spec =
	    container_of(to_delayed_work(work), struct cmi_spec, unsol_hp_work);
	struct hda_jack_tbl *jack;
	hda_nid_t hp_pin = spec->gen.autocfg.hp_pins[0];
	bool hp_jack_plugin = false;
	int err = 0;

	hp_jack_plugin = snd_hda_jack_detect(spec->codec, hp_pin);

	codec_dbg(spec->codec, "hp_jack_plugin %d, hp_pin 0x%X\n",
		  (int)hp_jack_plugin, hp_pin);

	if (!hp_jack_plugin) {
		err =
		    snd_hda_codec_write(spec->codec, 0x42, 0,
					AC_VERB_SET_PIN_WIDGET_CONTROL, 0x40);
		if (err)
			codec_dbg(spec->codec, "codec_write err %d\n", err);

		snd_hda_sequence_write(spec->codec, spec->chip_hp_remove_verbs);
	} else {
		snd_hda_sequence_write(spec->codec,
				       spec->chip_hp_present_verbs);
	}

	jack = snd_hda_jack_tbl_get(spec->codec, hp_pin);
	if (jack) {
		jack->block_report = 0;
		snd_hda_jack_report_sync(spec->codec);
	}
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

static void cm9825_unsol_linein_delayed(struct work_struct *work)
{
	struct cmi_spec *spec =
	    container_of(to_delayed_work(work), struct cmi_spec,
			 unsol_linein_work);
	struct hda_jack_tbl *jack;

	hda_nid_t linein_pin = spec->gen.autocfg.inputs[1].pin;

	bool linein_jack_plugin = false;

	linein_jack_plugin = snd_hda_jack_detect(spec->codec, linein_pin);

	codec_dbg(spec->codec,
		    "%s, lineout_jack_plugin %d, linein_pin 0x%X, line%d\n",
		    __func__, (int)linein_jack_plugin, linein_pin, __LINE__);

	jack = snd_hda_jack_tbl_get(spec->codec, linein_pin);

	if (jack) {
		jack->block_report = 0;
		snd_hda_jack_report_sync(spec->codec);
	}
}

static void linein_callback(struct hda_codec *codec,
			    struct hda_jack_callback *cb)
{
	struct cmi_spec *spec = codec->spec;
	struct hda_jack_tbl *tbl;

	/* Delay enabling the line, to let the linein-detection
	 * state machine run.
	 */

	codec_dbg(codec, "%s, cb->nid 0x%X, line%d\n", __func__,
		    (int)cb->nid, __LINE__);

	tbl = snd_hda_jack_tbl_get(codec, cb->nid);
	if (tbl)
		tbl->block_report = 1;
	schedule_delayed_work(&spec->unsol_linein_work, msecs_to_jiffies(200));
}

static void cm9825_unsol_lineout_delayed(struct work_struct *work)
{
	struct cmi_spec *spec =
	    container_of(to_delayed_work(work), struct cmi_spec,
			 unsol_lineout_work);
	struct hda_jack_tbl *jack;

	hda_nid_t lineout_pin = spec->gen.autocfg.line_out_pins[0];

	bool lineout_jack_plugin = false;

	lineout_jack_plugin = snd_hda_jack_detect(spec->codec, lineout_pin);

	codec_dbg(spec->codec,
		  "%s, lineout_jack_plugin %d, lineout_pin 0x%X, line%d\n",
		  __func__, (int)lineout_jack_plugin, lineout_pin, __LINE__);

	jack = snd_hda_jack_tbl_get(spec->codec, lineout_pin);

	if (jack) {
		jack->block_report = 0;
		snd_hda_jack_report_sync(spec->codec);
	}
}

static void lineout_callback(struct hda_codec *codec,
			     struct hda_jack_callback *cb)
{
	struct cmi_spec *spec = codec->spec;
	struct hda_jack_tbl *tbl;

	/* Delay enabling the line, to let the linein-detection
	 * state machine run.
	 */

	codec_dbg(codec, "%s, cb->nid 0x%X, line%d\n", __func__,
		  (int)cb->nid, __LINE__);

	tbl = snd_hda_jack_tbl_get(codec, cb->nid);
	if (tbl)
		tbl->block_report = 1;
	schedule_delayed_work(&spec->unsol_lineout_work, msecs_to_jiffies(200));
}

static void cm9825_setup_unsol(struct hda_codec *codec)
{
	struct cmi_spec *spec = codec->spec;

	hda_nid_t hp_pin = spec->gen.autocfg.hp_pins[0];

	snd_hda_jack_detect_enable_callback(codec, hp_pin, hp_callback);
}

static void cm9825_gene_twl7_setup_unsol(struct hda_codec *codec)
{
	struct cmi_spec *spec = codec->spec;

	hda_nid_t lineout_pin = spec->gen.autocfg.line_out_pins[0];

	hda_nid_t lineint_pin = spec->gen.autocfg.inputs[0].pin;

	codec_dbg(codec, "%s, lineout_pin 0x%X, lineint_pin 0x%X, line%d\n",
		    __func__, lineout_pin, lineint_pin, __LINE__);

	snd_hda_jack_detect_enable_callback(codec, lineout_pin,
					    lineout_callback);
	snd_hda_jack_detect_enable_callback(codec, lineint_pin,
					    linein_callback);
}

static void cm9825_playback_pcm_hook(struct hda_pcm_stream *hinfo,
				     struct hda_codec *codec,
				     struct snd_pcm_substream *substream,
				     int action)
{
	struct cmi_spec *spec = codec->spec;

	switch (action) {
	case HDA_GEN_PCM_ACT_PREPARE:
		snd_hda_sequence_write(spec->codec,
				       cm9825_gene_twl7_playback_start_verbs);
		break;
	case HDA_GEN_PCM_ACT_CLEANUP:
		snd_hda_sequence_write(spec->codec,
				       cm9825_gene_twl7_playback_stop_verbs);
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

static void cm9825_cm_std_remove(struct hda_codec *codec)
{
	struct cmi_spec *spec = codec->spec;

	cancel_delayed_work_sync(&spec->unsol_hp_work);
}

static void cm9825_gene_twl7_remove(struct hda_codec *codec)
{
	struct cmi_spec *spec = codec->spec;

	cancel_delayed_work_sync(&spec->unsol_linein_work);
	cancel_delayed_work_sync(&spec->unsol_lineout_work);
}

static void cm9825_remove(struct hda_codec *codec)
{
	if (codec->core.subsystem_id == QUIRK_CM_STD)
		cm9825_cm_std_remove(codec);
	else if (codec->core.subsystem_id == QUIRK_GENE_TWL7_SSID)
		cm9825_gene_twl7_remove(codec);

	snd_hda_gen_remove(codec);
}

static void cm9825_cm_std_suspend(struct hda_codec *codec)
{
	struct cmi_spec *spec = codec->spec;

	cancel_delayed_work_sync(&spec->unsol_hp_work);
}

static void cm9825_gene_twl7_suspend(struct hda_codec *codec)
{
	struct cmi_spec *spec = codec->spec;

	cancel_delayed_work_sync(&spec->unsol_linein_work);
	cancel_delayed_work_sync(&spec->unsol_lineout_work);
}

static int cm9825_suspend(struct hda_codec *codec)
{
	struct cmi_spec *spec = codec->spec;

	if (codec->core.subsystem_id == QUIRK_CM_STD)
		cm9825_cm_std_suspend(codec);
	else if (codec->core.subsystem_id == QUIRK_GENE_TWL7_SSID)
		cm9825_gene_twl7_suspend(codec);

	snd_hda_sequence_write(codec, spec->chip_d3_verbs);

	return 0;
}

static int cm9825_cm_std_resume(struct hda_codec *codec)
{
	struct cmi_spec *spec = codec->spec;
	hda_nid_t hp_pin = 0;
	bool hp_jack_plugin = false;
	int err;

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

		snd_hda_sequence_write(codec, cm9825_hp_remove_verbs);
	}

	return 0;
}

static int cm9825_resume(struct hda_codec *codec)
{
	struct cmi_spec *spec = codec->spec;

	if (codec->core.subsystem_id == QUIRK_CM_STD)
		cm9825_cm_std_resume(codec);
	else if (codec->core.subsystem_id == QUIRK_GENE_TWL7_SSID)
		snd_hda_sequence_write(codec, spec->chip_d0_verbs);

	snd_hda_regmap_sync(codec);
	hda_call_check_power_status(codec, 0x01);

	return 0;
}

static int cm9825_probe(struct hda_codec *codec, const struct hda_device_id *id)
{
	struct cmi_spec *spec;
	struct auto_pin_cfg *cfg;
	int err = 0;
	unsigned int val = 0;

	spec = kzalloc(sizeof(*spec), GFP_KERNEL);
	if (spec == NULL)
		return -ENOMEM;

	codec_dbg(codec, "chip_name: %s, ssid: 0x%X\n",
		  codec->core.chip_name, codec->core.subsystem_id);

	codec->spec = spec;
	spec->codec = codec;
	cfg = &spec->gen.autocfg;
	snd_hda_gen_spec_init(&spec->gen);
	spec->chip_d0_verbs = cm9825_std_d0_verbs;
	spec->chip_d3_verbs = cm9825_std_d3_verbs;
	spec->chip_hp_present_verbs = cm9825_hp_present_verbs;
	spec->chip_hp_remove_verbs = cm9825_hp_remove_verbs;

	switch (codec->core.subsystem_id) {
	case QUIRK_CM_STD:
		snd_hda_codec_set_name(codec, "CM9825 STD");
		INIT_DELAYED_WORK(&spec->unsol_hp_work,
				  cm9825_unsol_hp_delayed);
		spec->chip_d0_verbs = cm9825_std_d0_verbs;
		spec->chip_d3_verbs = cm9825_std_d3_verbs;
		spec->chip_hp_present_verbs = cm9825_hp_present_verbs;
		spec->chip_hp_remove_verbs = cm9825_hp_remove_verbs;
		cm9825_setup_unsol(codec);
		break;
	case QUIRK_GENE_TWL7_SSID:
		snd_hda_codec_set_name(codec, "CM9825 GENE_TWL7");
		INIT_DELAYED_WORK(&spec->unsol_linein_work,
				  cm9825_unsol_linein_delayed);
		INIT_DELAYED_WORK(&spec->unsol_lineout_work,
				  cm9825_unsol_lineout_delayed);
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
		cm9825_gene_twl7_setup_unsol(codec);
		break;
	default:
		err = -ENXIO;
		break;
	}

	if (err < 0)
		goto error;

	/* Support OMTP Jack */
	val = snd_hda_codec_read(codec, 0x46, 0, 0xfec, 0x0);
	snd_hda_codec_write(codec, 0x46, 0, 0x7ef, (val >> 24) & 0x7f);

	snd_hda_sequence_write(codec, spec->chip_d0_verbs);

	err = snd_hda_parse_pin_defcfg(codec, cfg, NULL, 0);
	if (err < 0)
		goto error;
	err = snd_hda_gen_parse_auto_config(codec, cfg);
	if (err < 0)
		goto error;

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
	{} /* terminator */
};
MODULE_DEVICE_TABLE(hdaudio, snd_hda_id_cm9825);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("CM9825 HD-audio codec");

static struct hda_codec_driver cm9825_driver = {
	.id = snd_hda_id_cm9825,
	.ops = &cm9825_codec_ops,
};

module_hda_codec_driver(cm9825_driver);
