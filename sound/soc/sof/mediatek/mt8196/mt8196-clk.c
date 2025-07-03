// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
/*
 * Copyright(c) 2025 MediaTek Inc. All rights reserved.
 * Author: Hailong Fan <hailong.fan@mediatek.com>
 */

// Hardware interface for mt8196 DSP clock

#include <linux/clk.h>
#include <linux/io.h>

#include "../../sof-audio.h"
#include "../../ops.h"
#include "../adsp_helper.h"
#include "mt8196.h"
#include "mt8196-clk.h"

static const char *adsp_clks[ADSP_CLK_MAX] = {
	[CLK_TOP_ADSP_SEL] = "clk_top_adsp_sel",
	[CLK_TOP_CLK26M] = "clk_top_clk26m",
	[CLK_TOP_ADSPPLL] = "clk_top_adsppll",

};

int mt8196_adsp_init_clock(struct snd_sof_dev *sdev)
{
	struct adsp_priv *priv = sdev->pdata->hw_pdata;
	struct device *dev = sdev->dev;
	int i;

	priv->clk = devm_kcalloc(dev, ADSP_CLK_MAX, sizeof(*priv->clk), GFP_KERNEL);
	if (!priv->clk)
		return -ENOMEM;

	for (i = 0; i < ADSP_CLK_MAX; i++) {
		priv->clk[i] = devm_clk_get(dev, adsp_clks[i]);

		if (IS_ERR(priv->clk[i]))
			return PTR_ERR(priv->clk[i]);
	}

	return 0;
}

static int adsp_enable_all_clock(struct snd_sof_dev *sdev)
{
	struct adsp_priv *priv = sdev->pdata->hw_pdata;
	struct device *dev = sdev->dev;
	int ret;

	ret = clk_prepare_enable(priv->clk[CLK_TOP_ADSP_SEL]);
	if (ret) {
		dev_err(dev, "%s clk_prepare_enable(audiodsp) fail %d\n",
			__func__, ret);
		return ret;
	}

	return 0;
}

static void adsp_disable_all_clock(struct snd_sof_dev *sdev)
{
	struct adsp_priv *priv = sdev->pdata->hw_pdata;

	clk_disable_unprepare(priv->clk[CLK_TOP_ADSP_SEL]);
}

int mt8196_adsp_clock_on(struct snd_sof_dev *sdev)
{
	struct device *dev = sdev->dev;
	int ret;

	ret = adsp_enable_all_clock(sdev);
	if (ret) {
		dev_err(dev, "failed to adsp_enable_clock: %d\n", ret);
		return ret;
	}

	ret = snd_sof_dsp_update_bits(sdev, DSP_REG_BAR, ADSP_CK_EN,
				      UART_BT_EN | DMA_AXI_EN | DMA1_EN,
				      UART_BT_EN | DMA_AXI_EN | DMA1_EN);
	if (ret < 0)
		dev_err(sdev->dev, "Failed to update CK register: %d\n", ret);

	ret = snd_sof_dsp_update_bits(sdev, DSP_REG_BAR, ADSP_UART_CTRL,
				      UART_BCLK_CG | UART_RSTN,
				      UART_BCLK_CG | UART_RSTN);
	if (ret < 0)
		dev_err(sdev->dev, "Failed to update UART register: %d\n", ret);

	return 0;
}

void mt8196_adsp_clock_off(struct snd_sof_dev *sdev)
{
	snd_sof_dsp_write(sdev, DSP_REG_BAR, ADSP_UART_CTRL, 0);
	adsp_disable_all_clock(sdev);
}
