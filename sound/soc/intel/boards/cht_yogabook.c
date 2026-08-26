// SPDX-License-Identifier: GPL-2.0-only
/*
 *  cht_yogabook.c - ASoC machine driver for the Lenovo Yoga Book
 *
 *  Copyright (C) 2019 Yauhen Kharuzhy <jekhor@gmail.com>
 *
 *  Based on cht_bsw_rt5672.c:
 *  Copyright (C) 2014 Intel Corp
 *  Author: Subhransu S. Prusty <subhransu.s.prusty@intel.com>
 *          Mengdong Lin <mengdong.lin@intel.com>
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <sound/jack.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-acpi.h>
#include "../../codecs/rt5677.h"
#include "../../codecs/ts3a227e.h"
#include "../atom/sst-atom-controls.h"

#define RT5677_I2C	"i2c-rt5677"

/* Platform clock 3 provides the codec's 19.2 MHz I2S master clock. */
#define CHT_PLAT_CLK_3_HZ	19200000
#define CHT_CODEC_DAI	"rt5677-aif1"

struct cht_mc_private {
	char codec_name[SND_ACPI_I2C_ID_LEN];
	struct snd_soc_jack jack;
	struct notifier_block jack_nb;
	struct clk *mclk;
	struct gpio_desc *gpio_spk_en1;
	struct gpio_desc *gpio_spk_en2;
	struct gpio_desc *gpio_hp_en;
};

static int platform_clock_control(struct snd_soc_dapm_widget *w,
				  struct snd_kcontrol *k, int event)
{
	struct snd_soc_dapm_context *dapm = w->dapm;
	struct snd_soc_card *card = snd_soc_dapm_to_card(dapm);
	struct snd_soc_dai *codec_dai;
	struct cht_mc_private *ctx = snd_soc_card_get_drvdata(card);
	int ret;

	dev_dbg(card->dev, "Setting platform clock\n");

	codec_dai = snd_soc_card_get_codec_dai(card, CHT_CODEC_DAI);
	if (!codec_dai) {
		dev_err(card->dev, "Codec dai not found; Unable to set platform clock\n");
		return -EIO;
	}

	if (SND_SOC_DAPM_EVENT_ON(event)) {
		if (ctx->mclk) {
			ret = clk_prepare_enable(ctx->mclk);
			if (ret < 0) {
				dev_err(card->dev,
					"could not configure MCLK state");
				return ret;
			}
		}

		/* set codec PLL source to the 19.2MHz platform clock (MCLK) */
		ret = snd_soc_dai_set_pll(codec_dai, 0, RT5677_PLL1_S_MCLK,
					  CHT_PLAT_CLK_3_HZ, 48000 * 512);
		if (ret < 0) {
			dev_err(card->dev, "can't set codec pll: %d\n", ret);
			goto disable_mclk;
		}

		/* set codec sysclk source to PLL */
		ret = snd_soc_dai_set_sysclk(codec_dai, RT5677_SCLK_S_PLL1,
					     48000 * 512, SND_SOC_CLOCK_IN);
		if (ret < 0) {
			dev_err(card->dev, "can't set codec sysclk: %d\n", ret);
			goto disable_mclk;
		}
	} else {
		/*
		 * Set codec sysclk source to its internal clock because codec
		 * PLL will be off when idle and MCLK will also be off by ACPI
		 * when codec is runtime suspended. Codec needs clock for jack
		 * detection and button press.
		 */
		snd_soc_dai_set_sysclk(codec_dai, RT5677_SCLK_S_RCCLK,
				       48000 * 512, SND_SOC_CLOCK_IN);

		if (ctx->mclk)
			clk_disable_unprepare(ctx->mclk);
	}
	return 0;

disable_mclk:
	if (ctx->mclk)
		clk_disable_unprepare(ctx->mclk);
	return ret;
}

static int cht_yb_hp_event(struct snd_soc_dapm_widget *w,
			   struct snd_kcontrol *k, int event)
{
	struct snd_soc_dapm_context *dapm = w->dapm;
	struct snd_soc_card *card = snd_soc_dapm_to_card(dapm);
	struct cht_mc_private *ctx = snd_soc_card_get_drvdata(card);

	dev_dbg(card->dev, "HP event: %s\n",
		SND_SOC_DAPM_EVENT_ON(event) ? "ON" : "OFF");

	if (SND_SOC_DAPM_EVENT_ON(event)) {
		msleep(20);
		gpiod_set_value_cansleep(ctx->gpio_hp_en, 1);
		msleep(50);
	} else {
		gpiod_set_value_cansleep(ctx->gpio_hp_en, 0);
	}

	return 0;
}

static int cht_yb_spk_event(struct snd_soc_dapm_widget *w,
			    struct snd_kcontrol *k, int event)
{
	struct snd_soc_dapm_context *dapm = w->dapm;
	struct snd_soc_card *card = snd_soc_dapm_to_card(dapm);
	struct cht_mc_private *ctx = snd_soc_card_get_drvdata(card);

	dev_dbg(card->dev, "SPK event: %s\n",
		SND_SOC_DAPM_EVENT_ON(event) ? "ON" : "OFF");

	/* Replicate the speaker-enable pulse sequence from Lenovo's kernel. */
	if (SND_SOC_DAPM_EVENT_ON(event)) {
		gpiod_set_value_cansleep(ctx->gpio_spk_en1, 1);
		udelay(2);
		gpiod_set_value_cansleep(ctx->gpio_spk_en1, 0);
		udelay(2);
		gpiod_set_value_cansleep(ctx->gpio_spk_en1, 1);
		udelay(2);
		gpiod_set_value_cansleep(ctx->gpio_spk_en1, 0);
		udelay(2);
	}

	gpiod_set_value_cansleep(ctx->gpio_spk_en1, SND_SOC_DAPM_EVENT_ON(event));
	gpiod_set_value_cansleep(ctx->gpio_spk_en2, SND_SOC_DAPM_EVENT_ON(event));
	msleep(50);

	return 0;
}

static const struct snd_soc_dapm_widget cht_dapm_widgets[] = {
	SND_SOC_DAPM_HP("Headphone", cht_yb_hp_event),
	SND_SOC_DAPM_MIC("Headset Mic", NULL),
	SND_SOC_DAPM_MIC("Int Mic", NULL),
	SND_SOC_DAPM_SPK("Speaker", cht_yb_spk_event),
	SND_SOC_DAPM_SUPPLY("Platform Clock", SND_SOC_NOPM, 0, 0,
			    platform_clock_control, SND_SOC_DAPM_PRE_PMU |
			    SND_SOC_DAPM_POST_PMD),
};

static const struct snd_soc_dapm_route cht_audio_map[] = {
	{"IN1P", NULL, "Headset Mic"},
	{"IN1N", NULL, "Headset Mic"},
	{"DMIC L1", NULL, "Int Mic"},
	{"DMIC R1", NULL, "Int Mic"},
	{"Headphone", NULL, "LOUT1"},
	{"Headphone", NULL, "LOUT2"},
	{"Speaker", NULL, "LOUT1"},
	{"Speaker", NULL, "LOUT2"},

	{"AIF1 Playback", NULL, "ssp2 Tx"},
	{"ssp2 Tx", NULL, "codec_out0"},
	{"ssp2 Tx", NULL, "codec_out1"},
	{"codec_in0", NULL, "ssp2 Rx"},
	{"codec_in1", NULL, "ssp2 Rx"},
	{"ssp2 Rx", NULL, "AIF1 Capture"},
	{"Headphone", NULL, "Platform Clock"},
	{"Speaker", NULL, "Platform Clock"},
	{"Headset Mic", NULL, "Platform Clock"},
	{"Int Mic", NULL, "Platform Clock"},
};

static const struct snd_kcontrol_new cht_mc_controls[] = {
	SOC_DAPM_PIN_SWITCH("Headphone"),
	SOC_DAPM_PIN_SWITCH("Headset Mic"),
	SOC_DAPM_PIN_SWITCH("Int Mic"),
	SOC_DAPM_PIN_SWITCH("Speaker"),
};

static int cht_aif1_hw_params(struct snd_pcm_substream *substream,
			      struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);
	int ret;

	/* set codec PLL source to the 19.2MHz platform clock (MCLK) */
	ret = snd_soc_dai_set_pll(codec_dai, 0, RT5677_PLL1_S_MCLK,
				  CHT_PLAT_CLK_3_HZ, params_rate(params) * 512);
	if (ret < 0) {
		dev_err(rtd->dev, "can't set codec pll: %d\n", ret);
		return ret;
	}

	/* set codec sysclk source to PLL */
	ret = snd_soc_dai_set_sysclk(codec_dai, RT5677_SCLK_S_PLL1,
				     params_rate(params) * 512,
				     SND_SOC_CLOCK_IN);
	if (ret < 0) {
		dev_err(rtd->dev, "can't set codec sysclk: %d\n", ret);
		return ret;
	}
	/*
	 * Default mode for SSP configuration is TDM 4 slot
	 */
	ret = snd_soc_dai_set_fmt(codec_dai,
				  SND_SOC_DAIFMT_DSP_B |
				  SND_SOC_DAIFMT_IB_NF |
				  SND_SOC_DAIFMT_CBC_CFC);
	if (ret < 0) {
		dev_err(codec_dai->dev, "can't set format to TDM %d\n", ret);
		return ret;
	}

	/* TDM 4 slots 24 bit, set Rx & Tx bitmask to 4 active slots */
	ret = snd_soc_dai_set_tdm_slot(codec_dai, 0xF, 0xF, 4, 25);
	if (ret < 0) {
		dev_err(rtd->dev, "can't set codec TDM slot %d\n", ret);
		return ret;
	}

	return 0;
}

static int cht_yb_jack_event(struct notifier_block *nb,
			     unsigned long event, void *data)
{
	struct snd_soc_jack *jack = (struct snd_soc_jack *)data;
	struct snd_soc_dapm_context *dapm = snd_soc_card_to_dapm(jack->card);

	if (event & SND_JACK_MICROPHONE) {
		snd_soc_dapm_force_enable_pin(dapm, "MICBIAS1");
		snd_soc_dapm_sync(dapm);
	} else {
		snd_soc_dapm_disable_pin(dapm, "MICBIAS1");
		snd_soc_dapm_sync(dapm);
	}

	return 0;
}

static int cht_codec_init(struct snd_soc_pcm_runtime *runtime)
{
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(runtime, 0);
	struct snd_soc_component *component = codec_dai->component;
	struct cht_mc_private *ctx = snd_soc_card_get_drvdata(runtime->card);
	struct snd_soc_jack *jack = &ctx->jack;
	int ret;

	/*
	 * Enable codec ASRC function for Stereo DAC/Stereo1 ADC/DMIC/I2S1.
	 * The ASRC clock source is clk_i2s1_asrc.
	 */
	rt5677_sel_asrc_clk_src(component, RT5677_DA_STEREO_FILTER |
					     RT5677_AD_STEREO1_FILTER |
					     RT5677_I2S1_SOURCE,
				     RT5677_CLK_SEL_I2S1_ASRC);
	/*
	 * Enable codec ASRC function for Mono ADC L.
	 * The ASRC clock source is clk_sys2_asrc.
	 */
	rt5677_sel_asrc_clk_src(component, RT5677_AD_MONO_L_FILTER, RT5677_CLK_SEL_SYS2);

	ctx->gpio_spk_en1 = devm_gpiod_get(component->dev, "speaker-enable", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->gpio_spk_en1)) {
		dev_err(component->dev, "Can't find speaker enable GPIO\n");
		return PTR_ERR(ctx->gpio_spk_en1);
	}

	ctx->gpio_spk_en2 = devm_gpiod_get(component->dev, "speaker-enable2", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->gpio_spk_en2)) {
		dev_err(component->dev, "Can't find speaker enable 2 GPIO\n");
		return PTR_ERR(ctx->gpio_spk_en2);
	}

	ctx->gpio_hp_en = devm_gpiod_get(component->dev, "headphone-enable", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->gpio_hp_en)) {
		dev_err(component->dev, "Can't find headphone enable GPIO\n");
		return PTR_ERR(ctx->gpio_hp_en);
	}

	snd_soc_jack_notifier_register(jack, &ctx->jack_nb);

	if (ctx->mclk) {
		/*
		 * The firmware might enable the clock at
		 * boot (this information may or may not
		 * be reflected in the enable clock register).
		 * To change the rate we must disable the clock
		 * first to cover these cases. Due to common
		 * clock framework restrictions that do not allow
		 * to disable a clock that has not been enabled,
		 * we need to enable the clock first.
		 */
		ret = clk_prepare_enable(ctx->mclk);
		if (!ret)
			clk_disable_unprepare(ctx->mclk);

		ret = clk_set_rate(ctx->mclk, CHT_PLAT_CLK_3_HZ);

		if (ret) {
			dev_err(runtime->dev, "unable to set MCLK rate\n");
			return ret;
		}
	}

	return 0;
}

static int cht_codec_fixup(struct snd_soc_pcm_runtime *rtd,
			   struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
			SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
						SNDRV_PCM_HW_PARAM_CHANNELS);

	/* The DSP will convert the FE rate to 48k, stereo, 24bits */
	rate->min = 48000;
	rate->max = 48000;
	channels->min = 2;
	channels->max = 2;

	/*
	 * Configure SSP2 for the 24-bit format expected by the codec. The SST
	 * ssp2-port front end still advertises S16_LE and converts the stream.
	 */
	params_set_format(params, SNDRV_PCM_FORMAT_S24_LE);

	return 0;
}

static struct snd_soc_jack_pin cht_yb_jack_pins[] = {
	{
		.pin = "Headphone",
		.mask = SND_JACK_HEADPHONE,
	},
	{
		.pin = "Headset Mic",
		.mask = SND_JACK_MICROPHONE,
	},
};

static int cht_yb_headset_init(struct snd_soc_component *component)
{
	struct snd_soc_card *card = component->card;
	struct cht_mc_private *ctx = snd_soc_card_get_drvdata(card);
	struct snd_soc_jack *jack = &ctx->jack;
	int jack_type;
	int ret;

	/*
	 * TI supports four headset buttons:
	 * KEY_MEDIA
	 * KEY_VOICECOMMAND
	 * KEY_VOLUMEUP
	 * KEY_VOLUMEDOWN
	 */
	jack_type = SND_JACK_HEADPHONE | SND_JACK_MICROPHONE |
		    SND_JACK_BTN_0 | SND_JACK_BTN_1 |
		    SND_JACK_BTN_2 | SND_JACK_BTN_3;

	ret = snd_soc_card_jack_new_pins(card, "Headset Jack", jack_type,
					 jack, cht_yb_jack_pins,
					 ARRAY_SIZE(cht_yb_jack_pins));
	if (ret) {
		dev_err(card->dev, "Headset Jack creation failed %d\n", ret);
		return ret;
	}

	return ts3a227e_enable_jack_detect(component, jack);
}

static int cht_aif1_startup(struct snd_pcm_substream *substream)
{
	return snd_pcm_hw_constraint_single(substream->runtime,
			SNDRV_PCM_HW_PARAM_RATE, 48000);
}

static const struct snd_soc_ops cht_aif1_ops = {
	.startup = cht_aif1_startup,
};

static const struct snd_soc_ops cht_be_ssp2_ops = {
	.hw_params = cht_aif1_hw_params,
};

static const struct snd_soc_aux_dev cht_yb_headset_dev = {
	.dlc = COMP_AUX("i2c-ts3a227e"),
	.init = cht_yb_headset_init,
};

SND_SOC_DAILINK_DEF(dummy, DAILINK_COMP_ARRAY(COMP_DUMMY()));

SND_SOC_DAILINK_DEF(media, DAILINK_COMP_ARRAY(COMP_CPU("media-cpu-dai")));

SND_SOC_DAILINK_DEF(deepbuffer, DAILINK_COMP_ARRAY(COMP_CPU("deepbuffer-cpu-dai")));

SND_SOC_DAILINK_DEF(ssp2_port, DAILINK_COMP_ARRAY(COMP_CPU("ssp2-port")));
SND_SOC_DAILINK_DEF(ssp2_codec, DAILINK_COMP_ARRAY(COMP_CODEC(RT5677_I2C, CHT_CODEC_DAI)));

SND_SOC_DAILINK_DEF(platform, DAILINK_COMP_ARRAY(COMP_PLATFORM("sst-mfld-platform")));

static const struct snd_soc_dai_link cht_dailink[] = {
	/* Front End DAI links */
	[MERR_DPCM_AUDIO] = {
		.name = "Audio Port",
		.stream_name = "Audio",
		.nonatomic = true,
		.dynamic = 1,
		.ops = &cht_aif1_ops,
		SND_SOC_DAILINK_REG(media, dummy, platform),
	},
	[MERR_DPCM_DEEP_BUFFER] = {
		.name = "Deep-Buffer Audio Port",
		.stream_name = "Deep-Buffer Audio",
		.nonatomic = true,
		.dynamic = 1,
		.playback_only = 1,
		.ops = &cht_aif1_ops,
		SND_SOC_DAILINK_REG(deepbuffer, dummy, platform),
	},

	/* Back End DAI links */
	{
		/* SSP2 - Codec */
		.name = "SSP2-Codec",
		.id = 0,
		.no_pcm = 1,
		.nonatomic = true,
		.init = cht_codec_init,
		.be_hw_params_fixup = cht_codec_fixup,
		.ops = &cht_be_ssp2_ops,
		SND_SOC_DAILINK_REG(ssp2_port, ssp2_codec, platform),
	},
};

/* SoC card */
static const struct snd_soc_card snd_soc_card_cht = {
	.owner = THIS_MODULE,
	.num_links = ARRAY_SIZE(cht_dailink),
	.num_aux_devs = 1,
	.dapm_widgets = cht_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(cht_dapm_widgets),
	.dapm_routes = cht_audio_map,
	.num_dapm_routes = ARRAY_SIZE(cht_audio_map),
	.controls = cht_mc_controls,
	.num_controls = ARRAY_SIZE(cht_mc_controls),
};

static const struct acpi_gpio_params speaker_enable_gpio = { 2, 0, false };
static const struct acpi_gpio_mapping cht_yb_gpios[] = {
	{ "speaker-enable-gpios", &speaker_enable_gpio, 1 },
	{ NULL }
};

#define SOF_CARD_NAME "cht yogabook"
#define SOF_DRIVER_NAME "SOF"

#define CARD_NAME "cht-yogabook"
#define DRIVER_NAME NULL

static int snd_cht_mc_probe(struct platform_device *pdev)
{
	struct cht_mc_private *drv;
	struct snd_soc_acpi_mach *mach = pdev->dev.platform_data;
	struct snd_soc_aux_dev *aux_dev;
	struct snd_soc_dai_link_component *codecs;
	struct snd_soc_dai_link *dai_links;
	struct snd_soc_card *card;
	const void *codec_template;
	size_t codecs_size;
	const char *platform_name;
	struct acpi_device *adev;
	struct device *codec_dev;
	bool has_acpi_codec = false;
	bool sof_parent;
	int ret;
	int i;

	drv = devm_kzalloc(&pdev->dev, sizeof(*drv), GFP_KERNEL);
	if (!drv)
		return -ENOMEM;

	card = devm_kmemdup(&pdev->dev, &snd_soc_card_cht, sizeof(*card), GFP_KERNEL);
	if (!card)
		return -ENOMEM;

	dai_links = devm_kmemdup(&pdev->dev, cht_dailink, sizeof(cht_dailink), GFP_KERNEL);
	if (!dai_links)
		return -ENOMEM;

	aux_dev = devm_kmemdup(&pdev->dev, &cht_yb_headset_dev,
			       sizeof(cht_yb_headset_dev), GFP_KERNEL);
	if (!aux_dev)
		return -ENOMEM;

	card->dai_link = dai_links;
	card->aux_dev = aux_dev;
	drv->jack_nb.notifier_call = cht_yb_jack_event;

	strscpy(drv->codec_name, RT5677_I2C, sizeof(drv->codec_name));

	/* fixup codec name based on HID if ACPI node is present */
	adev = acpi_dev_get_first_match_dev(mach->id, NULL, -1);
	if (adev) {
		has_acpi_codec = true;
		snprintf(drv->codec_name, sizeof(drv->codec_name),
			 "i2c-%s", acpi_dev_name(adev));
		dev_info(&pdev->dev, "real codec name: %s\n", drv->codec_name);

		put_device(&adev->dev);
		for (i = 0; i < card->num_links; i++) {
			if (dai_links[i].codecs->name &&
			    !strcmp(dai_links[i].codecs->name,
				    RT5677_I2C)) {
				codecs_size = sizeof(*codecs) * dai_links[i].num_codecs;
				codec_template = dai_links[i].codecs;
				codecs = devm_kmemdup(&pdev->dev, codec_template, codecs_size,
						      GFP_KERNEL);
				if (!codecs)
					return -ENOMEM;

				codecs->name = drv->codec_name;
				dai_links[i].codecs = codecs;
				break;
			}
		}
	}

	codec_dev = bus_find_device_by_name(&i2c_bus_type, NULL,
					    drv->codec_name);
	if (!codec_dev)
		return -EPROBE_DEFER;

	if (has_acpi_codec) {
		ret = devm_acpi_dev_add_driver_gpios(codec_dev, cht_yb_gpios);
		if (ret)
			dev_warn(&pdev->dev, "Unable to add GPIO mapping table: %d\n",
				 ret);
	}
	put_device(codec_dev);

	/* Override platform name, if required. */
	card->dev = &pdev->dev;
	platform_name = mach->mach_params.platform;

	ret = snd_soc_fixup_dai_links_platform_name(card, platform_name);
	if (ret) {
		dev_err(&pdev->dev, "snd_soc_fixup_dai_links_platform_name failed: %d\n",
			ret);
		return ret;
	}

	drv->mclk = devm_clk_get(&pdev->dev, "pmc_plt_clk_3");
	if (IS_ERR(drv->mclk)) {
		dev_err(&pdev->dev,
			"Failed to get MCLK from pmc_plt_clk_3: %ld\n",
			PTR_ERR(drv->mclk));
		return PTR_ERR(drv->mclk);
	}
	snd_soc_card_set_drvdata(card, drv);

	sof_parent = snd_soc_acpi_sof_parent(&pdev->dev);

	/* set the card and driver name */
	if (sof_parent) {
		card->name = SOF_CARD_NAME;
		card->driver_name = SOF_DRIVER_NAME;
	} else {
		card->name = CARD_NAME;
		card->driver_name = DRIVER_NAME;
	}

	/* register the soc card */
	ret = devm_snd_soc_register_card(&pdev->dev, card);
	if (ret) {
		dev_err(&pdev->dev,
			"snd_soc_register_card failed %d\n", ret);
		return ret;
	}
	platform_set_drvdata(pdev, card);

	return 0;
}

static struct platform_driver snd_cht_mc_driver = {
	.driver = {
		.name = "cht-yogabook",
	},
	.probe = snd_cht_mc_probe,
};

module_platform_driver(snd_cht_mc_driver);

MODULE_DESCRIPTION("Lenovo Yoga Book YB1-X91 machine driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:cht-yogabook");
