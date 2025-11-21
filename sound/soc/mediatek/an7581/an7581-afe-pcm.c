// SPDX-License-Identifier: GPL-2.0
/*
 * Airoha ALSA SoC AFE platform driver for AN7581
 *
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/pm_runtime.h>

#include "an7581-afe-common.h"
#include "an7581-reg.h"
#include "../common/mtk-afe-platform-driver.h"
#include "../common/mtk-afe-fe-dai.h"

enum {
	ARH_AFE_RATE_8K = 0,
	ARH_AFE_RATE_12K = 1,
	ARH_AFE_RATE_16K = 2,
	ARH_AFE_RATE_24K = 3,
	ARH_AFE_RATE_32K = 4,
	ARH_AFE_RATE_48K = 5,
	ARH_AFE_RATE_96K = 6,
	ARH_AFE_RATE_192K = 7,
	ARH_AFE_RATE_384K = 8,
	ARH_AFE_RATE_7K = 16,
	ARH_AFE_RATE_11K = 17,
	ARH_AFE_RATE_14K = 18,
	ARH_AFE_RATE_22K = 19,
	ARH_AFE_RATE_29K = 20,
	ARH_AFE_RATE_44K = 21,
	ARH_AFE_RATE_88K = 22,
	ARH_AFE_RATE_176K = 23,
	ARH_AFE_RATE_352K = 24,
};

unsigned int an7581_afe_rate_transform(struct device *dev, unsigned int rate)
{
	switch (rate) {
	case 7350:
		return ARH_AFE_RATE_7K;
	case 8000:
		return ARH_AFE_RATE_8K;
	case 11025:
		return ARH_AFE_RATE_11K;
	case 12000:
		return ARH_AFE_RATE_12K;
	case 14700:
		return ARH_AFE_RATE_14K;
	case 16000:
		return ARH_AFE_RATE_16K;
	case 22050:
		return ARH_AFE_RATE_22K;
	case 24000:
		return ARH_AFE_RATE_24K;
	case 29400:
		return ARH_AFE_RATE_29K;
	case 32000:
		return ARH_AFE_RATE_32K;
	case 44100:
		return ARH_AFE_RATE_44K;
	case 48000:
		return ARH_AFE_RATE_48K;
	case 88200:
		return ARH_AFE_RATE_88K;
	case 96000:
		return ARH_AFE_RATE_96K;
	case 176400:
		return ARH_AFE_RATE_176K;
	case 192000:
		return ARH_AFE_RATE_192K;
	case 352800:
		return ARH_AFE_RATE_352K;
	case 384000:
		return ARH_AFE_RATE_384K;
	default:
		dev_warn(dev, "%s(), rate %u invalid, using %d!!!\n",
			 __func__, rate, ARH_AFE_RATE_48K);
		return ARH_AFE_RATE_48K;
	}
}

static const int an7581_memif_specified_irqs[AN7581_MEMIF_NUM] = {
	[AN7581_MEMIF_DL1] = AN7581_IRQ_0,
	[AN7581_MEMIF_UL1] = AN7581_IRQ_1,
};

static const struct snd_pcm_hardware an7581_afe_hardware = {
	.info = SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_MMAP_VALID,
	.formats = SNDRV_PCM_FMTBIT_S16_LE |
		   SNDRV_PCM_FMTBIT_S24_LE |
		   SNDRV_PCM_FMTBIT_S32_LE,
	.period_bytes_min = 512,
	.period_bytes_max = 128 * 1024,
	.periods_min = 2,
	.periods_max = 256,
	.buffer_bytes_max = 256 * 1024,
	.fifo_size = 0,
};

static int an7581_memif_fs(struct snd_pcm_substream *substream,
			   unsigned int rate)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_component *component = snd_soc_rtdcom_lookup(rtd, AFE_PCM_NAME);
	struct mtk_base_afe *afe = snd_soc_component_get_drvdata(component);

	return an7581_afe_rate_transform(afe->dev, rate);
}

static int an7581_irq_fs(struct snd_pcm_substream *substream,
			 unsigned int rate)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_component *component = snd_soc_rtdcom_lookup(rtd, AFE_PCM_NAME);
	struct mtk_base_afe *afe = snd_soc_component_get_drvdata(component);

	return an7581_afe_rate_transform(afe->dev, rate);
}

#define ARH_PCM_FORMATS (SNDRV_PCM_FMTBIT_S16_LE |\
			 SNDRV_PCM_FMTBIT_S32_LE)

static struct snd_soc_dai_driver an7581_memif_dai_driver[] = {
	/* FE DAIs: memory intefaces to CPU */
	{
		.name = "DL1",
		.id = AN7581_MEMIF_DL1,
		.playback = {
			.stream_name = "DL1",
			.channels_min = 1,
			.channels_max = 2,
			.rates = SNDRV_PCM_RATE_8000_192000,
			.formats = ARH_PCM_FORMATS,
		},
		.ops = &mtk_afe_fe_ops,
	},
	{
		.name = "UL1",
		.id = AN7581_MEMIF_UL1,
		.capture = {
			.stream_name = "UL1",
			.channels_min = 1,
			.channels_max = 2,
			.rates = SNDRV_PCM_RATE_8000_192000,
			.formats = ARH_PCM_FORMATS,
		},
		.ops = &mtk_afe_fe_ops,
	},
};

static const struct snd_soc_component_driver an7581_afe_pcm_dai_component = {
	.name = "an7581-afe-pcm-dai",
};

static const struct mtk_base_memif_data memif_data[AN7581_MEMIF_NUM] = {
	[AN7581_MEMIF_DL1] = {
		.name = "DL1",
		.id = AN7581_MEMIF_DL1,
		.reg_ofs_base = AFE_DL1_BASE,
		.reg_ofs_cur = AFE_DL1_CUR,
		.reg_ofs_end = AFE_DL1_END,
		.fs_reg = -1,
		.fs_shift = -1,
		.fs_maskbit = -1,
		.mono_reg = -1,
		.mono_shift = -1,
		.hd_reg = -1,
		.hd_shift = -1,
		.enable_reg = AFE_DAC_CON0,
		.enable_shift = 17,
		.msb_reg = -1,
		.msb_shift = -1,
		.agent_disable_reg = -1,
		.agent_disable_shift = -1,
	},
	[AN7581_MEMIF_UL1] = {
		.name = "UL1",
		.id = AN7581_MEMIF_UL1,
		.reg_ofs_base = AFE_UL1_BASE,
		.reg_ofs_cur = AFE_UL1_CUR,
		.reg_ofs_end = AFE_UL1_END,
		.fs_reg = -1,
		.fs_shift = -1,
		.fs_maskbit = -1,
		.mono_reg = -1,
		.mono_shift = -1,
		.hd_reg = -1,
		.hd_shift = -1,
		.enable_reg = AFE_DAC_CON0,
		.enable_shift = 1,
		.msb_reg = -1,
		.msb_shift = -1,
		.agent_disable_reg = -1,
		.agent_disable_shift = -1,
	},
};

static const struct mtk_base_irq_data irq_data[AN7581_IRQ_NUM] = {
	[AN7581_IRQ_0] = {
		.id = AN7581_IRQ_0,
		.irq_cnt_reg = -1,
		.irq_cnt_shift = -1,
		.irq_cnt_maskbit = -1,
		.irq_en_reg = AFE_IRQ0_CON0,
		.irq_en_shift = 0,
		.irq_fs_reg = -1,
		.irq_fs_shift = -1,
		.irq_fs_maskbit = -1,
		.irq_clr_reg = AFE_IRQ0_CON0,
		.irq_clr_shift = 2,
	},
	[AN7581_IRQ_1] = {
		.id = AN7581_IRQ_1,
		.irq_cnt_reg = -1,
		.irq_cnt_shift = -1,
		.irq_cnt_maskbit = -1,
		.irq_en_reg = AFE_IRQ1_CON0,
		.irq_en_shift = 0,
		.irq_fs_reg = -1,
		.irq_fs_shift = -1,
		.irq_fs_maskbit = -1,
		.irq_clr_reg = AFE_IRQ1_CON0,
		.irq_clr_shift = 2,
	},
};

static const struct regmap_config an7581_afe_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.max_register = AFE_MAX_REGISTER,
	.num_reg_defaults_raw = ((AFE_MAX_REGISTER / 4) + 1),
};

static irqreturn_t an7581_afe_irq_handler(int irq_id, void *dev)
{
	struct mtk_base_afe *afe = dev;
	struct mtk_base_afe_irq *irq;
	u32 status;
	u32 reg;
	int i;

	regmap_read(afe->regmap, AFE_IRQ_STS, &status);

	if (status & AFE_IRQ_STS_RECORD)
		reg = AFE_IRQ0_CON0;
	else
		reg = AFE_IRQ1_CON0;

	regmap_set_bits(afe->regmap, reg, BIT(2));
	regmap_clear_bits(afe->regmap, reg, BIT(2));

	regmap_set_bits(afe->regmap, reg, BIT(3));
	regmap_clear_bits(afe->regmap, reg, BIT(3));

	for (i = 0; i < AN7581_MEMIF_NUM; i++) {
		struct mtk_base_afe_memif *memif = &afe->memif[i];

		if (!memif->substream)
			continue;

		if (memif->irq_usage < 0)
			continue;

		irq = &afe->irqs[memif->irq_usage];

		if (status & (1 << irq->irq_data->irq_clr_shift))
			snd_pcm_period_elapsed(memif->substream);
	}

	return IRQ_HANDLED;
}

static int an7581_afe_runtime_suspend(struct device *dev)
{
	return 0;
}

static int an7581_afe_runtime_resume(struct device *dev)
{
	return 0;
}

static int an7581_dai_memif_register(struct mtk_base_afe *afe)
{
	struct mtk_base_afe_dai *dai;

	dai = devm_kzalloc(afe->dev, sizeof(*dai), GFP_KERNEL);
	if (!dai)
		return -ENOMEM;

	list_add(&dai->list, &afe->sub_dais);

	dai->dai_drivers = an7581_memif_dai_driver;
	dai->num_dai_drivers = ARRAY_SIZE(an7581_memif_dai_driver);

	return 0;
}

typedef int (*dai_register_cb)(struct mtk_base_afe *);
static const dai_register_cb dai_register_cbs[] = {
	an7581_dai_etdm_register,
	an7581_dai_memif_register,
};

static int an7581_afe_pcm_dev_probe(struct platform_device *pdev)
{
	struct mtk_base_afe *afe;
	struct an7581_afe_private *afe_priv;
	struct device *dev;
	int i, irq_id, ret;

	afe = devm_kzalloc(&pdev->dev, sizeof(*afe), GFP_KERNEL);
	if (!afe)
		return -ENOMEM;
	platform_set_drvdata(pdev, afe);

	afe->platform_priv = devm_kzalloc(&pdev->dev, sizeof(*afe_priv),
					  GFP_KERNEL);
	if (!afe->platform_priv)
		return -ENOMEM;

	afe_priv = afe->platform_priv;
	afe->dev = &pdev->dev;
	dev = afe->dev;

	afe->base_addr = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(afe->base_addr))
		return PTR_ERR(afe->base_addr);

	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;

	pm_runtime_get_sync(&pdev->dev);

	afe->regmap = devm_regmap_init_mmio(&pdev->dev, afe->base_addr,
					    &an7581_afe_regmap_config);

	pm_runtime_put_sync(&pdev->dev);
	if (IS_ERR(afe->regmap))
		return PTR_ERR(afe->regmap);

	mutex_init(&afe->irq_alloc_lock);

	/* irq initialize */
	afe->irqs_size = AN7581_IRQ_NUM;
	afe->irqs = devm_kcalloc(dev, afe->irqs_size, sizeof(*afe->irqs),
				 GFP_KERNEL);
	if (!afe->irqs)
		return -ENOMEM;

	for (i = 0; i < afe->irqs_size; i++)
		afe->irqs[i].irq_data = &irq_data[i];

	/* request irq */
	irq_id = platform_get_irq(pdev, 0);
	if (irq_id < 0)
		return irq_id;

	ret = devm_request_irq(dev, irq_id, an7581_afe_irq_handler,
			       IRQF_TRIGGER_NONE, "asys-isr", (void *)afe);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to request irq for asys-isr\n");

	/* init memif */
	afe->memif_size = AN7581_MEMIF_NUM;
	afe->memif = devm_kcalloc(dev, afe->memif_size, sizeof(*afe->memif),
				  GFP_KERNEL);
	if (!afe->memif)
		return -ENOMEM;

	for (i = 0; i < afe->memif_size; i++) {
		int sel_irq = an7581_memif_specified_irqs[i];

		afe->memif[i].data = &memif_data[i];
		afe->memif[i].irq_usage = sel_irq;
		afe->memif[i].const_irq = 1;
		afe->irqs[sel_irq].irq_occupyed = true;
	}

	/* init sub_dais */
	INIT_LIST_HEAD(&afe->sub_dais);

	for (i = 0; i < ARRAY_SIZE(dai_register_cbs); i++) {
		ret = dai_register_cbs[i](afe);
		if (ret)
			return dev_err_probe(dev, ret, "DAI register failed, i: %d\n", i);
	}

	/* init dai_driver and component_driver */
	ret = mtk_afe_combine_sub_dai(afe);
	if (ret)
		return dev_err_probe(dev, ret, "mtk_afe_combine_sub_dai fail\n");

	afe->mtk_afe_hardware = &an7581_afe_hardware;
	afe->memif_fs = an7581_memif_fs;
	afe->irq_fs = an7581_irq_fs;

	afe->runtime_resume = an7581_afe_runtime_resume;
	afe->runtime_suspend = an7581_afe_runtime_suspend;

	/* register component */
	ret = devm_snd_soc_register_component(&pdev->dev,
					      &mtk_afe_pcm_platform,
					      NULL, 0);
	if (ret)
		return dev_err_probe(dev, ret, "Cannot register AFE component\n");

	ret = devm_snd_soc_register_component(afe->dev,
					      &an7581_afe_pcm_dai_component,
					      afe->dai_drivers,
					      afe->num_dai_drivers);
	if (ret)
		return dev_err_probe(dev, ret, "Cannot register PCM DAI component\n");

	return 0;
}

static void an7581_afe_pcm_dev_remove(struct platform_device *pdev)
{
	pm_runtime_disable(&pdev->dev);
	if (!pm_runtime_status_suspended(&pdev->dev))
		an7581_afe_runtime_suspend(&pdev->dev);
}

static const struct of_device_id an7581_afe_pcm_dt_match[] = {
	{ .compatible = "airoha,an7581-afe" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, an7581_afe_pcm_dt_match);

static const struct dev_pm_ops an7581_afe_pm_ops = {
	RUNTIME_PM_OPS(an7581_afe_runtime_suspend,
		       an7581_afe_runtime_resume, NULL)
};

static struct platform_driver an7581_afe_pcm_driver = {
	.driver = {
		   .name = "an7581-audio",
		   .of_match_table = an7581_afe_pcm_dt_match,
		   .pm = pm_ptr(&an7581_afe_pm_ops),
	},
	.probe = an7581_afe_pcm_dev_probe,
	.remove = an7581_afe_pcm_dev_remove,
};
module_platform_driver(an7581_afe_pcm_driver);

MODULE_DESCRIPTION("Airoha SoC AFE platform driver for ALSA AN7581");
MODULE_LICENSE("GPL");
