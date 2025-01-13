// SPDX-License-Identifier: (GPL-2.0-only OR MIT)
/*
 * Copyright (C) 2023 Amlogic, Inc. All rights reserved
 */

#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/regmap.h>
#include <sound/soc.h>
#include <sound/soc-dai.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include<linux/kstrtox.h>
#include <linux/clk-provider.h>
#include <linux/reset.h>
#include "axg-tdm.h"

#define TOACODEC_CTRL0			0x0

#define CTRL0_ENABLE_SHIFT		31
#define CTRL0_BCLK_ENABLE_SHIFT		30
#define CTRL0_MCLK_ENABLE_SHIFT		29
#define CTRL0_BLK_CAP_INV_SHIFT		9

#define TDM_IFACE 0
#define TDM_A_PAD 0
#define TDM_B_PAD 1
#define TDM_C_PAD 2

struct toacodec {
	struct regmap_field *field_dat_sel;
	struct regmap_field *field_lrclk_sel;
	struct regmap_field *field_bclk_sel;
	struct regmap_field *field_mclk_sel;
};

struct toacodec_match_data {
	const struct snd_soc_component_driver *component_drv;
	const struct reg_field field_dat_sel;
	const struct reg_field field_lrclk_sel;
	const struct reg_field field_bclk_sel;
	const struct reg_field field_mclk_sel;
};

static const struct regmap_config tocodec_regmap_cfg = {
	.reg_bits	= 32,
	.val_bits	= 32,
	.reg_stride	= 4,
	.max_register	= 0x1,
};

#define S4_LANE_OFFSET 8

static const char * const s4_tocodec_lane_sel_texts[] = {
	"Lane0", "Lane1", "Lane2", "Lane3", "Lane4", "Lane5", "Lane6", "Lane7"
};

static const struct soc_enum s4_tocodec_lane_sel_enum =
	SOC_ENUM_SINGLE(SND_SOC_NOPM, 0, ARRAY_SIZE(s4_tocodec_lane_sel_texts),
			s4_tocodec_lane_sel_texts);

static const struct snd_kcontrol_new s4_tocodec_lane_sel =
	SOC_DAPM_ENUM("TOCODEC LANE SEL", s4_tocodec_lane_sel_enum);

static const char * const s4_tocodec_src_sel_texts[] = {
	"TDMA", "TDMB", "TDMC"
};

static const struct soc_enum s4_tocodec_src_sel_enum =
	SOC_ENUM_SINGLE(SND_SOC_NOPM, 0, ARRAY_SIZE(s4_tocodec_src_sel_texts),
			s4_tocodec_src_sel_texts);

static const struct snd_kcontrol_new s4_tocodec_src_sel =
	SOC_DAPM_ENUM("TOCODEC SEL", s4_tocodec_src_sel_enum);

static const struct snd_kcontrol_new s4_toacodec_out_enable =
	SOC_DAPM_SINGLE_AUTODISABLE("Switch", TOACODEC_CTRL0,
				    CTRL0_ENABLE_SHIFT, 1, 0);

static struct snd_soc_dai *tocodec_tdm_get_ahead_be(struct snd_soc_dapm_widget *w)
{
	struct snd_soc_dapm_path *p;
	struct snd_soc_dai *be;

	snd_soc_dapm_widget_for_each_source_path(w, p) {
		if (!p->connect)
			continue;
		if (p->source->id == snd_soc_dapm_dai_in)
			return (struct snd_soc_dai *)p->source->priv;
		be = tocodec_tdm_get_ahead_be(p->source);
		if (be && be->id == TDM_IFACE)
			return be;
	}
	return NULL;
}

static unsigned int aml_simple_strtoull(const char *cp)
{
	unsigned int result = 0;
	unsigned int value = 0;
	unsigned int len = strlen(cp);

	while (len != 0) {
		len--;
		value = isdigit(*cp);
		if (value) {
			value = *cp - '0';
		} else {
			cp++;
			continue;
		}
		cp++;
		result = result * 10 + value;
	}
	return result;
}

static int aml_get_clk_id(const char *name)
{
	int clk_id = 0;

	if (strstr(name, "mst_a"))
		clk_id = 0;
	else if (strstr(name, "mst_b"))
		clk_id = 1;
	else if (strstr(name, "mst_c"))
		clk_id = 2;
	else if (strstr(name, "mst_d"))
		clk_id = 3;
	else if (strstr(name, "mst_e"))
		clk_id = 4;
	else if (strstr(name, "mst_f"))
		clk_id = 5;

	return clk_id;
}

static int aml_tocodec_sel_set(struct snd_soc_dapm_widget *w)
{
	struct snd_soc_dai *be;
	struct axg_tdm_stream *stream;
	struct axg_tdm_iface *iface;
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);
	struct toacodec *priv = snd_soc_component_get_drvdata(component);
	unsigned int tdm_id = TDM_A_PAD;
	const char *dai_widget_name;
	struct snd_soc_dapm_path *p;
	unsigned int lane = 0;
	unsigned int val = 0;
	struct clk *sclk, *mclk;
	char *clk_name;
	int mclk_id, sclk_id;

	be = tocodec_tdm_get_ahead_be(w);
	if (!be) {
		dev_err(component->dev, "%s not find the be\n", __func__);
		return -EINVAL;
	}
	stream = snd_soc_dai_dma_data_get_playback(be);
	if (!stream) {
		dev_err(component->dev, "%s not find the stream\n", __func__);
		return -EINVAL;
	}
	/*we like to use dai id, but it is fixed val*/
	dai_widget_name = be->stream[SNDRV_PCM_STREAM_PLAYBACK].widget->name;
	if (strstr(dai_widget_name, "TDM_A"))
		tdm_id = TDM_A_PAD;
	else if (strstr(dai_widget_name, "TDM_B"))
		tdm_id = TDM_B_PAD;
	else if (strstr(dai_widget_name, "TDM_C"))
		tdm_id = TDM_C_PAD;
	snd_soc_dapm_widget_for_each_source_path(w, p) {
		if (p->connect && p->name) {
			lane = aml_simple_strtoull(p->name);
			val = lane + tdm_id * S4_LANE_OFFSET;
			regmap_field_write(priv->field_dat_sel, val);
		}
	}
	iface = stream->iface;
	mclk = iface->mclk;
	sclk = iface->sclk;
	mclk_id = aml_get_clk_id(__clk_get_name(mclk));
	sclk_id = aml_get_clk_id(__clk_get_name(sclk));
	regmap_field_write(priv->field_mclk_sel, mclk_id);
	regmap_field_write(priv->field_bclk_sel, sclk_id);
	regmap_field_write(priv->field_lrclk_sel, sclk_id);

	return 0;
}

static int tocodec_sel_event(struct snd_soc_dapm_widget *w,
			     struct snd_kcontrol *control,
			     int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);
	int ret = 0;

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		ret = aml_tocodec_sel_set(w);
		break;

	case SND_SOC_DAPM_PRE_PMD:
		break;

	default:
		dev_err(component->dev, "Unexpected event %d\n", event);
		return -EINVAL;
	}

	return ret;
}

static int tocodec_clk_enable(struct snd_soc_dapm_widget *w,
			      struct snd_kcontrol *control,
			      int event)
{
	int ret = 0;
	unsigned int mask = 0, val = 0;
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);

	snd_soc_component_update_bits(component, TOACODEC_CTRL0,
				      1 << CTRL0_BLK_CAP_INV_SHIFT, 1 << CTRL0_BLK_CAP_INV_SHIFT);
	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		mask = 1 << CTRL0_MCLK_ENABLE_SHIFT | 1 << CTRL0_BCLK_ENABLE_SHIFT;
		val = 1 << CTRL0_MCLK_ENABLE_SHIFT | 1 << CTRL0_BCLK_ENABLE_SHIFT;
		snd_soc_component_update_bits(component, TOACODEC_CTRL0, mask, val);
		break;
	case SND_SOC_DAPM_PRE_PMD:
		mask = 1 << CTRL0_MCLK_ENABLE_SHIFT | 1 << CTRL0_BCLK_ENABLE_SHIFT;
		val = 0 << CTRL0_MCLK_ENABLE_SHIFT | 0 << CTRL0_BCLK_ENABLE_SHIFT;
		snd_soc_component_update_bits(component, TOACODEC_CTRL0, mask, val);
		break;
	default:
		dev_err(component->dev, "Unexpected event %d\n", event);
		return -EINVAL;
	}

	return ret;
}

static const struct snd_soc_dapm_widget s4_toacodec_widgets[] = {
	SND_SOC_DAPM_MUX_E("Lane SRC", SND_SOC_NOPM, 0, 0,
			   &s4_tocodec_lane_sel, tocodec_sel_event,
			   (SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_PRE_PMD)),
	SND_SOC_DAPM_MUX("INPUT SRC", SND_SOC_NOPM, 0, 0, &s4_tocodec_src_sel),
	SND_SOC_DAPM_SWITCH_E("OUT EN", SND_SOC_NOPM, 0, 0,
			      &s4_toacodec_out_enable, tocodec_clk_enable,
				(SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_PRE_PMD)),
	SND_SOC_DAPM_AIF_IN("TDMA", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("TDMB", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("TDMC", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_OUT_DRV("Lane0", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_OUT_DRV("Lane1", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_OUT_DRV("Lane2", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_OUT_DRV("Lane3", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_OUT_DRV("Lane4", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_OUT_DRV("Lane5", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_OUT_DRV("Lane6", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_OUT_DRV("Lane7", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_OUTPUT("TDM_TO_ACODEC"),
};

static const struct snd_soc_dapm_route s4_tocodec_dapm_routes[] = {
	{ "INPUT SRC", "TDMA", "TDMA"},
	{ "INPUT SRC", "TDMB", "TDMB"},
	{ "INPUT SRC", "TDMC", "TDMC"},
	{ "Lane0", NULL, "INPUT SRC" },
	{ "Lane1", NULL, "INPUT SRC"},
	{ "Lane2", NULL, "INPUT SRC"},
	{ "Lane3", NULL, "INPUT SRC"},
	{ "Lane4", NULL, "INPUT SRC"},
	{ "Lane5", NULL, "INPUT SRC"},
	{ "Lane6", NULL, "INPUT SRC"},
	{ "Lane7", NULL, "INPUT SRC"},
	{ "Lane SRC", "Lane0", "Lane0"},
	{ "Lane SRC", "Lane1", "Lane1"},
	{ "Lane SRC", "Lane2", "Lane2"},
	{ "Lane SRC", "Lane3", "Lane3"},
	{ "Lane SRC", "Lane4", "Lane4"},
	{ "Lane SRC", "Lane5", "Lane5"},
	{ "Lane SRC", "Lane6", "Lane6"},
	{ "Lane SRC", "Lane7", "Lane7"},
	{ "OUT EN", "Switch", "Lane SRC"},
	{ "TDM_TO_ACODEC", NULL, "OUT EN"},

};

static const struct snd_soc_component_driver s4_tocodec_component_drv = {
	.dapm_widgets		= s4_toacodec_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(s4_toacodec_widgets),
	.dapm_routes		= s4_tocodec_dapm_routes,
	.num_dapm_routes	= ARRAY_SIZE(s4_tocodec_dapm_routes),
};

static const struct toacodec_match_data s4_toacodec_match_data = {
	.component_drv	= &s4_tocodec_component_drv,
	.field_dat_sel	= REG_FIELD(TOACODEC_CTRL0, 16, 20),
	.field_lrclk_sel = REG_FIELD(TOACODEC_CTRL0, 12, 14),
	.field_bclk_sel	= REG_FIELD(TOACODEC_CTRL0, 4, 6),
	.field_mclk_sel	= REG_FIELD(TOACODEC_CTRL0, 0, 2),
};

static const struct of_device_id s4_tocodec_of_match[] = {
	{
		.compatible = "amlogic,s4-tocodec",
		.data = &s4_toacodec_match_data,
	}, {}
};

MODULE_DEVICE_TABLE(of, s4_tocodec_of_match);

static int tocodec_probe(struct platform_device *pdev)
{
	const struct toacodec_match_data *data;
	struct device *dev = &pdev->dev;
	struct toacodec *priv;
	void __iomem *regs;
	struct regmap *map;
	int ret;

	data = device_get_match_data(dev);
	if (!data)
		return dev_err_probe(dev, -ENODEV, "failed to match device\n");
	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	platform_set_drvdata(pdev, priv);

	ret = device_reset(dev);
	if (ret)
		return ret;

	regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(regs))
		return PTR_ERR(regs);

	map = devm_regmap_init_mmio(dev, regs, &tocodec_regmap_cfg);
	if (IS_ERR(map))
		return dev_err_probe(dev, PTR_ERR(map), "failed to init regmap\n");

	priv->field_dat_sel = devm_regmap_field_alloc(dev, map, data->field_dat_sel);
	if (IS_ERR(priv->field_dat_sel))
		return PTR_ERR(priv->field_dat_sel);

	priv->field_lrclk_sel = devm_regmap_field_alloc(dev, map, data->field_lrclk_sel);
	if (IS_ERR(priv->field_lrclk_sel))
		return PTR_ERR(priv->field_lrclk_sel);

	priv->field_bclk_sel = devm_regmap_field_alloc(dev, map, data->field_bclk_sel);
	if (IS_ERR(priv->field_bclk_sel))
		return PTR_ERR(priv->field_bclk_sel);

	priv->field_mclk_sel = devm_regmap_field_alloc(dev, map, data->field_mclk_sel);
	if (IS_ERR(priv->field_mclk_sel))
		return PTR_ERR(priv->field_mclk_sel);

	return devm_snd_soc_register_component(dev,
			data->component_drv, NULL, 0);
}

static struct platform_driver tocodec_pdrv = {
	.probe = tocodec_probe,
	.driver = {
		.name = "s4-tocodec",
		.of_match_table = s4_tocodec_of_match,
	},
};

module_platform_driver(tocodec_pdrv);

MODULE_DESCRIPTION("Amlogic to codec driver");
MODULE_AUTHOR("jiebing.chen@amlogic.com");
MODULE_LICENSE("GPL");
