// SPDX-License-Identifier: GPL-2.0
/*
 * Cadence Multi-Channel I2S controller driver on the StarFive JH8100 SoC
 *
 * Copyright (c) 2023 StarFive Technology Co., Ltd.
 */

#include <linux/bitfield.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "cdns-jh8100-i2s.h"

static void cdns_jh8100_i2s_set_fifo_mask(struct cdns_jh8100_i2s_dev *i2s, u32 type)
{
	unsigned int temp = readl(i2s->base + CDNS_JH8100_CID_CTRL);

	temp &= ~CDNS_JH8100_I2S_IT_ALL;
	temp |= type;
	writel(temp, i2s->base + CDNS_JH8100_CID_CTRL);
}

static inline void cdns_jh8100_i2s_clear_int(struct cdns_jh8100_i2s_dev *i2s)
{
	writel(0, i2s->base + CDNS_JH8100_I2S_INTR_STAT);
}

static int cdns_jh8100_i2s_reset_mask(struct cdns_jh8100_i2s_dev *i2s, u32 mask)
{
	unsigned int val = readl(i2s->base + CDNS_JH8100_I2S_CTRL);

	val &= ~mask;
	writel(val, i2s->base + CDNS_JH8100_I2S_CTRL);

	/* Wait for the reset bit to done and is set to 1 */
	return readl_poll_timeout_atomic(i2s->base + CDNS_JH8100_I2S_CTRL, val,
					 (val & mask), 0,
					 CDNS_JH8100_FIFO_ACK_TIMEOUT_US);
}

/* Reset for TX and RX control unit  */
static void cdns_jh8100_i2s_reset_txrx_unit(struct cdns_jh8100_i2s_dev *i2s)
{
	unsigned int val = readl(i2s->base + CDNS_JH8100_I2S_CTRL);

	val |= CDNS_JH8100_I2S_CTRL_TXRX_RST;
	writel(val, i2s->base + CDNS_JH8100_I2S_CTRL);
}

static void cdns_jh8100_i2s_set_ms_mode(struct cdns_jh8100_i2s_dev *i2s)
{
	unsigned int val = readl(i2s->base + CDNS_JH8100_I2S_CTRL);

	val &= ~(CDNS_JH8100_I2S_CTRL_T_MS_MASK | CDNS_JH8100_I2S_CTRL_R_MS_MASK);
	val |= (FIELD_PREP(CDNS_JH8100_I2S_CTRL_T_MS_MASK, i2s->tx_sync_ms_mode) |
		FIELD_PREP(CDNS_JH8100_I2S_CTRL_R_MS_MASK, i2s->rx_sync_ms_mode));

	writel(val, i2s->base + CDNS_JH8100_I2S_CTRL);
}

/* The threshold of almost empty & full config */
static void cdns_jh8100_i2s_set_aempty_afull_th(struct cdns_jh8100_i2s_dev *i2s,
						unsigned int aempty,
						unsigned int afull)
{
	unsigned int val = aempty | (afull << CDNS_TRFIFO_CTRL_AFULL_THRESHOLD_SHIFT);

	writel(val, i2s->base + CDNS_JH8100_TFIFO_CTRL);
	writel(val, i2s->base + CDNS_JH8100_RFIFO_CTRL);
}

static void cdns_jh8100_i2s_set_channel_strobes(struct cdns_jh8100_i2s_dev *i2s,
						u32 ch, bool strobe)
{
	unsigned int val = readl(i2s->base + CDNS_JH8100_CID_CTRL);

	/* Active Low */
	if (strobe)
		val &= ~ch;
	else
		val |= ch;

	writel(val, i2s->base + CDNS_JH8100_CID_CTRL);
}

/* Enable TX or RX clock */
static void cdns_jh8100_i2s_enable_clock(struct cdns_jh8100_i2s_dev *i2s,
					 bool is_rx)
{
	unsigned int val = readl(i2s->base + CDNS_JH8100_CID_CTRL);
	unsigned int mask = (is_rx ? CDNS_JH8100_CID_CTRL_STROBE_TX :
			     CDNS_JH8100_CID_CTRL_STROBE_RX);

	/* Active Low */
	val &= ~mask;
	writel(val, i2s->base + CDNS_JH8100_CID_CTRL);
}

static void cdns_jh8100_i2s_set_transmitter_receiver(struct cdns_jh8100_i2s_dev *i2s,
						     u32 ch, bool is_transmit)
{
	unsigned int val = readl(i2s->base + CDNS_JH8100_I2S_CTRL);

	/* 1: Transmitter, 0: Receiver */
	if (is_transmit)
		val |= (ch << CDNS_JH8100_I2S_CTRL_TR_CFG_0_SHIFT);
	else
		val &= ~(ch << CDNS_JH8100_I2S_CTRL_TR_CFG_0_SHIFT);

	writel(val, i2s->base + CDNS_JH8100_I2S_CTRL);
}

static irqreturn_t cdns_jh8100_i2s_irq_handler(int irq, void *data)
{
	struct cdns_jh8100_i2s_dev *i2s = data;
	unsigned int val = readl(i2s->base + CDNS_JH8100_I2S_INTR_STAT);
	irqreturn_t ret = IRQ_NONE;

	cdns_jh8100_i2s_clear_int(i2s);

	if (val & CDNS_JH8100_I2S_STAT_TX_UNDERRUN)
		dev_err(i2s->dev, "TX underrun on channel %ld!\n",
			FIELD_GET(CDNS_JH8100_I2S_STAT_UNDERR_CODE, val));

	if (val & CDNS_JH8100_I2S_STAT_RX_OVERRUN)
		dev_err(i2s->dev, "RX overrun on channel %ld!\n",
			FIELD_GET(CDNS_JH8100_I2S_STAT_OVERR_CODE, val));

	/* FIFO is empty when playback start and I2S also need to push the data. */
	if (val & (CDNS_JH8100_I2S_STAT_TFIFO_AEMPTY | CDNS_JH8100_I2S_STAT_TFIFO_EMPTY)) {
		cdns_jh8100_i2s_pcm_push_tx(i2s);
		ret = IRQ_HANDLED;
	}

	if (val & CDNS_JH8100_I2S_STAT_RFIFO_AFULL) {
		cdns_jh8100_i2s_pcm_pop_rx(i2s);
		ret = IRQ_HANDLED;
	}

	return ret;
}

static void cdns_jh8100_i2s_enable_channel(struct cdns_jh8100_i2s_dev *i2s,
					   u32 ch, bool enable)
{
	unsigned int val = readl(i2s->base + CDNS_JH8100_I2S_CTRL);

	/* Active High */
	if (enable)
		val |= ch;
	else
		val &= ~ch;

	writel(val, i2s->base + CDNS_JH8100_I2S_CTRL);
}

/* Bit masking all interrupt requests */
static void cdns_jh8100_i2s_set_all_irq_mask(struct cdns_jh8100_i2s_dev *i2s, bool mask)
{
	unsigned int val = readl(i2s->base + CDNS_JH8100_CID_CTRL);

	/* Active Low: IRQ are masked */
	if (mask)
		val &= ~CDNS_JH8100_CID_CTRL_INTREQ_MASK;
	else
		val |= CDNS_JH8100_CID_CTRL_INTREQ_MASK;

	writel(val, i2s->base + CDNS_JH8100_CID_CTRL);
}

static void cdns_jh8100_i2s_enable_channel_int(struct cdns_jh8100_i2s_dev *i2s,
					       u32 ch, bool enable)
{
	unsigned int val = readl(i2s->base + CDNS_JH8100_CID_CTRL);

	/* Active High */
	if (enable)
		val |= (ch << CDNS_JH8100_CID_CTRL_I2S_MASK_0_SHIFT);
	else
		val &= ~(ch << CDNS_JH8100_CID_CTRL_I2S_MASK_0_SHIFT);

	writel(val, i2s->base + CDNS_JH8100_CID_CTRL);
}

static void cdns_jh8100_i2s_channel_start(struct cdns_jh8100_i2s_dev *i2s,
					  u32 ch, bool is_transmit)
{
	cdns_jh8100_i2s_set_transmitter_receiver(i2s, ch, is_transmit);
	cdns_jh8100_i2s_enable_channel(i2s, ch, true);
	cdns_jh8100_i2s_set_channel_strobes(i2s, ch, true);
	if (i2s->irq >= 0)
		cdns_jh8100_i2s_enable_channel_int(i2s, ch, true);
}

static void cdns_jh8100_i2s_channel_stop(struct cdns_jh8100_i2s_dev *i2s, u32 ch)
{
	cdns_jh8100_i2s_enable_channel(i2s, ch, false);
	if (i2s->irq >= 0)
		cdns_jh8100_i2s_enable_channel_int(i2s, ch, false);
}

static int cdns_jh8100_i2s_start(struct cdns_jh8100_i2s_dev *i2s,
				 struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	unsigned char max_ch = i2s->max_channels;
	unsigned char i2s_ch;
	int i;

	/* Each channel is stereo */
	i2s_ch = runtime->channels / 2;
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		if ((i2s_ch + i2s->rx_using_channels) > max_ch) {
			dev_err(i2s->dev,
				"Max %d channels: using %d for RX, do not support %d for TX\n",
				max_ch, i2s->rx_using_channels, i2s_ch);
			return -ENOMEM;
		}

		i2s->tx_using_channels = i2s_ch;
		/* Enable channels from 0 to 'max_ch' as tx */
		for (i = 0; i < i2s_ch; i++)
			cdns_jh8100_i2s_channel_start(i2s, CDNS_JH8100_I2S_CM_0 << i,
						      CDNS_JH8100_I2S_TC_TRANSMITTER);

	} else {
		if ((i2s_ch + i2s->tx_using_channels) > max_ch) {
			dev_err(i2s->dev,
				"Max %d channels: using %d for TX, do not support %d for RX\n",
				max_ch, i2s->tx_using_channels, i2s_ch);
			return -ENOMEM;
		}

		i2s->rx_using_channels = i2s_ch;
		/* Enable channels from 'max_ch' to 0 as rx */
		for (i = (max_ch - 1); i > (max_ch - i2s_ch - 1); i--) {
			if (i < 0)
				return -EINVAL;

			cdns_jh8100_i2s_channel_start(i2s, CDNS_JH8100_I2S_CM_0 << i,
						      CDNS_JH8100_I2S_TC_RECEIVER);
		}
	}
	cdns_jh8100_i2s_enable_clock(i2s, substream->stream);

	if (i2s->irq >= 0)
		cdns_jh8100_i2s_set_all_irq_mask(i2s, false);

	cdns_jh8100_i2s_clear_int(i2s);

	return 0;
}

static int cdns_jh8100_i2s_stop(struct cdns_jh8100_i2s_dev *i2s,
				struct snd_pcm_substream *substream)
{
	unsigned char i2s_ch;
	int i;

	cdns_jh8100_i2s_clear_int(i2s);

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		i2s_ch = i2s->tx_using_channels;
		for (i = 0; i < i2s_ch; i++)
			cdns_jh8100_i2s_channel_stop(i2s, (CDNS_JH8100_I2S_CM_0 << i));

		i2s->tx_using_channels = 0;
	} else {
		unsigned char max_ch = i2s->max_channels;

		i2s_ch = i2s->rx_using_channels;
		for (i = (max_ch - 1); i > (max_ch - i2s_ch - 1); i--) {
			if (i < 0)
				return -EINVAL;

			cdns_jh8100_i2s_channel_stop(i2s, (CDNS_JH8100_I2S_CM_0 << i));
		}

		i2s->rx_using_channels = 0;
	}

	if (i2s->irq >= 0 && !i2s->tx_using_channels && !i2s->rx_using_channels)
		cdns_jh8100_i2s_set_all_irq_mask(i2s, true);

	return 0;
}

static int cdns_jh8100_i2s_startup(struct snd_pcm_substream *substream,
				   struct snd_soc_dai *dai)
{
	struct cdns_jh8100_i2s_dev *i2s = snd_soc_dai_get_drvdata(dai);
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai_link *dai_link = rtd->dai_link;

	if (i2s->irq < 0)
		dai_link->trigger_stop = SND_SOC_TRIGGER_ORDER_LDC;

	return 0;
}

static void cdns_jh8100_i2s_config(struct cdns_jh8100_i2s_dev *i2s, int stream)
{
	unsigned int val = readl(i2s->base + CDNS_JH8100_I2S_SRR);

	if (stream == SNDRV_PCM_STREAM_PLAYBACK) {
		val &= ~(CDNS_JH8100_I2S_SRR_TRATE_MASK | CDNS_JH8100_I2S_SRR_TRESOLUTION_MASK);
		val |= (FIELD_PREP(CDNS_JH8100_I2S_SRR_TRATE_MASK, i2s->sample_rate_param) |
			FIELD_PREP(CDNS_JH8100_I2S_SRR_TRESOLUTION_MASK, (i2s->resolution - 1)));
	} else {
		val &= ~(CDNS_JH8100_I2S_SRR_RRATE_MASK | CDNS_JH8100_I2S_SRR_RRESOLUTION_MASK);
		val |= (FIELD_PREP(CDNS_JH8100_I2S_SRR_RRATE_MASK, i2s->sample_rate_param) |
			FIELD_PREP(CDNS_JH8100_I2S_SRR_RRESOLUTION_MASK, (i2s->resolution - 1)));
	}

	writel(val, i2s->base + CDNS_JH8100_I2S_SRR);
}

static int cdns_jh8100_i2s_hw_params(struct snd_pcm_substream *substream,
				     struct snd_pcm_hw_params *params,
				     struct snd_soc_dai *dai)
{
	struct cdns_jh8100_i2s_dev *i2s = snd_soc_dai_get_drvdata(dai);
	unsigned int sample_rate = params_rate(params);
	unsigned int channels = params_channels(params);
	unsigned int fclk_hz = clk_get_rate(i2s->clks[2].clk); /* mclk_inner */
	unsigned int bclk_rate;
	int ret;
	struct snd_dmaengine_dai_dma_data *dma_data;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		dma_data = &i2s->tx_dma_data;
	else
		dma_data = &i2s->rx_dma_data;

	switch (sample_rate) {
	case 8000:
		bclk_rate = 512000;
		break;
	case 11025:
		bclk_rate = 705600;
		break;
	case 16000:
		bclk_rate = 1024000;
		break;
	case 22050:
		bclk_rate = 1411200;
		break;
	case 32000:
		bclk_rate = 2048000;
		break;
	case 44100:
		bclk_rate = 2822400;
		break;
	case 48000:
		bclk_rate = 3072000;
		break;
	default:
		dev_err(dai->dev, "%d rate not supported\n", sample_rate);
		return -EINVAL;
	}

	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S16_LE:
		dma_data->addr_width = DMA_SLAVE_BUSWIDTH_2_BYTES;
		break;
	case SNDRV_PCM_FORMAT_S32_LE:
		dma_data->addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
		break;
	default:
		dev_err(i2s->dev, "unsupported PCM fmt\n");
		return -EINVAL;
	}

	ret = clk_set_rate(i2s->clks[0].clk, bclk_rate); /* bclk */
	if (ret < 0) {
		dev_err(i2s->dev, "Can't set i2s bclk: %d\n", ret);
		return ret;
	}

	i2s->resolution = params_width(params);
	i2s->sample_rate_param = fclk_hz / (sample_rate * channels * 32);
	cdns_jh8100_i2s_config(i2s, substream->stream);

	if (i2s->irq < 0)
		snd_soc_dai_set_dma_data(dai, substream, dma_data);

	return 0;
}

static int cdns_jh8100_i2s_trigger(struct snd_pcm_substream *substream,
				   int cmd, struct snd_soc_dai *dai)
{
	struct cdns_jh8100_i2s_dev *i2s = snd_soc_dai_get_drvdata(dai);
	int ret = 0;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		ret = cdns_jh8100_i2s_start(i2s, substream);
		break;

	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		ret = cdns_jh8100_i2s_stop(i2s, substream);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int cdns_jh8100_i2s_set_fmt(struct snd_soc_dai *cpu_dai,
				   unsigned int fmt)
{
	struct cdns_jh8100_i2s_dev *i2s = snd_soc_dai_get_drvdata(cpu_dai);
	int ret = 0;

	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBM_CFM:
		i2s->tx_sync_ms_mode = CDNS_JH8100_I2S_MASTER_MODE;
		i2s->rx_sync_ms_mode = CDNS_JH8100_I2S_MASTER_MODE;
		cdns_jh8100_i2s_set_ms_mode(i2s);
		break;
	case SND_SOC_DAIFMT_CBS_CFS:
		i2s->tx_sync_ms_mode = CDNS_JH8100_I2S_SLAVE_MODE;
		i2s->rx_sync_ms_mode = CDNS_JH8100_I2S_SLAVE_MODE;
		cdns_jh8100_i2s_set_ms_mode(i2s);
		break;
	case SND_SOC_DAIFMT_CBM_CFS:
	case SND_SOC_DAIFMT_CBS_CFM:
		ret = -EINVAL;
		break;
	default:
		dev_dbg(i2s->dev, "Invalid master/slave format\n");
		ret = -EINVAL;
		break;
	}
	return ret;
}

static int cdns_jh8100_i2s_dai_probe(struct snd_soc_dai *dai)
{
	struct cdns_jh8100_i2s_dev *i2s = snd_soc_dai_get_drvdata(dai);
	struct snd_dmaengine_dai_dma_data *tx = &i2s->tx_dma_data;
	struct snd_dmaengine_dai_dma_data *rx = &i2s->rx_dma_data;

	if (i2s->irq >= 0)
		return 0;

	/* Buswidth will be set by framework */
	tx->addr_width = DMA_SLAVE_BUSWIDTH_UNDEFINED;
	tx->addr = i2s->phybase + CDNS_JH8100_FIFO_MEM;
	tx->maxburst = 16;
	tx->fifo_size = 16;

	rx->addr_width = DMA_SLAVE_BUSWIDTH_UNDEFINED;
	rx->addr = i2s->phybase + CDNS_JH8100_FIFO_MEM;
	rx->maxburst = 16;
	rx->fifo_size = 16;

	snd_soc_dai_init_dma_data(dai, tx, rx);

	return 0;
}

static const struct snd_soc_component_driver cdns_jh8100_i2s_component = {
	.name = "cdns-jh8100-i2s",
};

static const struct snd_soc_dai_ops cdns_jh8100_i2s_dai_ops = {
	.probe		= cdns_jh8100_i2s_dai_probe,
	.startup	= cdns_jh8100_i2s_startup,
	.hw_params	= cdns_jh8100_i2s_hw_params,
	.trigger	= cdns_jh8100_i2s_trigger,
	.set_fmt	= cdns_jh8100_i2s_set_fmt,
};

static struct snd_soc_dai_driver cdns_jh8100_i2s_dai = {
	.name = "cdns-jh8100-i2s",
	.id = 0,
	.playback = {
		.stream_name = "Playback",
		.channels_min = 2,
		.channels_max = 8,
		.rates = SNDRV_PCM_RATE_8000_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE |
			   SNDRV_PCM_FMTBIT_S32_LE,
	},
	.capture = {
		.stream_name = "Capture",
		.channels_min = 2,
		.channels_max = 8,
		.rates = SNDRV_PCM_RATE_8000_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE |
			   SNDRV_PCM_FMTBIT_S32_LE,
	},
	.ops = &cdns_jh8100_i2s_dai_ops,
	.symmetric_rate = 1,
};

static int cdns_jh8100_i2s_runtime_suspend(struct device *dev)
{
	struct cdns_jh8100_i2s_dev *i2s = dev_get_drvdata(dev);

	clk_disable_unprepare(i2s->clks[1].clk); /* ICG clock */
	return 0;
}

static int cdns_jh8100_i2s_runtime_resume(struct device *dev)
{
	struct cdns_jh8100_i2s_dev *i2s = dev_get_drvdata(dev);

	return clk_prepare_enable(i2s->clks[1].clk); /* ICG clock */
}

static int cdns_jh8100_i2s_crg_init(struct cdns_jh8100_i2s_dev *i2s)
{
	struct reset_control *reset = devm_reset_control_get_exclusive(i2s->dev, NULL);
	int ret;

	if (IS_ERR(reset))
		return dev_err_probe(i2s->dev, PTR_ERR(reset), "failed to get i2s resets\n");

	i2s->clks[0].id = "bclk";
	i2s->clks[1].id = "icg";
	i2s->clks[2].id = "mclk_inner";

	ret = devm_clk_bulk_get(i2s->dev, ARRAY_SIZE(i2s->clks), i2s->clks);
	if (ret)
		return dev_err_probe(i2s->dev, ret, "failed to get i2s clocks\n");

	ret = clk_prepare_enable(i2s->clks[1].clk); /* ICG clock */
	if (ret)
		return dev_err_probe(i2s->dev, ret, "failed to enable icg clock\n");

	ret = reset_control_deassert(reset);
	if (ret)
		goto rst_err;

	return 0;

rst_err:
	clk_disable_unprepare(i2s->clks[1].clk);
	return ret;
}

static int cdns_jh8100_i2s_init(struct cdns_jh8100_i2s_dev *i2s)
{
	int ret	= cdns_jh8100_i2s_crg_init(i2s);
	unsigned int tmp;

	if (ret)
		return ret;

	/* Software reset i2s controller */
	ret = cdns_jh8100_i2s_reset_mask(i2s, CDNS_JH8100_I2S_CTRL_SFR_RST_MASK);
	if (ret) {
		dev_err(i2s->dev, "Failed to reset I2S.\n");
		return ret;
	}

	/* reset TX FIFO */
	ret = cdns_jh8100_i2s_reset_mask(i2s, CDNS_JH8100_I2S_CTRL_TFIFO_RST_MASK);
	if (ret) {
		dev_err(i2s->dev, "Failed to reset tx fifo.\n");
		return ret;
	}

	/* reset RX FIFO */
	ret = cdns_jh8100_i2s_reset_mask(i2s, CDNS_JH8100_I2S_CTRL_RFIFO_RST_MASK);
	if (ret) {
		dev_err(i2s->dev, "Failed to reset rx fifo.\n");
		return ret;
	}

	/* default master mode to init */
	i2s->tx_sync_ms_mode = CDNS_JH8100_I2S_MASTER_MODE;
	i2s->rx_sync_ms_mode = CDNS_JH8100_I2S_MASTER_MODE;
	cdns_jh8100_i2s_set_ms_mode(i2s);

	/* Should do it after setting Master/Slave mode */
	cdns_jh8100_i2s_reset_txrx_unit(i2s);
	cdns_jh8100_i2s_clear_int(i2s);

	cdns_jh8100_i2s_set_aempty_afull_th(i2s, (CDNS_JH8100_I2S_FIFO_DEPTH / 4),
					    (CDNS_JH8100_I2S_FIFO_DEPTH / 4 * 3));
	cdns_jh8100_i2s_set_fifo_mask(i2s, CDNS_JH8100_I2S_IT_TFIFO_AEMPTY |
				      CDNS_JH8100_I2S_IT_RFIFO_AFULL);

	i2s->rx_using_channels = 0;
	i2s->tx_using_channels = 0;

	/* cdns,i2s-max-channels is optional property and default 8 */
	ret = device_property_read_u32(i2s->dev, "cdns,i2s-max-channels", &tmp);
	if (ret) {
		i2s->max_channels = CDNS_JH8100_I2S_CHANNEL_MAX;
	} else {
		if (tmp > CDNS_JH8100_I2S_CHANNEL_MAX) {
			dev_err(i2s->dev,
				"The number %d of max channels from DTS is out of range!\n",
				tmp);
			return -EINVAL;
		}

		i2s->max_channels = tmp;
	}

	return 0;
}

static int cdns_jh8100_i2s_probe(struct platform_device *pdev)
{
	struct cdns_jh8100_i2s_dev *i2s;
	struct resource *res;
	int ret;

	i2s = devm_kzalloc(&pdev->dev, sizeof(*i2s), GFP_KERNEL);
	if (!i2s) {
		ret = -ENOMEM;
		goto err;
	}
	platform_set_drvdata(pdev, i2s);

	i2s->base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(i2s->base)) {
		ret = PTR_ERR(i2s->base);
		goto err;
	}

	i2s->dev = &pdev->dev;
	i2s->phybase = res->start;

	ret = cdns_jh8100_i2s_init(i2s);
	if (ret)
		goto err;

	i2s->irq = platform_get_irq(pdev, 0);
	if (i2s->irq >= 0) {
		ret = devm_request_irq(&pdev->dev, i2s->irq, cdns_jh8100_i2s_irq_handler,
				       0, pdev->name, i2s);
		if (ret < 0) {
			dev_err(&pdev->dev, "request_irq failed\n");
			goto err;
		}
	}

	ret = devm_snd_soc_register_component(&pdev->dev,
					      &cdns_jh8100_i2s_component,
					      &cdns_jh8100_i2s_dai, 1);
	if (ret < 0) {
		dev_err(&pdev->dev, "couldn't register component\n");
		goto err;
	}

	if (i2s->irq >= 0)
		ret = cdns_jh8100_i2s_pcm_register(pdev);
	else
		ret = devm_snd_dmaengine_pcm_register(&pdev->dev, NULL, 0);

	if (ret) {
		dev_err(&pdev->dev, "could not register pcm: %d\n", ret);
		goto err;
	}

	pm_runtime_enable(&pdev->dev);
	if (pm_runtime_enabled(&pdev->dev))
		cdns_jh8100_i2s_runtime_suspend(&pdev->dev);

	dev_info(&pdev->dev, "I2S supports %d stereo channels with %s.\n",
		 i2s->max_channels, ((i2s->irq < 0) ? "dma" : "interrupt"));

	return 0;

err:
	return ret;
}

static int cdns_jh8100_i2s_remove(struct platform_device *pdev)
{
	pm_runtime_disable(&pdev->dev);
	if (!pm_runtime_status_suspended(&pdev->dev))
		cdns_jh8100_i2s_runtime_suspend(&pdev->dev);

	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id cdns_jh8100_i2s_of_match[] = {
	{ .compatible = "starfive,jh8100-i2s", },
	{},
};
MODULE_DEVICE_TABLE(of, cdns_jh8100_i2s_of_match);
#endif

static const struct dev_pm_ops cdns_jh8100_i2s_pm_ops = {
	SET_RUNTIME_PM_OPS(cdns_jh8100_i2s_runtime_suspend,
			   cdns_jh8100_i2s_runtime_resume, NULL)
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				pm_runtime_force_resume)
};

static struct platform_driver cdns_jh8100_i2s_driver = {
	.probe   = cdns_jh8100_i2s_probe,
	.remove  = cdns_jh8100_i2s_remove,
	.driver  = {
		.name = "cdns-jh8100-i2s",
		.of_match_table = of_match_ptr(cdns_jh8100_i2s_of_match),
		.pm = &cdns_jh8100_i2s_pm_ops,
	},
};

module_platform_driver(cdns_jh8100_i2s_driver);

MODULE_AUTHOR("Xingyu Wu <xingyu.wu@starfivetech.com>");
MODULE_AUTHOR("Walker Chen <walker.chen@starfivetech.com>");
MODULE_DESCRIPTION("Cadence Multi-Channel I2S controller driver for StarFive JH8100 SoC");
MODULE_LICENSE("GPL");
