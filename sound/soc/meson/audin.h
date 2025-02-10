/* SPDX-License-Identifier: (GPL-2.0 OR MIT) */
/*
 * Copyright (c) 2025 BayLibre, SAS.
 * Author: Valerio Setti <vsetti@baylibre.com>
 */

#ifndef _MESON_AUDIN_H
#define _MESON_AUDIN_H

struct clk;
struct clk_bulk_data;
struct device;
struct of_phandle_args;
struct snd_soc_dai;
struct snd_soc_dai_ops;

enum audin_bulk_clks_id {
	PCLK = 0,
	AOCLK,
	MCLK,
	MIXER,
	INPUT,
};

struct audin {
	struct clk_bulk_data *bulk_clks;
	unsigned int bulk_clks_num;
	struct clk *aoclk_div_gate;
	struct clk *aoclk_basic_div;
	struct clk *aoclk_more_div;
	struct clk *lrclk_div;
};

/* I2SIN_CTRL register and bits */
#define AUDIN_I2SIN_CTRL	0x040	/* Reg index = 0x10 */
	#define AUDIN_I2SIN_CTRL_I2SIN_DIR		BIT(0)
	#define AUDIN_I2SIN_CTRL_I2SIN_CLK_SEL		BIT(1)
	#define AUDIN_I2SIN_CTRL_I2SIN_LRCLK_SEL	BIT(2)
	#define AUDIN_I2SIN_CTRL_I2SIN_POS_SYNC		BIT(3)
	#define AUDIN_I2SIN_CTRL_I2SIN_LRCLK_SKEW_MASK	GENMASK(6, 4)
	#define AUDIN_I2SIN_CTRL_I2SIN_LRCLK_INV	BIT(7)
	#define AUDIN_I2SIN_CTRL_I2SIN_SIZE_MASK	GENMASK(9, 8)
	#define AUDIN_I2SIN_CTRL_I2SIN_CHAN_EN_MASK	GENMASK(13, 10)
	#define AUDIN_I2SIN_CTRL_I2SIN_EN		BIT(15)

/* FIFO0 registers */
#define AUDIN_FIFO0_START	0x080	/* Reg index = 0x20 */
#define AUDIN_FIFO0_END		0x084	/* Reg index = 0x21 */
#define AUDIN_FIFO0_PTR		0x088	/* Reg index = 0x22 */
#define AUDIN_FIFO0_INTR	0x08C	/* Reg index = 0x23 */
#define AUDIN_FIFO0_RDPTR	0x090	/* Reg index = 0x24 */
#define AUDIN_FIFO0_WRAP	0x0C4	/* Reg index = 0x31 */

/* FIFO1 registers */
#define AUDIN_FIFO1_START	0x0CC	/* Reg index = 0x33 */
#define AUDIN_FIFO1_END		0x0D0	/* Reg index = 0x34 */
#define AUDIN_FIFO1_PTR		0x0D4	/* Reg index = 0x35 */
#define AUDIN_FIFO1_INTR	0x0D8	/* Reg index = 0x36 */
#define AUDIN_FIFO1_RDPTR	0x0DC	/* Reg index = 0x37 */
#define AUDIN_FIFO1_WRAP	0x110	/* Reg index = 0x44 */

/* FIFO2 registers */
#define AUDIN_FIFO2_START	0x114	/* Reg index = 0x45 */
#define AUDIN_FIFO2_END		0x118	/* Reg index = 0x46 */
#define AUDIN_FIFO2_PTR		0x11C	/* Reg index = 0x47 */
#define AUDIN_FIFO2_INTR	0x120	/* Reg index = 0x48 */
#define AUDIN_FIFO2_RDPTR	0x124	/* Reg index = 0x49 */
#define AUDIN_FIFO2_WRAP	0x140	/* Reg index = 0x50 */

/* FIFOx CTRL registers and bits */
#define AUDIN_FIFO0_CTRL	0x094	/* Reg index = 0x25 */
#define AUDIN_FIFO1_CTRL	0x0E0	/* Reg index = 0x38 */
#define AUDIN_FIFO2_CTRL	0x128	/* Reg index = 0x4a */
	#define AUDIN_FIFO_CTRL_EN		BIT(0)
	#define AUDIN_FIFO_CTRL_RST		BIT(1)
	#define AUDIN_FIFO_CTRL_LOAD		BIT(2)
	#define AUDIN_FIFO_CTRL_DIN_SEL_OFF	3
	#define AUDIN_FIFO_CTRL_DIN_SEL_MASK	GENMASK(5, 3)
	#define AUDIN_FIFO_CTRL_ENDIAN_MASK	GENMASK(10, 8)
	#define AUDIN_FIFO_CTRL_CHAN_MASK	GENMASK(14, 11)
	#define AUDIN_FIFO_CTRL_UG		BIT(15)

/* FIFOx_CTRL1 registers and bits */
#define AUDIN_FIFO0_CTRL1	0x098	/* Reg index = 0x26 */
#define AUDIN_FIFO1_CTRL1	0x0E4	/* Reg index = 0x39 */
#define AUDIN_FIFO2_CTRL1	0x12C	/* Reg index = 0x4b */
	#define AUDIN_FIFO_CTRL1_DIN_POS_2		BIT(7)
	#define AUDIN_FIFO_CTRL1_DIN_BYTE_NUM_MASK	GENMASK(3, 2)
	#define AUDIN_FIFO_CTRL1_DIN_POS_01_MASK	GENMASK(1, 0)

/* INT_CTRL register and bits */
#define AUDIN_INT_CTRL		0x144	/* Reg index = 0x51 */
	#define AUDIN_INT_CTRL_FIFO0_OVERFLOW		BIT(0)
	#define AUDIN_INT_CTRL_FIFO0_ADDR_TRIG		BIT(1)
	#define AUDIN_INT_CTRL_FIFO1_OVERFLOW		BIT(2)
	#define AUDIN_INT_CTRL_FIFO1_ADDR_TRIG		BIT(3)
	#define AUDIN_INT_CTRL_FIFO2_OVERFLOW		BIT(11)
	#define AUDIN_INT_CTRL_FIFO2_ADDR_TRIG		BIT(12)

/* FIFO_INT register and bits */
#define AUDIN_FIFO_INT		0x148	/* Reg index = 0x52 */
	#define AUDIN_FIFO_INT_FIFO0_OVERFLOW		BIT(0)
	#define AUDIN_FIFO_INT_FIFO0_ADDR_TRIG		BIT(1)
	#define AUDIN_FIFO_INT_FIFO1_OVERFLOW		BIT(2)
	#define AUDIN_FIFO_INT_FIFO1_ADDR_TRIG		BIT(3)
	#define AUDIN_FIFO_INT_FIFO2_OVERFLOW		BIT(11)
	#define AUDIN_FIFO_INT_FIFO2_ADDR_TRIG		BIT(12)

#define AUDIN_FORMATS (SNDRV_PCM_FMTBIT_S16_LE | \
		       SNDRV_PCM_FMTBIT_S24_LE)

extern const struct snd_soc_dai_ops audin_toddr_dai_ops;
extern const struct snd_soc_dai_ops audin_decoder_i2s_dai_ops;
extern struct device_attribute dev_attr_dump_regs;

snd_pcm_uframes_t audin_toddr_pointer(struct snd_soc_component *component,
				   struct snd_pcm_substream *substream);

#endif /* _MESON_AUDIN_H */
