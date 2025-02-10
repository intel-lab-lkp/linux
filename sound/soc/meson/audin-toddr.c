// SPDX-License-Identifier: GPL-2.0
//
// Copyright (c) 2025 BayLibre, SAS.
// Author: Valerio Setti <vsetti@baylibre.com>

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <sound/pcm_params.h>
#include <linux/dma-mapping.h>
#include <linux/hrtimer.h>
#include <sound/soc.h>
#include <sound/soc-dai.h>
#include <dt-bindings/sound/meson-audin.h>

#include "audin.h"

struct fifo_regs {
	unsigned int start;
	unsigned int end;
	unsigned int ptr;
	unsigned int intr;
	unsigned int rdptr;
	unsigned int ctrl;
	unsigned int ctrl1;
	unsigned int wrap;
};

struct fifo_regs_bit_masks {
	unsigned int overflow_en;
	unsigned int addr_trigger_en;
	unsigned int overflow_set;
	unsigned int addr_trigger_set;
};

#define AUDIN_FIFO_COUNT	3

struct fifo_regs audin_fifo_regs[AUDIN_FIFO_COUNT] = {
	[0] = {
		.start	= AUDIN_FIFO0_START,
		.end	= AUDIN_FIFO0_END,
		.ptr	= AUDIN_FIFO0_PTR,
		.intr	= AUDIN_FIFO0_INTR,
		.rdptr	= AUDIN_FIFO0_RDPTR,
		.ctrl	= AUDIN_FIFO0_CTRL,
		.ctrl1	= AUDIN_FIFO0_CTRL1,
		.wrap	= AUDIN_FIFO0_WRAP,
	},
	[1] = {
		.start	= AUDIN_FIFO1_START,
		.end	= AUDIN_FIFO1_END,
		.ptr	= AUDIN_FIFO1_PTR,
		.intr	= AUDIN_FIFO1_INTR,
		.rdptr	= AUDIN_FIFO1_RDPTR,
		.ctrl	= AUDIN_FIFO1_CTRL,
		.ctrl1	= AUDIN_FIFO1_CTRL1,
		.wrap	= AUDIN_FIFO1_WRAP,
	},
	[2] = {
		.start	= AUDIN_FIFO2_START,
		.end	= AUDIN_FIFO2_END,
		.ptr	= AUDIN_FIFO2_PTR,
		.intr	= AUDIN_FIFO2_INTR,
		.rdptr	= AUDIN_FIFO2_RDPTR,
		.ctrl	= AUDIN_FIFO2_CTRL,
		.ctrl1	= AUDIN_FIFO2_CTRL1,
		.wrap	= AUDIN_FIFO2_WRAP,
	}
};

struct fifo_regs_bit_masks audin_fifo_regs_bit_masks[AUDIN_FIFO_COUNT] = {
	[0] = {
		.overflow_en = AUDIN_INT_CTRL_FIFO0_OVERFLOW,
		.addr_trigger_en = AUDIN_INT_CTRL_FIFO0_ADDR_TRIG,
		.overflow_set = AUDIN_FIFO_INT_FIFO0_OVERFLOW,
		.addr_trigger_set = AUDIN_FIFO_INT_FIFO0_ADDR_TRIG,
	},
	[1] = {
		.overflow_en = AUDIN_INT_CTRL_FIFO1_OVERFLOW,
		.addr_trigger_en = AUDIN_INT_CTRL_FIFO1_ADDR_TRIG,
		.overflow_set = AUDIN_FIFO_INT_FIFO1_OVERFLOW,
		.addr_trigger_set = AUDIN_FIFO_INT_FIFO1_ADDR_TRIG,
	},
	[2] = {
		.overflow_en = AUDIN_INT_CTRL_FIFO2_OVERFLOW,
		.addr_trigger_en = AUDIN_INT_CTRL_FIFO2_ADDR_TRIG,
		.overflow_set = AUDIN_FIFO_INT_FIFO2_OVERFLOW,
		.addr_trigger_set = AUDIN_FIFO_INT_FIFO2_ADDR_TRIG,
	},

};

/* This is the size of the FIFO (i.e. 64*64 bytes). */
#define AUDIN_FIFO_I2S_BLOCK		4096

static struct snd_pcm_hardware toddr_pcm_hw = {
	.info = (SNDRV_PCM_INFO_INTERLEAVED |
		 SNDRV_PCM_INFO_MMAP |
		 SNDRV_PCM_INFO_MMAP_VALID |
		 SNDRV_PCM_INFO_PAUSE),
	.formats = AUDIN_FORMATS,
	.rate_min = 5512,
	.rate_max = 192000,
	.channels_min = 2,
	.channels_max = 2,
	.period_bytes_min = 2*AUDIN_FIFO_I2S_BLOCK,
	.period_bytes_max = AUDIN_FIFO_I2S_BLOCK * USHRT_MAX,
	.periods_min = 2,
	.periods_max = UINT_MAX,

	/* No real justification for this */
	.buffer_bytes_max = 1 * 1024 * 1024,
};

struct audin_fifo {
	const struct fifo_regs *reg;
	const struct fifo_regs_bit_masks *reg_bit_masks;
	struct snd_pcm_hardware *pcm_hw;
	struct clk *pclk;

	/* The AUDIN peripheral has an IRQ to signal when data is received, but
	 * it cannot grant a periodic behavior. The reason is that the register
	 * which holds the address which triggers the IRQ must be updated
	 * continuously. Therefore we use a periodic timer.
	 */
	struct hrtimer polling_timer;
	int poll_time_ns;
	struct snd_pcm_substream *substream;
};

static int audin_toddr_trigger(struct snd_pcm_substream *substream, int cmd,
				struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct audin_fifo *fifo = snd_soc_dai_dma_data_get_capture(dai);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		snd_soc_component_update_bits(component, fifo->reg->ctrl,
					      AUDIN_FIFO_CTRL_EN,
					      AUDIN_FIFO_CTRL_EN);
		break;
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
	case SNDRV_PCM_TRIGGER_STOP:
		snd_soc_component_update_bits(component, fifo->reg->ctrl,
					      AUDIN_FIFO_CTRL_EN, 0);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int audin_toddr_prepare(struct snd_pcm_substream *substream,
			       struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct audin_fifo *fifo = snd_soc_dai_dma_data_get_capture(dai);
	struct snd_pcm_runtime *runtime = substream->runtime;
	dma_addr_t dma_end = runtime->dma_addr + runtime->dma_bytes - 8;
	unsigned int val;

	/* Setup memory boundaries */
	snd_soc_component_write(component, fifo->reg->start, runtime->dma_addr);
	snd_soc_component_write(component, fifo->reg->ptr, runtime->dma_addr);
	snd_soc_component_write(component, fifo->reg->end, dma_end);

	/* Load new addresses */
	val = AUDIN_FIFO_CTRL_LOAD | AUDIN_FIFO_CTRL_UG;
	snd_soc_component_update_bits(component, fifo->reg->ctrl, val, val);

	/* Reset */
	snd_soc_component_update_bits(dai->component, fifo->reg->ctrl,
				      AUDIN_FIFO_CTRL_RST,
				      AUDIN_FIFO_CTRL_RST);

	return 0;
}

static int audin_toddr_hw_params(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *params,
				 struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct audin_fifo *fifo = snd_soc_dai_dma_data_get_capture(dai);
	unsigned int val;

	switch (params_width(params)) {
	case 16:
		/* FIFO is filled line by line and each of them is 8 bytes. The
		 * problem is that each line is filled starting from the end,
		 * so we need to properly reorder them before moving to the
		 * RAM. This is the value required to properly re-order 16 bits
		 * samples.
		 */
		val = FIELD_PREP(AUDIN_FIFO_CTRL_ENDIAN_MASK, 6);
		snd_soc_component_update_bits(component, fifo->reg->ctrl,
					      AUDIN_FIFO_CTRL_ENDIAN_MASK, val);

		/* The I2S input decoder passed 24 bits of left-justified data
		 * but samples were 16 bits. Therefore we drop the LSB.
		 */
		val = FIELD_PREP(AUDIN_FIFO_CTRL1_DIN_POS_01_MASK, 1);
		snd_soc_component_update_bits(component, fifo->reg->ctrl1,
					      AUDIN_FIFO_CTRL1_DIN_POS_01_MASK,
					      val);

		/* Set sample size to 2 bytes (16 bit) */
		val = FIELD_PREP(AUDIN_FIFO_CTRL1_DIN_BYTE_NUM_MASK, 1);
		snd_soc_component_update_bits(component, fifo->reg->ctrl1,
					      AUDIN_FIFO_CTRL1_DIN_BYTE_NUM_MASK,
					      val);
		break;
	case 24:
		/* The same as above but in this case we need to reorder 32 bits
		 * samples (because 24 bits samples are stored as 32 bits).
		 */
		val = FIELD_PREP(AUDIN_FIFO_CTRL_ENDIAN_MASK, 4);
		snd_soc_component_update_bits(component, fifo->reg->ctrl,
					      AUDIN_FIFO_CTRL_ENDIAN_MASK,
					      val);

		val = FIELD_PREP(AUDIN_FIFO_CTRL1_DIN_POS_01_MASK, 0);
		snd_soc_component_update_bits(component, fifo->reg->ctrl1,
					      AUDIN_FIFO_CTRL1_DIN_POS_01_MASK,
					      val);

		/* Set sample size to 3 bytes (24 bit) */
		val = FIELD_PREP(AUDIN_FIFO_CTRL1_DIN_BYTE_NUM_MASK, 2);
		snd_soc_component_update_bits(component, fifo->reg->ctrl1,
					      AUDIN_FIFO_CTRL1_DIN_BYTE_NUM_MASK,
					      val);
		break;
	default:
		dev_err(dai->dev, "Unsupported physical width %u\n",
			params_physical_width(params));
		return -EINVAL;
	}

	/* This is a bit counterintuitive. Even though the platform has a single pin
	 * for I2S input which would mean that we can only support 2 channels,
	 * doing so would cause samples to be stored in a weird way into the FIFO:
	 * all the samples from the 1st channel on the 1st half of the FIFO, then
	 * samples from the 2nd channel in the other half. Of course extra work
	 * would be required to properly interleave them before returning to the
	 * userspace.
	 * Setting a single channel mode instead solves the problem: samples from
	 * 1st and 2nd channel are stored interleaved and sequentially in the FIFO.
	 */
	val = FIELD_PREP(AUDIN_FIFO_CTRL_CHAN_MASK, 1);
	snd_soc_component_update_bits(component, fifo->reg->ctrl,
				      AUDIN_FIFO_CTRL_CHAN_MASK, val);

	/* Setup the period for the polling timer. */
	fifo->poll_time_ns = 1000000000 / params_rate(params) *
			     params_period_size(params);

	return 0;
}

static enum hrtimer_restart timer_cb(struct hrtimer *timer)
{
	struct audin_fifo *fifo = container_of(timer, struct audin_fifo,
				  polling_timer);
	snd_pcm_period_elapsed(fifo->substream);
	hrtimer_forward_now(timer, fifo->poll_time_ns);
	return HRTIMER_RESTART;
}

static int audin_toddr_startup(struct snd_pcm_substream *substream,
				struct snd_soc_dai *dai)
{
	struct audin_fifo *fifo = snd_soc_dai_dma_data_get_capture(dai);
	int ret;

	snd_soc_set_runtime_hwparams(substream, fifo->pcm_hw);

	/* Check runtime parameters */
	ret = snd_pcm_hw_constraint_step(substream->runtime, 0,
					 SNDRV_PCM_HW_PARAM_BUFFER_BYTES,
					 AUDIN_FIFO_I2S_BLOCK);
	if (ret) {
		dev_err(dai->dev, "Failed to set runtime constraint %d\n", ret);
		return ret;
	}

	ret = snd_pcm_hw_constraint_step(substream->runtime, 0,
					 SNDRV_PCM_HW_PARAM_PERIOD_BYTES,
					 AUDIN_FIFO_I2S_BLOCK);
	if (ret) {
		dev_err(dai->dev, "Failed to set runtime constraint %d\n", ret);
		return ret;
	}

	ret = clk_prepare_enable(fifo->pclk);
	if (ret) {
		dev_err(dai->dev, "Failed to enable PCLK %d\n", ret);
		return ret;
	}

	/* Start the reporting timer */
	fifo->substream = substream;
	hrtimer_start(&fifo->polling_timer, fifo->poll_time_ns,
		      HRTIMER_MODE_REL);

	return ret;
}

static void audin_toddr_shutdown(struct snd_pcm_substream *substream,
				struct snd_soc_dai *dai)
{
	struct audin_fifo *fifo = snd_soc_dai_dma_data_get_capture(dai);

	hrtimer_cancel(&fifo->polling_timer);
	clk_disable_unprepare(fifo->pclk);
}

snd_pcm_uframes_t audin_toddr_pointer(struct snd_soc_component *component,
				      struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct audin_fifo *fifo = snd_soc_dai_dma_data_get_capture(dai);
	struct snd_pcm_runtime *runtime = substream->runtime;
	unsigned int start, ptr;

	start = snd_soc_component_read(component, fifo->reg->start);
	ptr = snd_soc_component_read(component, fifo->reg->ptr);

	return bytes_to_frames(runtime, ptr - start);
}

static int audin_toddr_dai_probe(struct snd_soc_dai *dai)
{
	struct audin *audin = snd_soc_component_get_drvdata(dai->component);
	struct audin_fifo *fifo;

	fifo = kzalloc(sizeof(*fifo), GFP_KERNEL);
	if (!fifo)
		return -ENOMEM;

	if (dai->id >= AUDIN_FIFO_COUNT) {
		dev_err(dai->dev, "Invalid DAI ID %d\n", dai->id);
		kfree(fifo);
		return -EINVAL;
	}

	fifo->reg = &audin_fifo_regs[dai->id];
	fifo->reg_bit_masks = &audin_fifo_regs_bit_masks[dai->id];
	fifo->pcm_hw = &toddr_pcm_hw;
	fifo->pclk = audin->bulk_clks[PCLK].clk;
	hrtimer_init(&fifo->polling_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	fifo->polling_timer.function = timer_cb;

	snd_soc_dai_dma_data_set_capture(dai, fifo);

	return 0;
}

static int audin_toddr_dai_remove(struct snd_soc_dai *dai)
{
	kfree(snd_soc_dai_dma_data_get_capture(dai));

	return 0;
}

static int audin_toddr_pcm_new(struct snd_soc_pcm_runtime *rtd,
				struct snd_soc_dai *dai)
{
	struct snd_card *card = rtd->card->snd_card;
	struct audin_fifo *fifo = snd_soc_dai_dma_data_get_capture(dai);
	size_t size = fifo->pcm_hw->buffer_bytes_max;
	int ret;

	ret = dma_coerce_mask_and_coherent(card->dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(dai->dev, "Failed to set DMA mask %d\n", ret);
		return ret;
	}

	ret = snd_pcm_set_managed_buffer_all(rtd->pcm, SNDRV_DMA_TYPE_DEV,
					     card->dev, size, size);
	if (ret) {
		dev_err(dai->dev, "Failed to set PCM managed buffer %d\n", ret);
		return ret;
	}

	return 0;
}

const struct snd_soc_dai_ops audin_toddr_dai_ops = {
	.trigger	= audin_toddr_trigger,
	.prepare	= audin_toddr_prepare,
	.hw_params	= audin_toddr_hw_params,
	.startup	= audin_toddr_startup,
	.shutdown	= audin_toddr_shutdown,
	.pcm_new	= audin_toddr_pcm_new,
	.probe		= audin_toddr_dai_probe,
	.remove		= audin_toddr_dai_remove,
};
