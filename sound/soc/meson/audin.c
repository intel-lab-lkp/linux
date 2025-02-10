// SPDX-License-Identifier: GPL-2.0
//
// Copyright (c) 2025 BayLibre, SAS.
// Author: Valerio Setti <vsetti@baylibre.com>

#include <linux/clk.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <sound/soc.h>
#include <sound/soc-dai.h>
#include <dt-bindings/sound/meson-audin.h>

#include "audin.h"

static const char * const audin_fifo_input_sel_texts[] = {
	"SPDIF", "I2S", "PCM", "HDMI", "Demodulator"
};

static SOC_ENUM_SINGLE_DECL(audin_fifo0_input_sel_enum, AUDIN_FIFO0_CTRL,
			    AUDIN_FIFO_CTRL_DIN_SEL_OFF,
			    audin_fifo_input_sel_texts);

static const struct snd_kcontrol_new audin_fifo0_input_sel_mux =
	SOC_DAPM_ENUM("FIFO0 SRC SEL", audin_fifo0_input_sel_enum);

static SOC_ENUM_SINGLE_DECL(audin_fifo1_input_sel_enum, AUDIN_FIFO1_CTRL,
			    AUDIN_FIFO_CTRL_DIN_SEL_OFF,
			    audin_fifo_input_sel_texts);

static const struct snd_kcontrol_new audin_fifo1_input_sel_mux =
	SOC_DAPM_ENUM("FIFO1 SRC SEL", audin_fifo1_input_sel_enum);

static SOC_ENUM_SINGLE_DECL(audin_fifo2_input_sel_enum, AUDIN_FIFO2_CTRL,
			    AUDIN_FIFO_CTRL_DIN_SEL_OFF,
			    audin_fifo_input_sel_texts);

static const struct snd_kcontrol_new audin_fifo2_input_sel_mux =
	SOC_DAPM_ENUM("FIFO2 SRC SEL", audin_fifo2_input_sel_enum);

static const struct snd_soc_dapm_widget audin_cpu_dapm_widgets[] = {
	SND_SOC_DAPM_MUX("FIFO0 SRC SEL", SND_SOC_NOPM, 0, 0,
			 &audin_fifo0_input_sel_mux),
	SND_SOC_DAPM_MUX("FIFO1 SRC SEL", SND_SOC_NOPM, 0, 0,
			 &audin_fifo1_input_sel_mux),
	SND_SOC_DAPM_MUX("FIFO2 SRC SEL", SND_SOC_NOPM, 0, 0,
			 &audin_fifo2_input_sel_mux),
};

static const struct snd_soc_dapm_route audin_cpu_dapm_routes[] = {
	{ "FIFO0 SRC SEL", "I2S", "I2S Decoder Capture" },
	{ "FIFO1 SRC SEL", "I2S", "I2S Decoder Capture" },
	{ "FIFO2 SRC SEL", "I2S", "I2S Decoder Capture" },
	{ "TODDR 0 Capture", NULL, "FIFO0 SRC SEL" },
	{ "TODDR 1 Capture", NULL, "FIFO1 SRC SEL" },
	{ "TODDR 2 Capture", NULL, "FIFO2 SRC SEL" },
};

static int audin_cpu_of_xlate_dai_name(struct snd_soc_component *component,
				       const struct of_phandle_args *args,
				       const char **dai_name)
{
	struct snd_soc_dai *dai;
	int id;

	if (args->args_count != 1) {
		dev_err(component->dev, "Wrong number of arguments %d\n",
			args->args_count);
		return -EINVAL;
	}

	id = args->args[0];

	if (id < 0 || id >= component->num_dai) {
		dev_err(component->dev, "Invalid ID %d\n", id);
		return -EINVAL;
	}

	for_each_component_dais(component, dai) {
		if (id == 0)
			break;
		id--;
	}

	*dai_name = dai->driver->name;

	return 0;
}

static int audin_cpu_component_probe(struct snd_soc_component *component)
{
	struct audin *audin = snd_soc_component_get_drvdata(component);

	/* Required for the FIFO Source control operation */
	return clk_prepare_enable(audin->bulk_clks[INPUT].clk);
}

static void audin_cpu_component_remove(struct snd_soc_component *component)
{
	struct audin *audin = snd_soc_component_get_drvdata(component);

	clk_disable_unprepare(audin->bulk_clks[INPUT].clk);
}

static const struct snd_soc_component_driver audin_cpu_component = {
	.name			= "AUDIN CPU",
	.dapm_widgets		= audin_cpu_dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(audin_cpu_dapm_widgets),
	.dapm_routes		= audin_cpu_dapm_routes,
	.num_dapm_routes	= ARRAY_SIZE(audin_cpu_dapm_routes),
	.of_xlate_dai_name	= audin_cpu_of_xlate_dai_name,
	.pointer		= audin_toddr_pointer,
	.probe			= audin_cpu_component_probe,
	.remove			= audin_cpu_component_remove,
#ifdef CONFIG_DEBUG_FS
	.debugfs_prefix		= "audin-cpu",
#endif
};

static struct snd_soc_dai_driver audin_cpu_dai_drv[] = {
	[CPU_AUDIN_TODDR_0] = {
		.name = "TODDR 0",
		.capture = {
			.stream_name	= "TODDR 0 Capture",
			.channels_min	= 2,
			.channels_max	= 2,
			.rates		= SNDRV_PCM_RATE_CONTINUOUS,
			.rate_min	= 5512,
			.rate_max	= 192000,
			.formats	= AUDIN_FORMATS,
		},
		.ops = &audin_toddr_dai_ops,
	},
	[CPU_AUDIN_TODDR_1] = {
		.name = "TODDR 1",
		.capture = {
			.stream_name	= "TODDR 1 Capture",
			.channels_min	= 2,
			.channels_max	= 2,
			.rates		= SNDRV_PCM_RATE_CONTINUOUS,
			.rate_min	= 5512,
			.rate_max	= 192000,
			.formats	= AUDIN_FORMATS,
		},
		.ops = &audin_toddr_dai_ops,
	},
	[CPU_AUDIN_TODDR_2] = {
		.name = "TODDR 2",
		.capture = {
			.stream_name	= "TODDR 2 Capture",
			.channels_min	= 2,
			.channels_max	= 2,
			.rates		= SNDRV_PCM_RATE_CONTINUOUS,
			.rate_min	= 5512,
			.rate_max	= 192000,
			.formats	= AUDIN_FORMATS,
		},
		.ops = &audin_toddr_dai_ops,
	},
	[CPU_I2S_DECODER] = {
		.name = "I2S Decoder",
		.capture = {
			.stream_name = "I2S Decoder Capture",
			.channels_min = 2,
			.channels_max = 2,
			.rates = SNDRV_PCM_RATE_8000_192000,
			.formats = AUDIN_FORMATS,
		},
		.ops = &audin_decoder_i2s_dai_ops,
	},
};

static const struct regmap_config audin_regmap_cfg = {
	.reg_bits	= 32,
	.val_bits	= 32,
	.reg_stride	= 4,
	.max_register	= 0x308,
};

static const char * const clk_bulk_ids[] = {
	"i2s_pclk",
	"i2s_aoclk",
	"i2s_mclk",
	"i2s_mixer",
	"i2s_input_clk",
};

static int audin_clk_single_get(struct device *dev, const unsigned char *id,
				bool enable, struct clk **clk)
{
	*clk = devm_clk_get(dev, id);
	if (IS_ERR(*clk)) {
		dev_err(dev, "Failed to get %s clock %ld\n", id, PTR_ERR(*clk));
		return PTR_ERR(*clk);
	}

	if (enable)
		return clk_prepare_enable(*clk);

	return 0;
}

static int audin_clk_get(struct device *dev)
{
	struct audin *audin = dev_get_drvdata(dev);
	struct clk *pclk;
	int i, ret;

	ret = audin_clk_single_get(dev, "pclk", true, &pclk);
	if (ret)
		return ret;

	audin->bulk_clks_num = ARRAY_SIZE(clk_bulk_ids);
	audin->bulk_clks = devm_kcalloc(dev, audin->bulk_clks_num,
					sizeof(struct clk_bulk_data),
					GFP_KERNEL);
	if (!audin->bulk_clks)
		return -ENOMEM;

	for (i = 0; i < ARRAY_SIZE(clk_bulk_ids); i++)
		audin->bulk_clks[i].id = clk_bulk_ids[i];

	ret = devm_clk_bulk_get(dev, ARRAY_SIZE(clk_bulk_ids),
				audin->bulk_clks);
	if (ret) {
		dev_err(dev, "Failed to get bulk clocks %d\n", ret);
		return ret;
	}

	ret = audin_clk_single_get(dev, "i2s_aoclk_div_gate", false,
				   &audin->aoclk_div_gate);
	if (ret)
		return ret;

	ret = audin_clk_single_get(dev, "i2s_aoclk_basic_div", false,
				   &audin->aoclk_basic_div);
	if (ret)
		return ret;

	ret = audin_clk_single_get(dev, "i2s_aoclk_more_div", false,
				   &audin->aoclk_more_div);
	if (ret)
		return ret;

	ret = audin_clk_single_get(dev, "i2s_lrclk_div", false,
				   &audin->lrclk_div);
	if (ret)
		return ret;

	return 0;
}

static int audin_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	void __iomem *regs;
	struct regmap *map;
	struct audin *audin;
	int ret;

	audin = devm_kzalloc(dev, sizeof(*audin), GFP_KERNEL);
	if (!audin)
		return -ENOMEM;

	platform_set_drvdata(pdev, audin);

	ret = device_reset(dev);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to reset device\n");

	regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(regs))
		return PTR_ERR(regs);

	map = devm_regmap_init_mmio(dev, regs, &audin_regmap_cfg);
	if (IS_ERR(map)) {
		dev_err(dev, "failed to init regmap: %ld\n",
			PTR_ERR(map));
		return PTR_ERR(map);
	}

	ret = audin_clk_get(dev);
	if (ret)
		return ret;

	ret = snd_soc_register_component(dev, &audin_cpu_component,
					 audin_cpu_dai_drv,
					 ARRAY_SIZE(audin_cpu_dai_drv));
	if (ret) {
		dev_err(dev, "Failed to register cpu component\n");
		return ret;
	}

	return 0;
}

static void audin_remove(struct platform_device *pdev)
{
	snd_soc_unregister_component(&pdev->dev);
}

static const struct of_device_id audin_of_match[] = {
	{ .compatible = "amlogic,audin-gxbb", .data = NULL },
	{}
};
MODULE_DEVICE_TABLE(of, audin_of_match);

static struct platform_driver audin_pdrv = {
	.probe = audin_probe,
	.remove = audin_remove,
	.driver = {
		.name = "meson-audin",
		.of_match_table = audin_of_match,
	},
};
module_platform_driver(audin_pdrv);

MODULE_DESCRIPTION("Meson AUDIN Driver");
MODULE_AUTHOR("Valerio Setti <vsetti@baylibre.com>");
MODULE_LICENSE("GPL v2");
