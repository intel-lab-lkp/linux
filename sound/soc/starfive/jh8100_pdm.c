// SPDX-License-Identifier: GPL-2.0
/*
 * PDM driver for the StarFive JH8100 SoC
 *
 * Copyright (C) 2023 StarFive Technology Co., Ltd.
 */
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <sound/dmaengine_pcm.h>
#include <sound/tlv.h>

/* PDM RES */
/* MODULE 0 */
#define JH8100_PDM_DMIC_CTRL0			0x00
#define JH8100_PDM_DC_SCALE0			0x04
/* MODULE 1 */
#define JH8100_PDM_DMIC_CTRL1			0x10
#define JH8100_PDM_DC_SCALE1			0x14
#define JH8100_PDM_MODULEX_SHIFT		0x10

/* PDM CTRL0/1 OFFSET */
#define JH8100_PDM_DMIC_MSB_SHIFT		1
#define JH8100_PDM_DMIC_MSB_MASK		GENMASK(3, 1)
#define JH8100_PDM_DMIC_VOL_MASK		GENMASK(21, 16)
#define JH8100_PDM_VOL_DB_MUTE			GENMASK(21, 16)
#define JH8100_PDM_VOL_DB_MAX			0

#define JH8100_PDM_DMIC_RVOL_MASK		BIT(22)
#define JH8100_PDM_DMIC_LVOL_MASK		BIT(23)
#define JH8100_PDM_DMIC_I2S_SLAVE		BIT(24)
#define JH8100_PDM_DMIC_HPF_EN			BIT(28)
#define JH8100_PDM_DMIC_FASTMODE_MASK		BIT(29)
#define JH8100_PDM_DMIC_DC_BYPASS_MASK		BIT(30)
#define JH8100_PDM_SW_RST_MASK			BIT(31)
#define JH8100_PDM_SW_RST_RELEASE		BIT(31)

/* PDM SCALE0/1 OFFSET */
#define JH8100_DMIC_DCOFF3_MASK			GENMASK(27, 24)
#define JH8100_DMIC_DCOFF3_DEF_VAL		GENMASK(27, 26)
#define JH8100_DMIC_DCOFF1_MASK			GENMASK(15, 8)
#define JH8100_DMIC_DCOFF1_SHIFT		8
#define JH8100_DMIC_DCOFF1_DEF_VAL		FIELD_PREP(JH8100_DMIC_DCOFF1_MASK, 5)
#define JH8100_DMIC_SCALE_MASK			GENMASK(5, 0)
#define JH8100_DMIC_SCALE_DEF_VAL		0x8

struct jh8100_pdm_priv {
	struct regmap *regmap;
	struct regmap *syscon_regmap;
	struct device *dev;
	struct clk *dmic_clk;
	struct clk *icg_clk;
	struct reset_control *rst;
	unsigned int syscon_args[2]; /* [0]: offset, [1]: mask */
};

static const DECLARE_TLV_DB_SCALE(volume_tlv, -9450, 150, 0);

static const struct snd_kcontrol_new jh8100_pdm_snd_controls[] = {
	SOC_SINGLE("DC compensation Control", JH8100_PDM_DMIC_CTRL0, 30, 1, 0),
	SOC_SINGLE("High Pass Filter Control", JH8100_PDM_DMIC_CTRL0, 28, 1, 0),
	SOC_SINGLE("Left Channel Volume Control", JH8100_PDM_DMIC_CTRL0, 23, 1, 0),
	SOC_SINGLE("Right Channel Volume Control", JH8100_PDM_DMIC_CTRL0, 22, 1, 0),
	SOC_SINGLE_TLV("Volume", JH8100_PDM_DMIC_CTRL0, 16, 0x3F, 1, volume_tlv),
	SOC_SINGLE("Data MSB Shift", JH8100_PDM_DMIC_CTRL0, 1, 7, 0),
	SOC_SINGLE("SCALE", JH8100_PDM_DC_SCALE0, 0, 0x3F, 0),
	SOC_SINGLE("DC offset", JH8100_PDM_DC_SCALE0, 8, 0xFFFFF, 0),
};

static void jh8100_pdm_enable(struct regmap *map)
{
	/* Left and Right Channel Volume Control Enable */
	regmap_update_bits(map, JH8100_PDM_DMIC_CTRL0, JH8100_PDM_DMIC_RVOL_MASK, 0);
	regmap_update_bits(map, JH8100_PDM_DMIC_CTRL0, JH8100_PDM_DMIC_LVOL_MASK, 0);
}

static void jh8100_pdm_disable(struct regmap *map)
{
	/* Left and Right Channel Volume Control Disable */
	regmap_update_bits(map, JH8100_PDM_DMIC_CTRL0,
			   JH8100_PDM_DMIC_RVOL_MASK, JH8100_PDM_DMIC_RVOL_MASK);
	regmap_update_bits(map, JH8100_PDM_DMIC_CTRL0,
			   JH8100_PDM_DMIC_LVOL_MASK, JH8100_PDM_DMIC_LVOL_MASK);
}

static int jh8100_pdm_dai_probe(struct snd_soc_dai *dai)
{
	struct jh8100_pdm_priv *priv = snd_soc_dai_get_drvdata(dai);

	/* Change I2SDIN source to PDM */
	regmap_update_bits(priv->syscon_regmap, priv->syscon_args[0],
			   priv->syscon_args[1], priv->syscon_args[1]);

	return 0;
}

static int jh8100_pdm_trigger(struct snd_pcm_substream *substream, int cmd,
			      struct snd_soc_dai *dai)
{
	struct jh8100_pdm_priv *priv = snd_soc_dai_get_drvdata(dai);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		jh8100_pdm_enable(priv->regmap);
		return 0;

	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		jh8100_pdm_disable(priv->regmap);
		return 0;

	default:
		return -EINVAL;
	}
}

static int jh8100_pdm_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params,
				struct snd_soc_dai *dai)
{
	struct jh8100_pdm_priv *priv = snd_soc_dai_get_drvdata(dai);

	/* set pdm_mclk,  PDM MCLK = 128 * LRCLK */
	return clk_set_rate(priv->dmic_clk, 128 * params_rate(params));
}

static const struct snd_soc_dai_ops jh8100_pdm_dai_ops = {
	.probe		= jh8100_pdm_dai_probe,
	.trigger	= jh8100_pdm_trigger,
	.hw_params	= jh8100_pdm_hw_params,
};

/* Use DMIC1 in PDM */
static void jh8100_pdm_module_init(struct jh8100_pdm_priv *priv)
{
	/* Reset */
	regmap_update_bits(priv->regmap, JH8100_PDM_DMIC_CTRL0,
			   JH8100_PDM_SW_RST_MASK, 0x00);
	regmap_update_bits(priv->regmap, JH8100_PDM_DMIC_CTRL0,
			   JH8100_PDM_SW_RST_MASK, JH8100_PDM_SW_RST_RELEASE);

	/* Make sure the device is initially disabled */
	jh8100_pdm_disable(priv->regmap);

	/* MUTE */
	regmap_update_bits(priv->regmap, JH8100_PDM_DMIC_CTRL0,
			   JH8100_PDM_DMIC_VOL_MASK, JH8100_PDM_VOL_DB_MUTE);

	/* UNMUTE */
	regmap_update_bits(priv->regmap, JH8100_PDM_DMIC_CTRL0,
			   JH8100_PDM_DMIC_VOL_MASK, JH8100_PDM_VOL_DB_MAX);

	/* enable high pass filter */
	regmap_update_bits(priv->regmap, JH8100_PDM_DMIC_CTRL0,
			   JH8100_PDM_DMIC_HPF_EN, JH8100_PDM_DMIC_HPF_EN);

	/* PDM work as slave mode */
	regmap_update_bits(priv->regmap, JH8100_PDM_DMIC_CTRL0,
			   JH8100_PDM_DMIC_I2S_SLAVE, JH8100_PDM_DMIC_I2S_SLAVE);

	/* enable fast mode */
	regmap_update_bits(priv->regmap, JH8100_PDM_DMIC_CTRL0,
			   JH8100_PDM_DMIC_FASTMODE_MASK, JH8100_PDM_DMIC_FASTMODE_MASK);

	/* default dmic msb shift 0 */
	regmap_update_bits(priv->regmap, JH8100_PDM_DMIC_CTRL0,
			   JH8100_PDM_DMIC_MSB_MASK, 0);

	/* default scale: 0x8 */
	regmap_update_bits(priv->regmap, JH8100_PDM_DC_SCALE0,
			   JH8100_DMIC_SCALE_MASK, JH8100_DMIC_SCALE_DEF_VAL);

	regmap_update_bits(priv->regmap, JH8100_PDM_DC_SCALE0,
			   JH8100_DMIC_DCOFF1_MASK, JH8100_DMIC_DCOFF1_DEF_VAL);

	regmap_update_bits(priv->regmap, JH8100_PDM_DC_SCALE0,
			   JH8100_DMIC_DCOFF3_MASK, JH8100_DMIC_DCOFF3_DEF_VAL);
}

#define JH8100_PDM_RATES	(SNDRV_PCM_RATE_8000 | \
				SNDRV_PCM_RATE_11025 | \
				SNDRV_PCM_RATE_16000)

#define JH8100_PDM_FORMATS	(SNDRV_PCM_FMTBIT_S16_LE | \
				SNDRV_PCM_FMTBIT_S32_LE)

static struct snd_soc_dai_driver jh8100_pdm_dai_drv = {
	.name = "PDM",
	.id = 0,
	.capture = {
		.stream_name	= "Capture",
		.channels_min	= 1,
		.channels_max	= 2,
		.rates		= JH8100_PDM_RATES,
		.formats	= JH8100_PDM_FORMATS,
	},
	.ops = &jh8100_pdm_dai_ops,
	.symmetric_rate = 1,
};

static int jh8100_pdm_component_probe(struct snd_soc_component *component)
{
	struct jh8100_pdm_priv *priv = snd_soc_component_get_drvdata(component);

	snd_soc_component_init_regmap(component, priv->regmap);
	snd_soc_add_component_controls(component, jh8100_pdm_snd_controls,
				       ARRAY_SIZE(jh8100_pdm_snd_controls));

	return 0;
}

static int jh8100_pdm_crg_enable(struct jh8100_pdm_priv *priv)
{
	int ret;

	ret = clk_prepare_enable(priv->icg_clk);
	if (ret)
		return dev_err_probe(priv->dev, ret, "failed to enable icg clock\n");

	ret = reset_control_deassert(priv->rst);
	if (ret) {
		dev_err(priv->dev, "failed to deassert pdm_apb\n");
		goto disable_icg;
	}

	return 0;

disable_icg:
	clk_disable_unprepare(priv->icg_clk);
	return ret;
}

#ifdef CONFIG_PM
static int jh8100_pdm_runtime_suspend(struct device *dev)
{
	struct jh8100_pdm_priv *priv = dev_get_drvdata(dev);

	clk_disable_unprepare(priv->icg_clk);
	return 0;
}

static int jh8100_pdm_runtime_resume(struct device *dev)
{
	struct jh8100_pdm_priv *priv = dev_get_drvdata(dev);

	return jh8100_pdm_crg_enable(priv);
}
#endif

#ifdef CONFIG_PM_SLEEP
static int jh8100_pdm_suspend(struct snd_soc_component *component)
{
	return pm_runtime_force_suspend(component->dev);
}

static int jh8100_pdm_resume(struct snd_soc_component *component)
{
	return pm_runtime_force_resume(component->dev);
}

#else
#define jh8100_pdm_suspend	NULL
#define jh8100_pdm_resume	NULL
#endif

static const struct snd_soc_component_driver jh8100_pdm_component_drv = {
	.name = "jh8100-pdm",
	.probe = jh8100_pdm_component_probe,
	.suspend = jh8100_pdm_suspend,
	.resume = jh8100_pdm_resume,
};

static const struct regmap_config jh8100_pdm_regmap_cfg = {
	.reg_bits	= 32,
	.val_bits	= 32,
	.reg_stride	= 4,
	.max_register	= 0x20,
};

static int jh8100_pdm_crg_init(struct jh8100_pdm_priv *priv)
{
	priv->dmic_clk = devm_clk_get(priv->dev, "dmic");
	if (IS_ERR(priv->dmic_clk))
		return dev_err_probe(priv->dev, PTR_ERR(priv->dmic_clk),
				     "failed to get dmic clock.\n");

	priv->icg_clk = devm_clk_get(priv->dev, "icg");
	if (IS_ERR(priv->icg_clk))
		return dev_err_probe(priv->dev, PTR_ERR(priv->icg_clk),
				     "failed to get icg clock.\n");

	priv->rst = devm_reset_control_get_exclusive(priv->dev, NULL);
	if (IS_ERR(priv->rst))
		return dev_err_probe(priv->dev, PTR_ERR(priv->rst), "failed to get pdm reset\n");

	return jh8100_pdm_crg_enable(priv);
}

static int jh8100_pdm_probe(struct platform_device *pdev)
{
	struct jh8100_pdm_priv *priv;
	void __iomem *base;
	int ret;
	u8 using_modulex;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	platform_set_drvdata(pdev, priv);

	if (!device_property_read_u8(&pdev->dev, "starfive,pdm-modulex", &using_modulex))
		if (using_modulex == 1)
			base += JH8100_PDM_MODULEX_SHIFT; /* Use module 1 */

	priv->regmap = devm_regmap_init_mmio(&pdev->dev, base, &jh8100_pdm_regmap_cfg);
	if (IS_ERR(priv->regmap))
		return dev_err_probe(&pdev->dev, PTR_ERR(priv->regmap),
				     "failed to init regmap\n");

	priv->dev = &pdev->dev;
	ret = jh8100_pdm_crg_init(priv);
	if (ret)
		return ret;

	priv->syscon_regmap = syscon_regmap_lookup_by_phandle_args(pdev->dev.of_node,
								   "starfive,syscon",
								   2, priv->syscon_args);
	if (IS_ERR(priv->syscon_regmap))
		return dev_err_probe(&pdev->dev, PTR_ERR(priv->syscon_regmap),
				     "get the syscon regmap failed\n");

	ret = devm_snd_soc_register_component(&pdev->dev, &jh8100_pdm_component_drv,
					      &jh8100_pdm_dai_drv, 1);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "failed to register pdm dai\n");

	jh8100_pdm_module_init(priv);

	pm_runtime_enable(&pdev->dev);
	if (pm_runtime_enabled(&pdev->dev))
		clk_disable_unprepare(priv->icg_clk);

	return 0;
}

static int jh8100_pdm_remove(struct platform_device *pdev)
{
	struct jh8100_pdm_priv *priv = platform_get_drvdata(pdev);

	/* Change I2SDIN source to default(PAD) */
	regmap_update_bits(priv->syscon_regmap, priv->syscon_args[0],
			   priv->syscon_args[1], 0);
	pm_runtime_disable(&pdev->dev);

	return 0;
}

static const struct of_device_id jh8100_pdm_of_match[] = {
	{.compatible = "starfive,jh8100-pdm",},
	{}
};
MODULE_DEVICE_TABLE(of, jh8100_pdm_of_match);

static const struct dev_pm_ops jh8100_pdm_pm_ops = {
	SET_RUNTIME_PM_OPS(jh8100_pdm_runtime_suspend,
			   jh8100_pdm_runtime_resume, NULL)
};

static struct platform_driver jh8100_pdm_driver = {
	.driver = {
		.name = "jh8100-pdm",
		.of_match_table = jh8100_pdm_of_match,
		.pm = &jh8100_pdm_pm_ops,
	},
	.probe = jh8100_pdm_probe,
	.remove = jh8100_pdm_remove,
};
module_platform_driver(jh8100_pdm_driver);

MODULE_AUTHOR("Xingyu Wu <xingyu.wu@starfivetech.com>");
MODULE_AUTHOR("Walker Chen <walker.chen@starfivetech.com>");
MODULE_DESCRIPTION("StarFive JH8100 PDM Controller Driver");
MODULE_LICENSE("GPL");
