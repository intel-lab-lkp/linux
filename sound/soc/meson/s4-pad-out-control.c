// SPDX-License-Identifier: (GPL-2.0-only OR MIT)
/*
 * Copyright (C) 2024 Amlogic, Inc. All rights reserved
 */

#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/regmap.h>
#include <sound/soc.h>
#include <sound/soc-dai.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include<linux/kstrtox.h>

#include "axg-tdm.h"

static const struct regmap_config tdmout_pad_regmap_cfg = {
	.reg_bits	= 32,
	.val_bits	= 32,
	.reg_stride	= 4,
	.max_register	= 0x28,
};

#define TDM_IFACE 0
#define TDM_A_PAD 0
#define TDM_B_PAD 1
#define TDM_C_PAD 2

#define EE_AUDIO_DAT_PAD_CTRL6 0x0
#define EE_AUDIO_DAT_PAD_CTRL7 0x4
#define EE_AUDIO_DAT_PAD_CTRL8 0x8
#define EE_AUDIO_DAT_PAD_CTRL9 0xc
#define EE_AUDIO_DAT_PAD_CTRLA 0x10
#define EE_AUDIO_DAT_PAD_CTRLB 0x14
#define EE_AUDIO_DAT_PAD_CTRLC 0x1c
#define EE_AUDIO_DAT_PAD_CTRLD 0x20
#define EE_AUDIO_DAT_PAD_CTRLE 0x24
#define EE_AUDIO_DAT_PAD_CTRLF 0x28

#define REG_OFFSET 4

static const char * const s4_tdmout_sel_texts[] = {
	"TDM_D0", "TDM_D1", "TDM_D2", "TDM_D3", "TDM_D4", "TDM_D5", "TDM_D6", "TDM_D7",
	"TDM_D8", "TDM_D9", "TDM_D10", "TDM_D11", "TDM_D12", "TDM_D13", "TDM_D14", "TDM_D15",
	"TDM_D16", "TDM_D17", "TDM_D18", "TDM_D19", "TDM_D20", "TDM_D21", "TDM_D22", "TDM_D23",
	"TDM_D24", "TDM_D25", "TDM_D26", "TDM_D27", "TDM_D28", "TDM_D29", "TDM_D30", "TDM_D31"
};

static const struct soc_enum tdmout_sel_enum =
	SOC_ENUM_SINGLE(SND_SOC_NOPM, 0, ARRAY_SIZE(s4_tdmout_sel_texts),
			s4_tdmout_sel_texts);

static struct snd_soc_dai *tdm_get_ahead_be(struct snd_soc_dapm_widget *w)
{
	struct snd_soc_dapm_path *p;
	struct snd_soc_dai *be;

	snd_soc_dapm_widget_for_each_source_path(w, p) {
		if (p->source->id == snd_soc_dapm_dai_in)
			return (struct snd_soc_dai *)p->source->priv;
		be = tdm_get_ahead_be(p->source);
		if (be && be->id == TDM_IFACE)
			return be;
	}
	return NULL;
}

#define SND_SOC_DAPM_DEMUX_E(wname, wreg, wshift, winvert, wcontrols, \
	wevent, wflags) \
((struct snd_soc_dapm_widget) { \
	.id = snd_soc_dapm_demux, .name = wname, \
	SND_SOC_DAPM_INIT_REG_VAL(wreg, wshift, winvert), \
	.kcontrol_news = wcontrols, .num_kcontrols = 1, \
	.event = wevent, .event_flags = wflags})

static const struct snd_kcontrol_new tdmout_sel_demux[] = {
	SOC_DAPM_ENUM("TDMOUTA SEL", tdmout_sel_enum),
	SOC_DAPM_ENUM("TDMOUTB SEL", tdmout_sel_enum),
	SOC_DAPM_ENUM("TDMOUTC SEL", tdmout_sel_enum),
};

static unsigned int aml_simple_strtoull(const char *cp)
{
	unsigned int result = 0;
	unsigned int value = 0;
	unsigned int len =  strlen(cp);

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

static int tdm_out_pad_set(struct snd_soc_dapm_widget *w)
{
	struct snd_soc_dai *be;
	struct axg_tdm_stream *stream;
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);
	unsigned int tdm_id = TDM_A_PAD;
	const char *dai_widget_name;
	struct snd_soc_dapm_path *p;
	unsigned int lane_num = 0;
	unsigned long pin = 0;
	unsigned int reg, mask, val = 0;
	int lane_cnt;

	be = tdm_get_ahead_be(w);
	if (!be) {
		dev_err(component->dev, "%s not find the be\n", __func__);
		return -EINVAL;
	}
	stream = snd_soc_dai_dma_data_get_playback(be);
	if (!stream) {
		dev_err(component->dev, "%s not find the stream\n", __func__);
		return -EINVAL;
	}
	lane_cnt = (stream->channels - 1) / stream->iface->slots + 1;
	/*we like to use dai id, but it is fixed val*/
	dai_widget_name = be->stream[SNDRV_PCM_STREAM_PLAYBACK].widget->name;
	if (strstr(dai_widget_name, "TDM_A"))
		tdm_id = TDM_A_PAD;
	else if (strstr(dai_widget_name, "TDM_B"))
		tdm_id = TDM_B_PAD;
	else if (strstr(dai_widget_name, "TDM_C"))
		tdm_id = TDM_C_PAD;
	else
		dev_err(component->dev, "%s not find the be dai widget\n", __func__);
	dev_dbg(component->dev, "tdm_id:%d, channel:%d, slot:%d\n",
		tdm_id, stream->channels, stream->iface->slots);
	snd_soc_dapm_widget_for_each_sink_path(w, p) {
		if (p->sink->id == snd_soc_dapm_output) {
			if (p->connect) {
				pin = aml_simple_strtoull(p->name);
				reg = (pin / 4) * REG_OFFSET;
				/*calculate mask pos */
				mask = 0x1f << ((pin % 4) * 8);
				val = tdm_id * 8 + lane_num;
				snd_soc_component_update_bits(component, reg, mask, val);
				snd_soc_component_update_bits(component, EE_AUDIO_DAT_PAD_CTRLF,
							      0x1 << pin, 0 << pin);
				lane_num++;
				if (lane_num > lane_cnt - 1)
					break;
			}
		}
	}
	return 0;
}

static int tdmout_sel_pad_event(struct snd_soc_dapm_widget *w,
				struct snd_kcontrol *control,
				int event)
{
	int ret = 0;
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		tdm_out_pad_set(w);
		break;

	case SND_SOC_DAPM_PRE_PMD:
		break;

	default:
		dev_err(component->dev, "Unexpected event %d\n", event);
		return -EINVAL;
	}

	return ret;
}

static const struct snd_soc_dapm_widget s4_tdmout_pad_dapm_widgets[] = {
	SND_SOC_DAPM_DEMUX_E("TDMA_OUT SEL", SND_SOC_NOPM, 0, 0,
			     &tdmout_sel_demux[TDM_A_PAD], tdmout_sel_pad_event,
			   (SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_PRE_PMD)),
	SND_SOC_DAPM_DEMUX_E("TDMB_OUT SEL", SND_SOC_NOPM, 0, 0,
			     &tdmout_sel_demux[TDM_B_PAD], tdmout_sel_pad_event,
			   (SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_PRE_PMD)),
	SND_SOC_DAPM_DEMUX_E("TDMC_OUT SEL", SND_SOC_NOPM, 0, 0,
			     &tdmout_sel_demux[TDM_C_PAD], tdmout_sel_pad_event,
			   (SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_PRE_PMD)),
	SND_SOC_DAPM_OUTPUT("TDM_D0"),
	SND_SOC_DAPM_OUTPUT("TDM_D1"),
	SND_SOC_DAPM_OUTPUT("TDM_D2"),
	SND_SOC_DAPM_OUTPUT("TDM_D3"),
	SND_SOC_DAPM_OUTPUT("TDM_D4"),
	SND_SOC_DAPM_OUTPUT("TDM_D5"),
	SND_SOC_DAPM_OUTPUT("TDM_D6"),
	SND_SOC_DAPM_OUTPUT("TDM_D7"),
	SND_SOC_DAPM_OUTPUT("TDM_D8"),
	SND_SOC_DAPM_OUTPUT("TDM_D9"),
	SND_SOC_DAPM_OUTPUT("TDM_D10"),
	SND_SOC_DAPM_OUTPUT("TDM_D11"),
	SND_SOC_DAPM_OUTPUT("TDM_D12"),
	SND_SOC_DAPM_OUTPUT("TDM_D13"),
	SND_SOC_DAPM_OUTPUT("TDM_D14"),
	SND_SOC_DAPM_OUTPUT("TDM_D15"),
	SND_SOC_DAPM_OUTPUT("TDM_D16"),
	SND_SOC_DAPM_OUTPUT("TDM_D17"),
	SND_SOC_DAPM_OUTPUT("TDM_D18"),
	SND_SOC_DAPM_OUTPUT("TDM_D19"),
	SND_SOC_DAPM_OUTPUT("TDM_D20"),
	SND_SOC_DAPM_OUTPUT("TDM_D21"),
	SND_SOC_DAPM_OUTPUT("TDM_D22"),
	SND_SOC_DAPM_OUTPUT("TDM_D23"),
	SND_SOC_DAPM_OUTPUT("TDM_D24"),
	SND_SOC_DAPM_OUTPUT("TDM_D25"),
	SND_SOC_DAPM_OUTPUT("TDM_D26"),
	SND_SOC_DAPM_OUTPUT("TDM_D27"),
	SND_SOC_DAPM_OUTPUT("TDM_D28"),
	SND_SOC_DAPM_OUTPUT("TDM_D29"),
	SND_SOC_DAPM_OUTPUT("TDM_D30"),
	SND_SOC_DAPM_OUTPUT("TDM_D31"),
};

static const struct snd_soc_dapm_route s4_tdmout_pad_dapm_routes[] = {
	{ "TDM_D0", "TDM_D0", "TDMA_OUT SEL" },
	{ "TDM_D1", "TDM_D1", "TDMA_OUT SEL" },
	{ "TDM_D2", "TDM_D2", "TDMA_OUT SEL" },
	{ "TDM_D3", "TDM_D3", "TDMA_OUT SEL" },
	{ "TDM_D4", "TDM_D4", "TDMA_OUT SEL" },
	{ "TDM_D5", "TDM_D5", "TDMA_OUT SEL" },
	{ "TDM_D6", "TDM_D6", "TDMA_OUT SEL" },
	{ "TDM_D7", "TDM_D7", "TDMA_OUT SEL" },
	{ "TDM_D8", "TDM_D8", "TDMA_OUT SEL" },
	{ "TDM_D9", "TDM_D9", "TDMA_OUT SEL" },
	{ "TDM_D10", "TDM_D10", "TDMA_OUT SEL" },
	{ "TDM_D11", "TDM_D11", "TDMA_OUT SEL" },
	{ "TDM_D12", "TDM_D12", "TDMA_OUT SEL" },
	{ "TDM_D13", "TDM_D13", "TDMA_OUT SEL" },
	{ "TDM_D14", "TDM_D14", "TDMA_OUT SEL" },
	{ "TDM_D15", "TDM_D15", "TDMA_OUT SEL" },
	{ "TDM_D16", "TDM_D16", "TDMA_OUT SEL" },
	{ "TDM_D17", "TDM_D17", "TDMA_OUT SEL" },
	{ "TDM_D18", "TDM_D18", "TDMA_OUT SEL" },
	{ "TDM_D19", "TDM_D19", "TDMA_OUT SEL" },
	{ "TDM_D20", "TDM_D20", "TDMA_OUT SEL" },
	{ "TDM_D21", "TDM_D21", "TDMA_OUT SEL" },
	{ "TDM_D22", "TDM_D22", "TDMA_OUT SEL" },
	{ "TDM_D23", "TDM_D23", "TDMA_OUT SEL" },
	{ "TDM_D24", "TDM_D24", "TDMA_OUT SEL" },
	{ "TDM_D25", "TDM_D25", "TDMA_OUT SEL" },
	{ "TDM_D26", "TDM_D26", "TDMA_OUT SEL" },
	{ "TDM_D27", "TDM_D27", "TDMA_OUT SEL" },
	{ "TDM_D28", "TDM_D28", "TDMA_OUT SEL" },
	{ "TDM_D29", "TDM_D29", "TDMA_OUT SEL" },
	{ "TDM_D30", "TDM_D30", "TDMA_OUT SEL" },
	{ "TDM_D31", "TDM_D31", "TDMA_OUT SEL" },
	{ "TDM_D0", "TDM_D0", "TDMB_OUT SEL" },
	{ "TDM_D1", "TDM_D1", "TDMB_OUT SEL" },
	{ "TDM_D2", "TDM_D2", "TDMB_OUT SEL" },
	{ "TDM_D3", "TDM_D3", "TDMB_OUT SEL" },
	{ "TDM_D4", "TDM_D4", "TDMB_OUT SEL" },
	{ "TDM_D5", "TDM_D5", "TDMB_OUT SEL" },
	{ "TDM_D6", "TDM_D6", "TDMB_OUT SEL" },
	{ "TDM_D7", "TDM_D7", "TDMB_OUT SEL" },
	{ "TDM_D8", "TDM_D8", "TDMB_OUT SEL" },
	{ "TDM_D9", "TDM_D9", "TDMB_OUT SEL" },
	{ "TDM_D10", "TDM_D10", "TDMB_OUT SEL" },
	{ "TDM_D11", "TDM_D11", "TDMB_OUT SEL" },
	{ "TDM_D12", "TDM_D12", "TDMB_OUT SEL" },
	{ "TDM_D13", "TDM_D13", "TDMB_OUT SEL" },
	{ "TDM_D14", "TDM_D14", "TDMB_OUT SEL" },
	{ "TDM_D15", "TDM_D15", "TDMB_OUT SEL" },

	{ "TDM_D16", "TDM_D16", "TDMB_OUT SEL" },
	{ "TDM_D17", "TDM_D17", "TDMB_OUT SEL" },
	{ "TDM_D18", "TDM_D18", "TDMB_OUT SEL" },
	{ "TDM_D19", "TDM_D19", "TDMB_OUT SEL" },
	{ "TDM_D20", "TDM_D20", "TDMB_OUT SEL" },
	{ "TDM_D21", "TDM_D21", "TDMB_OUT SEL" },
	{ "TDM_D22", "TDM_D22", "TDMB_OUT SEL" },
	{ "TDM_D23", "TDM_D23", "TDMB_OUT SEL" },
	{ "TDM_D24", "TDM_D24", "TDMB_OUT SEL" },
	{ "TDM_D25", "TDM_D25", "TDMB_OUT SEL" },
	{ "TDM_D26", "TDM_D26", "TDMB_OUT SEL" },
	{ "TDM_D27", "TDM_D27", "TDMB_OUT SEL" },
	{ "TDM_D28", "TDM_D28", "TDMB_OUT SEL" },
	{ "TDM_D29", "TDM_D29", "TDMB_OUT SEL" },
	{ "TDM_D30", "TDM_D30", "TDMB_OUT SEL" },
	{ "TDM_D31", "TDM_D31", "TDMB_OUT SEL" },
	{ "TDM_D0", "TDM_D0", "TDMC_OUT SEL" },
	{ "TDM_D1", "TDM_D1", "TDMC_OUT SEL" },
	{ "TDM_D2", "TDM_D2", "TDMC_OUT SEL" },
	{ "TDM_D3", "TDM_D3", "TDMC_OUT SEL" },
	{ "TDM_D4", "TDM_D4", "TDMC_OUT SEL" },
	{ "TDM_D5", "TDM_D5", "TDMC_OUT SEL" },
	{ "TDM_D6", "TDM_D6", "TDMC_OUT SEL" },
	{ "TDM_D7", "TDM_D7", "TDMC_OUT SEL" },
	{ "TDM_D8", "TDM_D8", "TDMC_OUT SEL" },
	{ "TDM_D9", "TDM_D9", "TDMC_OUT SEL" },
	{ "TDM_D10", "TDM_D10", "TDMC_OUT SEL" },
	{ "TDM_D11", "TDM_D11", "TDMC_OUT SEL" },
	{ "TDM_D12", "TDM_D12", "TDMC_OUT SEL" },
	{ "TDM_D13", "TDM_D13", "TDMC_OUT SEL" },
	{ "TDM_D14", "TDM_D14", "TDMC_OUT SEL" },
	{ "TDM_D15", "TDM_D15", "TDMC_OUT SEL" },
	{ "TDM_D16", "TDM_D16", "TDMC_OUT SEL" },
	{ "TDM_D17", "TDM_D17", "TDMC_OUT SEL" },
	{ "TDM_D18", "TDM_D18", "TDMC_OUT SEL" },
	{ "TDM_D19", "TDM_D19", "TDMC_OUT SEL" },
	{ "TDM_D20", "TDM_D20", "TDMC_OUT SEL" },
	{ "TDM_D21", "TDM_D21", "TDMC_OUT SEL" },
	{ "TDM_D22", "TDM_D22", "TDMC_OUT SEL" },
	{ "TDM_D23", "TDM_D23", "TDMC_OUT SEL" },
	{ "TDM_D24", "TDM_D24", "TDMC_OUT SEL" },
	{ "TDM_D25", "TDM_D25", "TDMC_OUT SEL" },
	{ "TDM_D26", "TDM_D26", "TDMC_OUT SEL" },
	{ "TDM_D27", "TDM_D27", "TDMC_OUT SEL" },
	{ "TDM_D28", "TDM_D28", "TDMC_OUT SEL" },
	{ "TDM_D29", "TDM_D29", "TDMC_OUT SEL" },
	{ "TDM_D30", "TDM_D30", "TDMC_OUT SEL" },
	{ "TDM_D31", "TDM_D31", "TDMC_OUT SEL" },
};

static const struct snd_soc_component_driver s4_tdmout_pad_component_drv = {
	.dapm_widgets		= s4_tdmout_pad_dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(s4_tdmout_pad_dapm_widgets),
	.dapm_routes		= s4_tdmout_pad_dapm_routes,
	.num_dapm_routes	= ARRAY_SIZE(s4_tdmout_pad_dapm_routes),

};

static const struct of_device_id s4_tdmout_pad_of_match[] = {
	{
		.compatible = "amlogic,s4-tdmout-pad",
	}, {}
};

MODULE_DEVICE_TABLE(of, s4_tdmout_pad_of_match);

static int tdm_pad_out_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct regmap *map;
	void __iomem *regs;

	regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(regs))
		return PTR_ERR(regs);

	map = devm_regmap_init_mmio(dev, regs, &tdmout_pad_regmap_cfg);
	if (IS_ERR(map))
		return dev_err_probe(dev, PTR_ERR(map), "failed to init regmap\n");

	return devm_snd_soc_register_component(dev, &s4_tdmout_pad_component_drv,
					       NULL, 0);
}

static struct platform_driver tdmout_pad_pdrv = {
	.probe = tdm_pad_out_probe,
	.driver = {
		.name = "s4-pad-out",
		.of_match_table = s4_tdmout_pad_of_match,
	},
};

module_platform_driver(tdmout_pad_pdrv);

MODULE_DESCRIPTION("Amlogic TDM PAD DRIVER");
MODULE_AUTHOR("jiebing.chen@amlogic.com");
MODULE_LICENSE("GPL");
