// SPDX-License-Identifier: GPL-2.0-only
/*
 * AD24xx codec driver
 *
 * Copyright (c) 2023-2024 Alvin Šipraga <alsi@bang-olufsen.dk>
 *
 * Analog Devices Inc. documentation cited in some of the comments below:
 *
 * [1] AD2420(W)/6(W)/7(W)/8(W)/9(W) Automotive Audio Bus A2B Transceiver
 *     Technical Reference, Revision 1.1, October 2019, Part Number 82-100138-01
 */

#include <linux/a2b/a2b.h>
#include <linux/a2b/ad24xx.h>
#include <linux/bitfield.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <sound/soc.h>

#define AD24XX_RATES_SUB_48 \
	(SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_96000 | SNDRV_PCM_RATE_192000)
#define AD24XX_RATES_SUB_44_1                                                 \
	(SNDRV_PCM_RATE_11025 | SNDRV_PCM_RATE_22050 | SNDRV_PCM_RATE_44100 | \
	 SNDRV_PCM_RATE_88200 | SNDRV_PCM_RATE_176400)
#define AD24XX_RATES_MAIN_48 SNDRV_PCM_RATE_48000
#define AD24XX_RATES_MAIN_44_1 SNDRV_PCM_RATE_44100

struct ad24xx_codec {
	struct device *dev;
	struct a2b_func *func;
	struct a2b_node *node;
	struct regmap *regmap;
	struct snd_soc_dai_driver *dai_drv;
	struct a2b_slot_config slot_config;
};

static const char *const ad24xx_codec_slot_format_text[] = {
	"Normal Slot Format",
	"Alternate Slot Format",
};

static const char *const ad24xx_codec_slot_size_text[] = {
	"8 bits",  "12 bits", "16 bits", "20 bits",
	"24 bits", "28 bits", "32 bits",
};

static SOC_ENUM_SINGLE_VIRT_DECL(ad24xx_codec_dn_slot_size_enum,
				 ad24xx_codec_slot_size_text);
static SOC_ENUM_SINGLE_VIRT_DECL(ad24xx_codec_dn_slot_format_enum,
				 ad24xx_codec_slot_format_text);
static SOC_ENUM_SINGLE_VIRT_DECL(ad24xx_codec_up_slot_size_enum,
				 ad24xx_codec_slot_size_text);
static SOC_ENUM_SINGLE_VIRT_DECL(ad24xx_codec_up_slot_format_enum,
				 ad24xx_codec_slot_format_text);

static int ad24xx_codec_slot_config_get(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct ad24xx_codec *adc = snd_soc_component_get_drvdata(component);
	struct a2b_slot_config *slot_config = &adc->slot_config;
	const struct soc_enum *priv = (void *)kcontrol->private_value;
	unsigned int *val = &ucontrol->value.enumerated.item[0];

	if (priv == &ad24xx_codec_dn_slot_size_enum)
		*val = slot_config->size[A2B_DIR_DOWN];
	else if (priv == &ad24xx_codec_dn_slot_format_enum)
		*val = slot_config->format[A2B_DIR_DOWN];
	else if (priv == &ad24xx_codec_up_slot_size_enum)
		*val = slot_config->size[A2B_DIR_UP];
	else if (priv == &ad24xx_codec_up_slot_format_enum)
		*val = slot_config->format[A2B_DIR_UP];
	else
		return -ENOENT;

	return 0;
}

static int ad24xx_codec_slot_config_put(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct ad24xx_codec *adc = snd_soc_component_get_drvdata(component);
	struct a2b_slot_config *slot_config = &adc->slot_config;
	const struct soc_enum *priv = (void *)kcontrol->private_value;
	unsigned int val = ucontrol->value.enumerated.item[0];
	enum a2b_direction direction =
		(priv == &ad24xx_codec_up_slot_size_enum ||
		 priv == &ad24xx_codec_up_slot_format_enum) ?
			A2B_DIR_UP :
			A2B_DIR_DOWN;

	if (priv == &ad24xx_codec_up_slot_size_enum ||
	    priv == &ad24xx_codec_dn_slot_size_enum) {
		if (val >= ARRAY_SIZE(ad24xx_codec_slot_size_text))
			return -EINVAL;
		slot_config->size[direction] = val;
	} else if (priv == &ad24xx_codec_up_slot_format_enum ||
		   priv == &ad24xx_codec_dn_slot_format_enum) {
		if (val >= ARRAY_SIZE(ad24xx_codec_slot_format_text))
			return -EINVAL;
		slot_config->format[direction] = val;
	} else
		return -ENOENT;

	return 0;
}

static const struct snd_kcontrol_new ad24xx_codec_controls_main[] = {
	SOC_SINGLE("Downstream Slots", A2B_DNSLOTS, 0, 32, 0),
	SOC_SINGLE("Upstream Slots", A2B_UPSLOTS, 0, 32, 0),
	SOC_ENUM_EXT("Downstream Slot Size", ad24xx_codec_dn_slot_size_enum,
		     ad24xx_codec_slot_config_get,
		     ad24xx_codec_slot_config_put),
	SOC_ENUM_EXT("Downstream Slot Format", ad24xx_codec_dn_slot_format_enum,
		     ad24xx_codec_slot_config_get,
		     ad24xx_codec_slot_config_put),
	SOC_ENUM_EXT("Upstream Slot Size", ad24xx_codec_up_slot_size_enum,
		     ad24xx_codec_slot_config_get,
		     ad24xx_codec_slot_config_put),
	SOC_ENUM_EXT("Upstream Slot Format", ad24xx_codec_up_slot_format_enum,
		     ad24xx_codec_slot_config_get,
		     ad24xx_codec_slot_config_put),
};

static const struct snd_kcontrol_new ad24xx_codec_controls_sub[] = {
	SOC_SINGLE("Broadcast Downstream Slots", A2B_BCDNSLOTS, 0, 32, 0),
	SOC_SINGLE("Downstream Slots Targeted", A2B_LDNSLOTS, 0, 32, 0),
	SOC_SINGLE("Upstream Slots Generated", A2B_LUPSLOTS, 0, 32, 0),
	SOC_SINGLE("Downstream Slots", A2B_DNSLOTS, 0, 32, 0),
	SOC_SINGLE("Upstream Slots", A2B_UPSLOTS, 0, 32, 0),
};

static const struct snd_kcontrol_new ad24xx_codec_controls_data_rx_mask[] = {
	SOC_SINGLE("Downstream Broadcast Mask Enable", A2B_LDNSLOTS, 7, 1, 0),
	SND_SOC_BYTES("Upstream Data RX Mask", A2B_UPMASK0, 4),
	SOC_SINGLE("Local Upstream Channel Offset", A2B_UPOFFSET, 0, 31, 0),
	SND_SOC_BYTES("Downstream Data RX Mask", A2B_DNMASK0, 4),
	SOC_SINGLE("Local Downstream Channel Offset", A2B_DNOFFSET, 0, 31, 0),
};

#define SND_SOC_DAPM_ENCODER(wname, stname, wreg, wshift, winvert) \
{	.id = snd_soc_dapm_encoder, .name = wname, .sname = stname, \
	SND_SOC_DAPM_INIT_REG_VAL(wreg, wshift, winvert), }

#define SND_SOC_DAPM_DECODER(wname, stname, wreg, wshift, winvert) \
{	.id = snd_soc_dapm_decoder, .name = wname, .sname = stname, \
	SND_SOC_DAPM_INIT_REG_VAL(wreg, wshift, winvert), }

static const struct snd_soc_dapm_widget ad24xx_codec_dapm_widgets[] = {
	SND_SOC_DAPM_AIF_IN("RX0", NULL, 0, A2B_I2SCFG, 4, 0),
	SND_SOC_DAPM_AIF_IN("RX1", NULL, 0, A2B_I2SCFG, 5, 0),
	SND_SOC_DAPM_AIF_OUT("TX0", NULL, 0, A2B_I2SCFG, 0, 0),
	SND_SOC_DAPM_AIF_OUT("TX1", NULL, 0, A2B_I2SCFG, 1, 0),
	SND_SOC_DAPM_ENCODER("ENC", NULL, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_DECODER("DEC", NULL, SND_SOC_NOPM, 0, 0),
};

static const struct snd_soc_dapm_route ad24xx_codec_dapm_routes_main[] = {
	{ "I2S Capture", NULL, "DEC" },
	{ "TX0", NULL, "I2S Capture" },
	{ "TX1", NULL, "I2S Capture" },
	{ "I2S Playback", NULL, "RX0" },
	{ "I2S Playback", NULL, "RX1" },
	{ "ENC", NULL, "I2S Playback" },
};

static const struct snd_soc_dapm_route ad24xx_codec_dapm_routes_sub[] = {
	{ "ENC", NULL, "I2S Capture" },
	{ "I2S Capture", NULL, "RX0" },
	{ "I2S Capture", NULL, "RX1" },
	{ "TX0", NULL, "I2S Playback" },
	{ "TX1", NULL, "I2S Playback" },
	{ "I2S Playback", NULL, "DEC" },
};

static int ad24xx_codec_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct snd_soc_component *component = dai->component;
	struct ad24xx_codec *adc = snd_soc_component_get_drvdata(component);
	bool bclk_invert;
	unsigned int val;
	int ret;

	/* Main node must be BCLK/FSYNC consumer, subordinate node provider */
	if ((fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) !=
	    (is_a2b_main(adc->node) ? SND_SOC_DAIFMT_CBC_CFC :
				      SND_SOC_DAIFMT_CBP_CFP))
		return -EINVAL;

	switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_NB_NF:
		if (adc->node->invert_sync)
			return -EINVAL;
		bclk_invert = false;
		break;
	case SND_SOC_DAIFMT_NB_IF:
		if (!adc->node->invert_sync)
			return -EINVAL;
		bclk_invert = false;
		break;
	case SND_SOC_DAIFMT_IB_NF:
		if (adc->node->invert_sync)
			return -EINVAL;
		bclk_invert = true;
		break;
	case SND_SOC_DAIFMT_IB_IF:
		if (!adc->node->invert_sync)
			return -EINVAL;
		bclk_invert = true;
		break;
	}

	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
		if (!adc->node->alternating_sync || !adc->node->early_sync)
			return -EINVAL;
		break;
	case SND_SOC_DAIFMT_DSP_A:
		if (adc->node->alternating_sync || !adc->node->early_sync)
			return -EINVAL;
		break;
	case SND_SOC_DAIFMT_DSP_B:
		if (adc->node->alternating_sync || adc->node->early_sync)
			return -EINVAL;
		break;
	default:
		return -EINVAL;
	}

	val = bclk_invert ? A2B_I2SCFG_RXBCLKINV_MASK :
			    A2B_I2SCFG_TXBCLKINV_MASK;
	ret = regmap_update_bits(
		adc->regmap, A2B_I2SCFG,
		A2B_I2SCFG_TXBCLKINV_MASK | A2B_I2SCFG_RXBCLKINV_MASK, val);
	if (ret)
		return ret;

	return 0;
}

static int ad24xx_codec_calc_a_dnslots(struct ad24xx_codec *adc)
{
	struct a2b_node *node = adc->node;
	unsigned int dnslots;
	unsigned int dnmasken;
	unsigned int ldnslots;
	unsigned int bcdnslots;
	unsigned int dnmaskrx;
	__le32 dnmask;
	unsigned int val;
	int ret;

	/*
	 * Calculate the number of downstream slots to be received by this
	 * node's A-side transceiver. For main nodes this is trivially zero
	 * because the A-side is inactive. Following [1] section 3-18
	 * "Downstream Data Slots", for subordinate nodes the calculation
	 * depends on whether the A2B_LDNSLOTS.DNMASKEN bit is set:
	 *
	 *   DNMASKEN=0 => A2B_BCDNSLOTS + A2B_DNSLOTS + A2B_LDNSLOTS
	 *   DNMASKEN=1 => max(A2B_DNSLOTS, dnmaskrx)
	 *
	 * where dnmaskrx is the most significant bit of the A2B_DNMASK{0,3}
	 * mask.
	 */

	if (is_a2b_main(node))
		return 0;

	ret = regmap_read(adc->regmap, A2B_DNSLOTS, &val);
	if (ret)
		return ret;

	dnslots = FIELD_GET(A2B_DNSLOTS_DNSLOTS_MASK, val);

	ret = regmap_read(adc->regmap, A2B_LDNSLOTS, &val);
	if (ret)
		return ret;

	ldnslots = FIELD_GET(A2B_LDNSLOTS_LDNSLOTS_MASK, val);
	dnmasken = FIELD_GET(A2B_LDNSLOTS_DNMASKEN_MASK, val);

	if (!dnmasken) {
		ret = regmap_read(adc->regmap, A2B_BCDNSLOTS, &val);
		if (ret)
			return ret;

		bcdnslots = FIELD_GET(A2B_BCDNSLOTS_BCDNSLOTS_MASK, val);

		return bcdnslots + dnslots + ldnslots;
	}

	ret = regmap_bulk_read(adc->regmap, A2B_DNMASK0, &dnmask, 4);
	if (ret)
		return ret;

	dnmaskrx = fls(le32_to_cpu(dnmask));

	return max(dnslots, dnmaskrx);
}

static int ad24xx_codec_calc_b_dnslots(struct ad24xx_codec *adc)
{
	struct a2b_node *node = adc->node;
	unsigned int dnslots;
	unsigned int dnmasken;
	unsigned int ldnslots;
	unsigned int bcdnslots;
	unsigned int val;
	int ret;

	/*
	 * Calculate the number of downstream slots to be transmitted by this
	 * node's B-side transceiver. Following [1] section 3-18 "Downstream
	 * Data Slots", for main nodes the number is A2B_DNSLOTS. For
	 * subordinate nodes the calculation depends on whether the
	 * A2B_LDNSLOTS.DNMASKEN bit is set:
	 *
	 *   DNMASKEN=0 => A2B_BCDNSLOTS + A2B_DNSLOTS
	 *   DNMASKEN=1 => A2B_DNSLOTS + A2B_LDNSLOTS
	 */

	ret = regmap_read(adc->regmap, A2B_DNSLOTS, &val);
	if (ret)
		return ret;

	dnslots = FIELD_GET(A2B_DNSLOTS_DNSLOTS_MASK, val);

	if (is_a2b_main(node))
		return dnslots;

	ret = regmap_read(adc->regmap, A2B_LDNSLOTS, &val);
	if (ret)
		return ret;

	ldnslots = FIELD_GET(A2B_LDNSLOTS_LDNSLOTS_MASK, val);
	dnmasken = FIELD_GET(A2B_LDNSLOTS_DNMASKEN_MASK, val);

	if (dnmasken)
		return dnslots + ldnslots;

	ret = regmap_read(adc->regmap, A2B_BCDNSLOTS, &val);
	if (ret)
		return ret;

	bcdnslots = FIELD_GET(A2B_BCDNSLOTS_BCDNSLOTS_MASK, val);

	return bcdnslots + dnslots;
}

static unsigned int ad24xx_codec_calc_a_upslots(struct ad24xx_codec *adc)
{
	struct a2b_node *node = adc->node;
	unsigned int upslots;
	unsigned int lupslots;
	unsigned int val;
	int ret;

	/*
	 * Calculate the number of upstream slots to be transmitted by this
	 * node's A-side transceiver. According to [1] section 3-20 "Upstream
	 * Data Slots", this is A2B_UPSLOTS + A2B_LUPSLOTS for subordinate
	 * nodes. For the main node it is trivially always zero, as its A-side
	 * is inactive.
	 */

	if (is_a2b_main(node))
		return 0;

	ret = regmap_read(adc->regmap, A2B_UPSLOTS, &val);
	if (ret)
		return ret;

	upslots = FIELD_GET(A2B_UPSLOTS_UPSLOTS_MASK, val);

	ret = regmap_read(adc->regmap, A2B_LUPSLOTS, &val);
	if (ret)
		return ret;

	lupslots = FIELD_GET(A2B_LUPSLOTS_LUPSLOTS_MASK, val);

	return upslots + lupslots;
}

static unsigned int ad24xx_codec_calc_b_upslots(struct ad24xx_codec *adc)
{
	struct a2b_node *node = adc->node;
	unsigned int upslots;
	unsigned int upmaskrx;
	unsigned int upmask;
	unsigned int val;
	u8 buf[4];
	int ret;

	/*
	 * Calculate the number of upstream slots to be received by this node's
	 * B-side transceiver. This is, cf. [1] section 3-20, max(A2B_UPSLOTS,
	 * upmaskrx), where upmaskrx is the most significant bit of the
	 * A2B_UPMASK{0,3} mask. For main nodes it is simply the value of
	 * A2B_UPSLOTS, as they have no upstream data RX mask to configure.
	 */

	ret = regmap_read(adc->regmap, A2B_UPSLOTS, &val);
	if (ret)
		return ret;

	upslots = FIELD_GET(A2B_UPSLOTS_UPSLOTS_MASK, val);

	if (is_a2b_main(node))
		return upslots;

	ret = regmap_bulk_read(adc->regmap, A2B_UPMASK0, buf, 4);
	if (ret)
		return ret;

	upmask = buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
	upmaskrx = fls(upmask);

	return max(upslots, upmaskrx);
}

static int ad24xx_codec_hw_params(struct snd_pcm_substream *substream,
				  struct snd_pcm_hw_params *params,
				  struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct ad24xx_codec *adc = snd_soc_component_get_drvdata(component);
	unsigned int rate = params_rate(params);
	struct a2b_slot_req slot_req = {
		.a_dnslots = ad24xx_codec_calc_a_dnslots(adc),
		.a_upslots = ad24xx_codec_calc_a_upslots(adc),
		.b_dnslots = ad24xx_codec_calc_b_dnslots(adc),
		.b_upslots = ad24xx_codec_calc_b_upslots(adc),
		.slot_config = adc->slot_config, /* ignored for subordinates */
	};
	enum a2b_superframe_freq sff = adc->node->bus->sff;
	int ret;

	/* Configure I2S/TDM rate */
	if (is_a2b_main(adc->node)) {
		/*
		 * The I2S rate of the main node DAIs is fixed at the superframe
		 * frequency (SFF) and cannot change.
		 */
		if (!((rate == 48000 && sff == A2B_SFF_48000) ||
		      (rate == 44100 && sff == A2B_SFF_44100)))
			return -EINVAL;
	} else {
		/*
		 * The I2S rate of subordinate nodes can be set to (SFF * x)
		 * for x in { 0.25, 0.5, 1, 2, 4 }.
		 */
		unsigned int sff_rate = sff == A2B_SFF_48000 ? 48000 : 44100;
		unsigned int val = 0;

		if (rate == sff_rate / 4)
			val |= FIELD_PREP(A2B_I2SRATE_I2SRATE_MASK, 2);
		else if (rate == sff_rate / 2)
			val |= FIELD_PREP(A2B_I2SRATE_I2SRATE_MASK, 1);
		else if (rate == sff_rate)
			val |= FIELD_PREP(A2B_I2SRATE_I2SRATE_MASK, 0);
		/* A2B_I2SRRATE.RRDIV support is not implemented */
		else if (rate == sff_rate * 2)
			val |= FIELD_PREP(A2B_I2SRATE_I2SRATE_MASK, 5);
		else if (rate == sff_rate * 4)
			val |= FIELD_PREP(A2B_I2SRATE_I2SRATE_MASK, 6);
		else
			return -EINVAL;

		ret = regmap_update_bits(adc->regmap, A2B_I2SRATE,
					 A2B_I2SRATE_I2SRATE_MASK, val);
		if (ret)
			return ret;
	}


	/* Finally, request slots */
	ret = a2b_node_request_slots(adc->node, &slot_req);
	if (ret)
		return ret;

	return 0;
}

static int ad24xx_codec_hw_free(struct snd_pcm_substream *substream,
				struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct ad24xx_codec *adc = snd_soc_component_get_drvdata(component);
	int ret;

	ret = a2b_node_free_slots(adc->node);
	if (ret)
		return ret;

	return 0;
}

static const struct snd_soc_dai_ops ad24xx_codec_dai_ops = {
	.set_fmt = ad24xx_codec_set_fmt,
	.hw_params = ad24xx_codec_hw_params,
	.hw_free = ad24xx_codec_hw_free,
};

enum ad24xx_codec_dai {
	AD24XX_DAI_I2S,
};

static const struct snd_soc_dai_driver ad24xx_codec_dai_drv[] = {
	[AD24XX_DAI_I2S] = {
		.name = "ad24xx-i2s",
		.playback = {
			.stream_name = "I2S Playback",
			.channels_min = 1,
			.channels_max = 32,
		},
		.capture = {
			.stream_name = "I2S Capture",
			.channels_min = 1,
			.channels_max = 32,
		},
		.ops = &ad24xx_codec_dai_ops,
		.symmetric_rate = 1,
	},
};

static int ad24xx_codec_component_probe(struct snd_soc_component *component)
{
	struct ad24xx_codec *adc = snd_soc_component_get_drvdata(component);
	struct a2b_node *node = adc->node;
	int ret;

	snd_soc_component_init_regmap(component, adc->regmap);

	if (is_a2b_sub(node) &&
	    (node->chip_info->caps & A2B_CHIP_CAP_DATA_RX_MASK)) {
		ret = snd_soc_add_component_controls(
			component, ad24xx_codec_controls_data_rx_mask,
			ARRAY_SIZE(ad24xx_codec_controls_data_rx_mask));
		if (ret)
			return ret;
	}

	return 0;
}

static const struct snd_soc_component_driver ad24xx_codec_component_drv_main = {
	.probe = ad24xx_codec_component_probe,
	.controls = ad24xx_codec_controls_main,
	.num_controls = ARRAY_SIZE(ad24xx_codec_controls_main),
	.dapm_widgets = ad24xx_codec_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(ad24xx_codec_dapm_widgets),
	.dapm_routes = ad24xx_codec_dapm_routes_main,
	.num_dapm_routes = ARRAY_SIZE(ad24xx_codec_dapm_routes_main),
	.endianness = 1,
};

static const struct snd_soc_component_driver ad24xx_codec_component_drv_sub = {
	.probe = ad24xx_codec_component_probe,
	.controls = ad24xx_codec_controls_sub,
	.num_controls = ARRAY_SIZE(ad24xx_codec_controls_sub),
	.dapm_widgets = ad24xx_codec_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(ad24xx_codec_dapm_widgets),
	.dapm_routes = ad24xx_codec_dapm_routes_sub,
	.num_dapm_routes = ARRAY_SIZE(ad24xx_codec_dapm_routes_sub),
	.endianness = 1,
};

static const struct regmap_config ad24xx_codec_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.cache_type = REGCACHE_RBTREE,
};

static int ad24xx_codec_probe(struct device *dev)
{
	struct a2b_func *func = to_a2b_func(dev);
	const struct snd_soc_component_driver *drv;
	struct snd_soc_dai_driver *i2s_dai;
	struct ad24xx_codec *adc;
	int ret;

	adc = devm_kzalloc(dev, sizeof(*adc), GFP_KERNEL);
	if (!adc)
		return -ENOMEM;

	adc->dev = dev;
	adc->func = func;
	adc->node = func->node;
	dev_set_drvdata(dev, adc);

	adc->regmap =
		devm_regmap_init_a2b_func(func, &ad24xx_codec_regmap_config);
	if (IS_ERR(adc->regmap))
		return PTR_ERR(adc->regmap);

	adc->dai_drv = devm_kmemdup(dev, ad24xx_codec_dai_drv,
				    sizeof(ad24xx_codec_dai_drv), GFP_KERNEL);
	if (!adc->dai_drv)
		return -ENOMEM;

	i2s_dai = &adc->dai_drv[AD24XX_DAI_I2S];

	if (adc->node->tdm_slot_size == A2B_TDMSS_32)
		i2s_dai->playback.formats = i2s_dai->capture.formats =
			SNDRV_PCM_FMTBIT_S32_LE;
	else
		i2s_dai->playback.formats = i2s_dai->capture.formats =
			SNDRV_PCM_FMTBIT_S16_LE;

	if (is_a2b_main(adc->node)) {
		if (adc->node->bus->sff == A2B_SFF_48000)
			i2s_dai->playback.rates = i2s_dai->capture.rates =
				AD24XX_RATES_MAIN_48;
		else
			i2s_dai->playback.rates = i2s_dai->capture.rates =
				AD24XX_RATES_MAIN_44_1;
	} else {
		if (adc->node->bus->sff == A2B_SFF_48000)
			i2s_dai->playback.rates = i2s_dai->capture.rates =
				AD24XX_RATES_SUB_48;
		else
			i2s_dai->playback.rates = i2s_dai->capture.rates =
				AD24XX_RATES_SUB_44_1;
	}

	if (is_a2b_main(adc->node))
		drv = &ad24xx_codec_component_drv_main;
	else
		drv = &ad24xx_codec_component_drv_sub;

	ret = devm_snd_soc_register_component(dev, drv, adc->dai_drv,
					      ARRAY_SIZE(ad24xx_codec_dai_drv));
	if (ret)
		return ret;

	return 0;
}

static const struct of_device_id ad24xx_codec_of_match_table[] = {
	{ .compatible = "adi,ad2403-codec" },
	{ .compatible = "adi,ad2410-codec" },
	{ .compatible = "adi,ad2425-codec" },
	{ .compatible = "adi,ad2428-codec" },
	{ .compatible = "adi,ad2429-codec" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ad24xx_codec_of_match_table);

static struct a2b_driver ad24xx_codec_driver = {
	.driver = {
		.name = "ad24xx-codec",
		.of_match_table = ad24xx_codec_of_match_table,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.probe = ad24xx_codec_probe,
};
module_a2b_driver(ad24xx_codec_driver);

MODULE_AUTHOR("Alvin Šipraga <alsi@bang-olufsen.dk>");
MODULE_DESCRIPTION("AD24xx codec driver");
MODULE_LICENSE("GPL");
