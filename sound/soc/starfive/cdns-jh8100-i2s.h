/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Cadence Multi-Channel I2S Controller driver header file for StarFive JH8100 SoC
 *
 * Copyright (c) 2023 StarFive Technology Co., Ltd.
 * Author: Walker Chen <walker.chen@starfivetech.com>
 *         Xingyu Wu <xingyu.wu@starfivetech.com>
 */

#ifndef __CDNS_JH8100_I2S_MC_H
#define __CDNS_JH8100_I2S_MC_H

#include <linux/clk.h>
#include <sound/dmaengine_pcm.h>
#include <sound/pcm.h>

#define CDNS_JH8100_I2S_FIFO_DEPTH		128
#define CDNS_JH8100_FIFO_ACK_TIMEOUT_US		200
#define CDNS_JH8100_I2S_CHANNEL_MAX		8

/* I2S REGS */
#define CDNS_JH8100_I2S_CTRL		0x00
#define CDNS_JH8100_I2S_INTR_STAT	0x04
#define CDNS_JH8100_I2S_SRR		0x08
#define CDNS_JH8100_CID_CTRL		0x0c
#define CDNS_JH8100_TFIFO_CTRL		0x18
#define CDNS_JH8100_RFIFO_CTRL		0x1c
#define CDNS_JH8100_FIFO_MEM		0x3c

/*
 * I2S_CTRL: I2S transceiver control register
 */
#define CDNS_JH8100_I2S_CTRL_TR_CFG_0_SHIFT	8
#define CDNS_JH8100_I2S_CTRL_SFR_RST_MASK	BIT(20)
#define CDNS_JH8100_I2S_CTRL_T_MS_MASK		BIT(21)
#define CDNS_JH8100_I2S_CTRL_R_MS_MASK		BIT(22)
#define CDNS_JH8100_I2S_CTRL_TFIFO_RST_MASK	BIT(23)
#define CDNS_JH8100_I2S_CTRL_RFIFO_RST_MASK	BIT(24)
#define CDNS_JH8100_I2S_CTRL_TXRX_RST		GENMASK(26, 25)

/*
 * I2S_INTR_STAT: I2S Interrupt status register
 */
#define CDNS_JH8100_I2S_STAT_TX_UNDERRUN	BIT(0)
#define CDNS_JH8100_I2S_STAT_UNDERR_CODE	GENMASK(3, 1)
#define CDNS_JH8100_I2S_STAT_RX_OVERRUN		BIT(4)
#define CDNS_JH8100_I2S_STAT_OVERR_CODE		GENMASK(7, 5)
#define CDNS_JH8100_I2S_STAT_TFIFO_EMPTY	BIT(8)
#define CDNS_JH8100_I2S_STAT_TFIFO_AEMPTY	BIT(9)
#define CDNS_JH8100_I2S_STAT_RFIFO_AFULL	BIT(15)

/*
 * CID_CTRL: Clock strobes and interrupt masks control register
 */
#define CDNS_JH8100_CID_CTRL_STROBE_TX			BIT(8)
#define CDNS_JH8100_CID_CTRL_STROBE_RX			BIT(9)
#define CDNS_JH8100_CID_CTRL_INTREQ_MASK		BIT(15)
#define CDNS_JH8100_CID_CTRL_I2S_MASK_0_SHIFT		16

/*
 * I2S_SRR: Sample rate and resolution control register
 */
#define CDNS_JH8100_I2S_SRR_TRATE_MASK			GENMASK(9, 0)
#define CDNS_JH8100_I2S_SRR_RRATE_MASK			GENMASK(25, 16)
#define CDNS_JH8100_I2S_SRR_TRESOLUTION_MASK		GENMASK(15, 11)
#define CDNS_JH8100_I2S_SRR_RRESOLUTION_MASK		GENMASK(31, 27)

/*
 * TFIFO_CTRL & RFIFO_CTRL: The FIFO thresholds control register
 * AEMPTY: [15:0]
 * AFULL: [31:16]
 */
#define CDNS_TRFIFO_CTRL_AFULL_THRESHOLD_SHIFT		16

enum cdns_jh8100_i2s_channel_mask {
	CDNS_JH8100_I2S_CM_0   = BIT(0),
	CDNS_JH8100_I2S_CM_1   = BIT(1),
	CDNS_JH8100_I2S_CM_2   = BIT(2),
	CDNS_JH8100_I2S_CM_3   = BIT(3),
	CDNS_JH8100_I2S_CM_4   = BIT(4),
	CDNS_JH8100_I2S_CM_5   = BIT(5),
	CDNS_JH8100_I2S_CM_6   = BIT(6),
	CDNS_JH8100_I2S_CM_7   = BIT(7),
	CDNS_JH8100_I2S_CM_ALL = GENMASK(7, 0),
};

enum i2s_int_type {
	CDNS_JH8100_I2S_IT_TFIFO_EMPTY  = BIT(24),
	CDNS_JH8100_I2S_IT_TFIFO_AEMPTY = BIT(25),
	CDNS_JH8100_I2S_IT_TFIFO_FULL   = BIT(26),
	CDNS_JH8100_I2S_IT_TFIFO_AFULL  = BIT(27),
	CDNS_JH8100_I2S_IT_RFIFO_EMPTY  = BIT(28),
	CDNS_JH8100_I2S_IT_RFIFO_AEMPTY = BIT(29),
	CDNS_JH8100_I2S_IT_RFIFO_FULL   = BIT(30),
	CDNS_JH8100_I2S_IT_RFIFO_AFULL  = BIT(31),
	CDNS_JH8100_I2S_IT_ALL          = GENMASK(31, 24),
};

enum cdns_jh8100_i2s_master_slave_mode {
	CDNS_JH8100_I2S_SLAVE_MODE = 0,
	CDNS_JH8100_I2S_MASTER_MODE = 1,
};

enum cdns_jh8100_i2s_transmit_config {
	CDNS_JH8100_I2S_TC_RECEIVER = 0,
	CDNS_JH8100_I2S_TC_TRANSMITTER = 1,
};

struct cdns_jh8100_i2s_dev {
	struct device *dev;
	struct clk_bulk_data clks[3];
	void __iomem *base;
	resource_size_t	phybase; /* the physical memory */
	int irq;
	unsigned int sample_rate_param;
	unsigned char resolution;
	unsigned char max_channels /* up to CDNS_JH8100_I2S_CHANNEL_MAX */;
	unsigned char tx_using_channels;
	unsigned char rx_using_channels;

	/*
	 * Master (value '1') or slave (value '0') configuration bit
	 * for unit synchronizing all transmitters(receivers) with I2S bus
	 */
	bool tx_sync_ms_mode;
	bool rx_sync_ms_mode;

#if IS_ENABLED(CONFIG_SND_SOC_JH8100_CADENCE_I2S_PCM)
	/* current playback substream. NULL if not playing.
	 *
	 * Access to that field is synchronized between the interrupt handler
	 * and userspace through RCU.
	 *
	 * Interrupt handler (threaded part) does PIO on substream data in RCU
	 * read-side critical section. Trigger callback sets and clears the
	 * pointer when the playback is started and stopped with
	 * rcu_assign_pointer. When userspace is about to free the playback
	 * stream in the pcm_close callback it synchronizes with the interrupt
	 * handler by means of synchronize_rcu call.
	 */
	struct snd_pcm_substream __rcu *tx_substream;
	struct snd_pcm_substream __rcu *rx_substream;
	unsigned int (*tx_fn)(struct cdns_jh8100_i2s_dev *i2s,
			      struct snd_pcm_runtime *runtime, unsigned int tx_ptr,
			      bool *period_elapsed, snd_pcm_format_t format);
	unsigned int (*rx_fn)(struct cdns_jh8100_i2s_dev *dev,
			      struct snd_pcm_runtime *runtime, unsigned int rx_ptr,
			      bool *period_elapsed, snd_pcm_format_t format);
	snd_pcm_format_t format;
	unsigned int tx_ptr; /* next frame index in the sample buffer */
	unsigned int rx_ptr;
#endif

	struct snd_dmaengine_dai_dma_data tx_dma_data;
	struct snd_dmaengine_dai_dma_data rx_dma_data;
};

#if IS_ENABLED(CONFIG_SND_SOC_JH8100_CADENCE_I2S_PCM)
void cdns_jh8100_i2s_pcm_push_tx(struct cdns_jh8100_i2s_dev *dev);
void cdns_jh8100_i2s_pcm_pop_rx(struct cdns_jh8100_i2s_dev *dev);
int cdns_jh8100_i2s_pcm_register(struct platform_device *pdev);
#else
void cdns_jh8100_i2s_pcm_push_tx(struct cdns_jh8100_i2s_dev *dev) { }
void cdns_jh8100_i2s_pcm_pop_rx(struct cdns_jh8100_i2s_dev *dev) { }
int cdns_jh8100_i2s_pcm_register(struct platform_device *pdev)
{
	return -EINVAL;
}
#endif

#endif /* __CDNS_JH8100_I2S_MC_H */
