// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <asm/div64.h>
#include <linux/pm_runtime.h>
#include <linux/scatterlist.h>

#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-sg.h>

#include "qcom_jenc_dev.h"
#include "qcom_jenc_ops.h"
#include "qcom_jenc_defs.h"

#define JPEG_RESET_TIMEOUT_MS	300
#define JPEG_STOP_TIMEOUT_MS	200

#define JPEG_DQT_SHIFT		20
#define JPEG_Q5_21_SHIFT	21

#define JPEG_MCU_BLOCK_8	8
#define JPEG_MCU_BLOCK_16	16
#define JPEG_MCU_BLOCK_128	128
#define JPEG_MCU_BLOCK_256	256

#define JPEG_DEFAULT_SCALE_STEP	0x200000

#define JPEG_CLR_U32	(0U)
#define JPEG_SET_U32	(~0U)

/*
 *  JPEG | V4L2
 *  ---- | -------
 *  H1V1 | GREY
 *  H1V2 | YUV422M
 *  H2V1 | NV16M
 *  H2V2 | NV12M
 */
enum qcom_jpeg_encode_fmt {
	JPEG_ENCODE_H1V1 = 0,
	JPEG_ENCODE_H1V2,
	JPEG_ENCODE_H2V1,
	JPEG_ENCODE_H2V2,
	JPEG_ENCODE_MONO,
};

enum qcom_jpeg_memory_fmt {
	JPEG_MEM_FMT_PLANAR	 = 0x0,
	JPEG_MEM_FMT_PPLANAR	 = 0x1,
	JPEG_MEM_FMT_MONO	 = 0x2,
	JPEG_MEM_FMT_COEFFICIENT = 0x3
};

enum jpeg_mal_bounds {
	JPEG_CFG_MAL_BOUND_32_BYTES	= 0x0,
	JPEG_CFG_MAL_BOUND_64_BYTES	= 0x1,
	JPEG_CFG_MAL_BOUND_128_BYTES	= 0x2,
	JPEG_CFG_MAL_BOUND_256_BYTES	= 0x3,
	JPEG_CFG_MAL_BOUND_512_BYTES	= 0x4,
	JPEG_CFG_MAL_BOUND_1K_BYTES	= 0x5,
	JPEG_CFG_MAL_BOUND_2K_BYTES	= 0x6,
	JPEG_CFG_MAL_BOUND_4K_BYTES	= 0x7
};

struct qcom_jpeg_scale_blocks {
	u8 w_block[QCOM_JPEG_MAX_PLANES];
	u8 h_block[QCOM_JPEG_MAX_PLANES];
};

struct qcom_jpeg_mal_boundary {
	u32 bytes;
	int boundary;
};

struct qcom_jpeg_formats {
	u32 fourcc;
	enum qcom_jpeg_encode_fmt encode;
	enum qcom_jpeg_memory_fmt memory;
};

/*
 * Luminance quantization table defined by CCITT T.81.
 * See: https://www.w3.org/Graphics/JPEG/itu-t81.pdf
 */
static const u8 t81k1_dct_luma_table[V4L2_JPEG_PIXELS_IN_BLOCK] = {
	16,  11,  10,  16,  24,  40,  51,  61,
	12,  12,  14,  19,  26,  58,  60,  55,
	14,  13,  16,  24,  40,  57,  69,  56,
	14,  17,  22,  29,  51,  87,  80,  62,
	18,  22,  37,  56,  68, 109, 103,  77,
	24,  35,  55,  64,  81, 104, 113,  92,
	49,  64,  78,  87, 103, 121, 120, 101,
	72,  92,  95,  98, 112, 100, 103,  99
};

/*
 * Chrominance quantization table defined by CCITT T.81.
 * See: https://www.w3.org/Graphics/JPEG/itu-t81.pdf
 */
static const u8 t81k2_dct_chroma_table[V4L2_JPEG_PIXELS_IN_BLOCK] = {
	17,  18,  24,  47,  99,  99,  99,  99,
	18,  21,  26,  66,  99,  99,  99,  99,
	24,  26,  56,  99,  99,  99,  99,  99,
	47,  66,  99,  99,  99,  99,  99,  99,
	99,  99,  99,  99,  99,  99,  99,  99,
	99,  99,  99,  99,  99,  99,  99,  99,
	99,  99,  99,  99,  99,  99,  99,  99,
	99,  99,  99,  99,  99,  99,  99,  99
};

/*
 * Zig-zag scan order for quantized DCT coefficients
 * as defined by CCITT T.81.
 * See: https://www.w3.org/Graphics/JPEG/itu-t81.pdf
 */
static const u8 t81a6_dct_zig_zag_table[] = {
	 0,  1,  5,  6, 14, 15, 27, 28,
	 2,  4,  7, 13, 16, 26, 29, 42,
	 3,  8, 12, 17, 25, 30, 41, 43,
	 9, 11, 18, 24, 31, 40, 44, 53,
	10, 19, 23, 32, 39, 45, 52, 54,
	20, 22, 33, 38, 46, 51, 55, 60,
	21, 34, 37, 47, 50, 56, 59, 61,
	35, 36, 48, 49, 57, 58, 62, 63
};

static const u8 jpeg_mcu_per_ratio[] = {
	0, /* MCU = 1, Ratio < 2x	 */
	3, /* MCU = 0, 2x <= Ratio < 4x	 */
	2, /* MCU = 0, 4x <= Ratio < 8x	 */
	1, /* MCU = 0, 8x <= Ratio < 16x */
	0, /* MCU = 0, Ratio > 16x	 */
};

static const struct qcom_jpeg_formats jpeg_encode_fmt[] = {
	{
		.fourcc = V4L2_PIX_FMT_GREY,
		.encode = JPEG_ENCODE_MONO,
		.memory = JPEG_MEM_FMT_MONO
	},
	{
		.fourcc = V4L2_PIX_FMT_JPEG,
		.encode = JPEG_ENCODE_H1V1,
		.memory = JPEG_MEM_FMT_PPLANAR
	},
	{
		.fourcc = V4L2_PIX_FMT_YUV422M,
		.encode = JPEG_ENCODE_H1V2,
		.memory = JPEG_MEM_FMT_PLANAR
	},
	{
		.fourcc = V4L2_PIX_FMT_YVU422M,
		.encode = JPEG_ENCODE_H1V2,
		.memory = JPEG_MEM_FMT_PLANAR
	},
	{
		.fourcc = V4L2_PIX_FMT_NV16M,
		.encode = JPEG_ENCODE_H2V1,
		.memory = JPEG_MEM_FMT_PPLANAR
	},
	{
		.fourcc = V4L2_PIX_FMT_NV61M,
		.encode = JPEG_ENCODE_H2V1,
		.memory = JPEG_MEM_FMT_PPLANAR
	},
	{
		.fourcc = V4L2_PIX_FMT_NV12M,
		.encode = JPEG_ENCODE_H2V2,
		.memory = JPEG_MEM_FMT_PPLANAR
	},
	{
		.fourcc = V4L2_PIX_FMT_NV21M,
		.encode = JPEG_ENCODE_H2V2,
		.memory = JPEG_MEM_FMT_PPLANAR
	}
};

static const struct qcom_jpeg_mal_boundary jpeg_mal_bounds[] = {
	{ .bytes =   32, .boundary = JPEG_CFG_MAL_BOUND_32_BYTES  },
	{ .bytes =   64, .boundary = JPEG_CFG_MAL_BOUND_64_BYTES  },
	{ .bytes =  128, .boundary = JPEG_CFG_MAL_BOUND_128_BYTES },
	{ .bytes =  256, .boundary = JPEG_CFG_MAL_BOUND_256_BYTES },
	{ .bytes =  512, .boundary = JPEG_CFG_MAL_BOUND_512_BYTES },
	{ .bytes = 1024, .boundary = JPEG_CFG_MAL_BOUND_1K_BYTES  },
	{ .bytes = 4096, .boundary = JPEG_CFG_MAL_BOUND_4K_BYTES  }
};

static const struct qcom_jpeg_scale_blocks jpeg_mcu_blocks[] = {
	[JPEG_ENCODE_H1V1] = {
		.w_block = { JPEG_MCU_BLOCK_8, JPEG_MCU_BLOCK_8, JPEG_MCU_BLOCK_8 },
		.h_block = { JPEG_MCU_BLOCK_8, JPEG_MCU_BLOCK_8, JPEG_MCU_BLOCK_8 },
	},
	[JPEG_ENCODE_H1V2] = {
		.w_block = { JPEG_MCU_BLOCK_8, JPEG_MCU_BLOCK_8, JPEG_MCU_BLOCK_8  },
		.h_block = { JPEG_MCU_BLOCK_16, JPEG_MCU_BLOCK_8, JPEG_MCU_BLOCK_8 },
	},
	[JPEG_ENCODE_H2V1] = {
		.w_block = { JPEG_MCU_BLOCK_16, JPEG_MCU_BLOCK_8, JPEG_MCU_BLOCK_8 },
		.h_block = { JPEG_MCU_BLOCK_8, JPEG_MCU_BLOCK_8, JPEG_MCU_BLOCK_8  },
	},
	[JPEG_ENCODE_H2V2] = {
		.w_block = { JPEG_MCU_BLOCK_16, JPEG_MCU_BLOCK_8, JPEG_MCU_BLOCK_8 },
		.h_block = { JPEG_MCU_BLOCK_16, JPEG_MCU_BLOCK_8, JPEG_MCU_BLOCK_8 },
	},
	[JPEG_ENCODE_MONO] = {
		.w_block = { JPEG_MCU_BLOCK_8 },
		.h_block = { JPEG_MCU_BLOCK_8 }
	},
};

static inline int jpeg_get_memory_fmt(u32 fourcc)
{
	u32 fi;

	for (fi = 0; fi < ARRAY_SIZE(jpeg_encode_fmt); fi++) {
		if (jpeg_encode_fmt[fi].fourcc == fourcc)
			return jpeg_encode_fmt[fi].memory;
	}

	return -EINVAL;
}

static inline int jpeg_get_encode_fmt(u32 fourcc)
{
	u32 fi;

	for (fi = 0; fi < ARRAY_SIZE(jpeg_encode_fmt); fi++) {
		if (jpeg_encode_fmt[fi].fourcc == fourcc)
			return jpeg_encode_fmt[fi].encode;
	}

	return -EINVAL;
}

static inline int jpeg_get_mal_boundary(u32 width, const struct qcom_jpeg_mal_boundary *table,
					u32 count)
{
	u32 bi;

	if (WARN_ON_ONCE(!table || !count))
		return -EINVAL;

	for (bi = 0; bi < count; bi++) {
		if (table[bi].bytes > width)
			break;
	}

	if (!bi)
		return table[0].boundary;

	if (bi >= count)
		return table[count - 1].boundary;

	return table[bi - 1].boundary;
}

static inline u8 jpeg_get_mcu_per_block(u32 src_size, u32 dst_size)
{
	u8 h_rto;

	if (WARN_ON_ONCE(!src_size || !dst_size))
		return 0;

	/* Calculate scale factor */
	h_rto = max(src_size, dst_size) / min(src_size, dst_size);

	if (h_rto < 2)
		return jpeg_mcu_per_ratio[0];
	if (h_rto < 4)
		return jpeg_mcu_per_ratio[1];
	if (h_rto < 8)
		return jpeg_mcu_per_ratio[2];
	if (h_rto < 16)
		return jpeg_mcu_per_ratio[3];

	return jpeg_mcu_per_ratio[4];
}

static inline int jpeg_get_mcu_geometry(enum qcom_jpeg_encode_fmt fmt, u32 width, u32 height,
					u32 *blk_w, u32 *blk_h, u32 *mcu_cols, u32 *mcu_rows)
{
	const struct qcom_jpeg_scale_blocks *blks;
	u32 bw = 0, bh = 0;
	u8 pln;

	/*
	 * Dimensions are validated and normalized through V4L2 format negotiation;
	 * this is a defensive guard for unexpected internal callers.
	 */
	if (WARN_ON_ONCE(!width || !height))
		return -EINVAL;

	blks = &jpeg_mcu_blocks[fmt];

	for (pln = 0; pln < QCOM_JPEG_MAX_PLANES; pln++) {
		bw = max(bw, blks->w_block[pln]);
		bh = max(bh, blks->h_block[pln]);
	}

	if (!bw || !bh)
		return -EINVAL;

	if (blk_w)
		*blk_w = bw;
	if (blk_h)
		*blk_h = bh;

	if (mcu_cols)
		*mcu_cols = ALIGN(width, bw) / bw;

	if (mcu_rows)
		*mcu_rows = ALIGN(height, bh) / bh;

	return 0;
}

/* Integer part of scale */
static inline s32 jpeg_calc_scale_int(u32 in_width, u32 out_width)
{
	if (WARN_ON_ONCE(!out_width))
		return 0;

	return (s32)(in_width / out_width);
}

/* Fractional part of scale */
static inline u32 jpeg_calc_scale_frac(u32 in_width, u32 out_width)
{
	u32 remainder;

	if (WARN_ON_ONCE(!out_width))
		return 0;

	remainder = in_width % out_width;

	/* 64-bit to avoid overflow during shift */
	return (u32)(((u64)remainder << JPEG_Q5_21_SHIFT) / out_width);
}

static inline s32 jpeg_calc_q5_21(s32 int_part, u32 frac_part)
{
	return ((s32)((u32)int_part << JPEG_Q5_21_SHIFT)) | (frac_part & ((1u << 21) - 1));
}

static inline u32 jpeg_io_read(struct qcom_jenc_dev *jenc, u32 offset)
{
	return readl(jenc->jpeg_base + offset);
}

static inline void jpeg_io_write(struct qcom_jenc_dev *jenc, u32 offset, u32 value)
{
	writel(value, jenc->jpeg_base + offset);
}

/*
 * Runtime bitfield helpers (for non-constant masks).
 *
 * Requirements:
 *  - mask must be non-zero
 *  - mask must be contiguous (e.g. 0x7u << n)
 */

static inline u32 jpeg_bits_get(u32 mask, u32 val)
{
	/* __ffs(0) is undefined; fail-safe on invalid masks. */
	if (WARN_ON_ONCE(!mask))
		return 0;

	return (val & mask) >> __ffs(mask);
}

static inline u32 jpeg_bits_set(u32 mask, u32 val)
{
	/* __ffs(0) is undefined; fail-safe on invalid masks. */
	if (WARN_ON_ONCE(!mask))
		return 0;

	return (val << __ffs(mask)) & mask;
}

static inline u32 jpeg_rd_bits(struct qcom_jenc_dev *jenc, u32 offs, enum qcom_jpeg_mask_id mid)
{
	u32 reg  = jpeg_io_read(jenc, offs);
	u32 mask = jenc->res->hw_mask[mid];

	return jpeg_bits_get(mask, reg);
}

/*
 * Read-modify-write (for R/W registers)
 */
static inline void jpeg_rw_bits(struct qcom_jenc_dev *jenc, u32 offs, enum qcom_jpeg_mask_id mid,
				u32 val)
{
	u32 reg  = jpeg_io_read(jenc, offs);
	u32 mask = jenc->res->hw_mask[mid];

	reg &= ~mask;
	reg |= jpeg_bits_set(mask, val);

	jpeg_io_write(jenc, offs, reg);
}

/*
 * Write-only variant (for write only registers)
 */
static inline void jpeg_wo_bits(struct qcom_jenc_dev *jenc, u32 offs, enum qcom_jpeg_mask_id mid,
				u32 val)
{
	u32 mask = jenc->res->hw_mask[mid];

	jpeg_io_write(jenc, offs, jpeg_bits_set(mask, val));
}

static u8 jpeg_calculate_dqt(struct jenc_context *ectx, u8 dqt_value)
{
	u64 ratio;
	u8 calc_val;

	ratio = (QCOM_JPEG_QUALITY_MAX - ectx->quality_requested) << JPEG_DQT_SHIFT;
	ratio = max_t(u64, 1, ratio);
	do_div(ratio, QCOM_JPEG_QUALITY_MID);

	calc_val = DIV64_U64_ROUND_CLOSEST(ratio * dqt_value, 1LU << JPEG_DQT_SHIFT);

	return max_t(u8, 1, calc_val);
}

/*
 * jpeg_update_dqt_cache - compute scaled DQT coefficients and store them in
 * the software JPEG header cache (hdr_cache).  Safe to call from buf_prepare
 * before the hardware is powered on; no MMIO access is performed here.
 */
static void jpeg_update_dqt_cache(struct jenc_context *ectx)
{
	u8 *base;
	u8 dqt_val, idx;
	int i;

	/* Luma DQT cache update */
	if (ectx->hdr_cache.dqt_one_offs) {
		base = &ectx->hdr_cache.data[ectx->hdr_cache.dqt_one_offs + 1];
		for (i = 0; i < ARRAY_SIZE(t81k1_dct_luma_table); i++) {
			dqt_val = jpeg_calculate_dqt(ectx, t81k1_dct_luma_table[i]);
			idx = t81a6_dct_zig_zag_table[i];
			base[idx] = dqt_val;
		}
	}

	/* Chroma DQT cache update */
	if (ectx->hdr_cache.dqt_two_offs) {
		base = &ectx->hdr_cache.data[ectx->hdr_cache.dqt_two_offs + 1];
		for (i = 0; i < ARRAY_SIZE(t81k2_dct_chroma_table); i++) {
			dqt_val = jpeg_calculate_dqt(ectx, t81k2_dct_chroma_table[i]);
			idx = t81a6_dct_zig_zag_table[i];
			base[idx] = dqt_val;
		}
	}
}

/*
 * jpeg_upload_dmi_table - write the scaled DQT coefficients to the hardware
 * DMI registers.  Must only be called from the job execution path where
 * runtime PM has already been acquired (pm_runtime_resume_and_get).
 *
 * Reads precomputed values from hdr_cache (populated by jpeg_update_dqt_cache)
 * to avoid redundant per-coefficient recalculation on the hot encode path.
 */
static void jpeg_upload_dmi_table(struct jenc_context *ectx)
{
	const struct qcom_jpeg_reg_offs *offs = ectx->jenc->res->hw_offs;
	const u8 *luma_qt = &ectx->hdr_cache.data[ectx->hdr_cache.dqt_one_offs + 1];
	u32 pcfg = { 0x00000011 };
	u32 addr = { 0x00000000 };
	u32 reg_val;
	int i;

	/* DMI upload start sequence */
	jpeg_io_write(ectx->jenc, offs->dmi_addr, addr);
	jpeg_io_write(ectx->jenc, offs->dmi_cfg, pcfg);

	/* DMI Luma upload — values are stored in zigzag order in hdr_cache */
	for (i = 0; i < ARRAY_SIZE(t81k1_dct_luma_table); i++) {
		reg_val = div_u64(U16_MAX + 1U, luma_qt[i]);
		reg_val = clamp_t(u32, reg_val, 0, U16_MAX);
		jpeg_io_write(ectx->jenc, offs->dmi_data, reg_val);
	}

	/* DMI Chroma upload — only present for color formats */
	if (ectx->hdr_cache.dqt_two_offs) {
		const u8 *chroma_qt = &ectx->hdr_cache.data[ectx->hdr_cache.dqt_two_offs + 1];

		for (i = 0; i < ARRAY_SIZE(t81k2_dct_chroma_table); i++) {
			reg_val = div_u64(U16_MAX + 1U, chroma_qt[i]);
			reg_val = clamp_t(u32, reg_val, 0, U16_MAX);
			jpeg_io_write(ectx->jenc, offs->dmi_data, reg_val);
		}
	}

	/* DMI upload end sequence */
	jpeg_io_write(ectx->jenc, offs->dmi_cfg, addr);

	ectx->quality_programmed = ectx->quality_requested;

	dev_dbg(ectx->dev, "%s: ctx=%p quality_programmed=%d\n", __func__, ectx,
		ectx->quality_programmed);
}

static void jpeg_sync_sg(struct device *dev,
			 struct qcom_jpeg_buff *frame,
			 enum dma_data_direction direction, bool for_device)
{
	u8 pln;

	for (pln = 0; pln < QCOM_JPEG_MAX_PLANES; pln++) {
		struct sg_table *sgt = frame->plns[pln].sgt;

		if (!frame->plns[pln].dma || !sgt)
			break;

		if (for_device)
			dma_sync_sgtable_for_device(dev, sgt, direction);
		else
			dma_sync_sgtable_for_cpu(dev, sgt, direction);
	}
}

static int jpeg_init(struct qcom_jenc_dev *jenc)
{
	const struct qcom_jpeg_reg_offs *offs;
	unsigned long rtime;
	u32 hw_ver;

	if (!jenc || !jenc->dev || !jenc->jpeg_base || !jenc->res->hw_offs) {
		pr_err("encoder HW init failed\n");
		return -EINVAL;
	}

	offs	 = jenc->res->hw_offs;

	jpeg_wo_bits(jenc, offs->int_clr, JMSK_IRQ_STATUS_ALL_BITS, JPEG_SET_U32);
	jpeg_rw_bits(jenc, offs->int_mask, JMSK_IRQ_STATUS_RESET_ACK, JPEG_SET_U32);

	reinit_completion(&jenc->reset_complete);

	jpeg_wo_bits(jenc, offs->reset_cmd, JMSK_RST_CMD_COMMON, JPEG_SET_U32);

	rtime = wait_for_completion_timeout(&jenc->reset_complete,
					    msecs_to_jiffies(JPEG_RESET_TIMEOUT_MS));
	if (!rtime) {
		dev_err(jenc->dev, "encoder HW reset timeout\n");
		disable_irq(jenc->irq);
		return -ETIME;
	}

	hw_ver = jpeg_io_read(jenc, offs->hw_version);
	dev_dbg(jenc->dev, "JPEG HW encoder version %d.%d.%d\n",
		jpeg_bits_get(jenc->res->hw_mask[JMSK_HW_VER_MAJOR], hw_ver),
		jpeg_bits_get(jenc->res->hw_mask[JMSK_HW_VER_MINOR], hw_ver),
		jpeg_bits_get(jenc->res->hw_mask[JMSK_HW_VER_STEP], hw_ver));

	jpeg_wo_bits(jenc, offs->hw_cmd, JMSK_CMD_CLR_RD_PLNS_QUEUE, JPEG_SET_U32);
	jpeg_wo_bits(jenc, offs->hw_cmd, JMSK_CMD_CLR_RD_PLNS_QUEUE, JPEG_CLR_U32);

	jpeg_wo_bits(jenc, offs->hw_cmd, JMSK_CMD_CLR_WR_PLNS_QUEUE, JPEG_SET_U32);
	jpeg_wo_bits(jenc, offs->hw_cmd, JMSK_CMD_CLR_WR_PLNS_QUEUE, JPEG_CLR_U32);

	jpeg_wo_bits(jenc, offs->int_clr, JMSK_IRQ_STATUS_ALL_BITS, JPEG_SET_U32);
	jpeg_rw_bits(jenc, offs->int_mask, JMSK_IRQ_STATUS_ALL_BITS, JPEG_SET_U32);

	return 0;
}

static int jpeg_exec(struct qcom_jenc_dev *jenc)
{
	const struct qcom_jpeg_reg_offs *offs = jenc->res->hw_offs;

	jpeg_wo_bits(jenc, offs->hw_cmd, JMSK_CMD_HW_START, 1);

	return 0;
}

static void jpeg_stop(struct qcom_jenc_dev *jenc)
{
	const struct qcom_jpeg_reg_offs *offs = jenc->res->hw_offs;

	jpeg_wo_bits(jenc, offs->hw_cmd, JMSK_CMD_HW_START, 0);

	jpeg_wo_bits(jenc, offs->hw_cmd, JMSK_CMD_CLR_RD_PLNS_QUEUE, JPEG_SET_U32);
	jpeg_wo_bits(jenc, offs->hw_cmd, JMSK_CMD_CLR_RD_PLNS_QUEUE, JPEG_CLR_U32);

	jpeg_wo_bits(jenc, offs->hw_cmd, JMSK_CMD_CLR_WR_PLNS_QUEUE, JPEG_SET_U32);
	jpeg_wo_bits(jenc, offs->hw_cmd, JMSK_CMD_CLR_WR_PLNS_QUEUE, JPEG_CLR_U32);

	jpeg_wo_bits(jenc, offs->int_clr, JMSK_IRQ_STATUS_ALL_BITS, JPEG_SET_U32);
	jpeg_rw_bits(jenc, offs->int_mask, JMSK_IRQ_STATUS_ALL_BITS, JPEG_SET_U32);
}

static int jpeg_deinit(struct qcom_jenc_dev *jenc)
{
	const struct qcom_jpeg_reg_offs *offs = jenc->res->hw_offs;
	unsigned long rtime;

	jpeg_wo_bits(jenc, offs->int_clr, JMSK_IRQ_STATUS_ALL_BITS, JPEG_SET_U32);
	jpeg_rw_bits(jenc, offs->int_mask, JMSK_IRQ_STATUS_STOP_ACK, JPEG_SET_U32);

	jpeg_wo_bits(jenc, offs->hw_cmd, JMSK_CMD_HW_STOP, 1);

	reinit_completion(&jenc->stop_complete);
	rtime = wait_for_completion_timeout(&jenc->stop_complete,
					    msecs_to_jiffies(JPEG_STOP_TIMEOUT_MS));
	if (!rtime) {
		dev_err(jenc->dev, "encoder HW stop timeout\n");
		return -ETIME;
	}

	jpeg_rw_bits(jenc, offs->int_mask, JMSK_IRQ_STATUS_ALL_BITS, JPEG_CLR_U32);
	jpeg_wo_bits(jenc, offs->int_clr, JMSK_IRQ_STATUS_ALL_BITS, JPEG_SET_U32);

	return 0;
}

static int jpeg_apply_fe_addr(struct jenc_context *ectx, struct qcom_jenc_queue *q,
			      struct vb2_buffer *vb)
{
	struct qcom_jenc_dev *jenc = ectx->jenc;
	const struct qcom_jpeg_reg_offs *offs = jenc->res->hw_offs;
	struct qcom_jpeg_buff *frame = &q->buff[vb->index];
	struct v4l2_pix_format_mplane *fmt = &q->vf;
	unsigned long flags;
	u8 pln = 0;

	if (WARN_ON_ONCE(!frame->plns[pln].dma))
		return -EPERM;

	jpeg_sync_sg(jenc->dev, frame, DMA_TO_DEVICE, true);

	for (pln = 0; pln < fmt->num_planes; pln++) {
		if (!frame->plns[pln].sgt || !frame->plns[pln].sgt->sgl)
			break;

		jpeg_io_write(jenc, offs->fe.pntr[pln], frame->plns[pln].dma);
		jpeg_io_write(jenc, offs->fe.offs[pln], 0);

		dev_dbg(jenc->dev, "%s: pln=%d addr=0x%llx idx:%d\n", __func__,
			pln, frame->plns[pln].dma, vb->index);
	}

	spin_lock_irqsave(&jenc->hw_lock, flags);
	q->buff_id = vb->index;
	spin_unlock_irqrestore(&jenc->hw_lock, flags);

	return 0;
}

static int jpeg_store_fe_next(struct jenc_context *ectx, struct vb2_buffer *vb2)
{
	struct qcom_jenc_queue *q = &ectx->bufq[TYPE2QID(vb2->type)];
	struct qcom_jpeg_buff *buff = &q->buff[vb2->index];
	u8 pln = 0;

	buff->plns[pln].sgt = vb2_dma_sg_plane_desc(vb2, pln);
	if (!buff->plns[pln].sgt)
		return -EINVAL;

	if (!buff->plns[pln].sgt->sgl)
		return -EINVAL;

	buff->plns[pln].dma = sg_dma_address(buff->plns[pln].sgt->sgl);
	if (!buff->plns[pln].dma)
		return -EINVAL;

	buff->plns[pln].size = vb2_plane_size(vb2, pln);
	if (!buff->plns[pln].size)
		return -EINVAL;

	for (pln = 1; pln < q->vf.num_planes; pln++) {
		buff->plns[pln].sgt = vb2_dma_sg_plane_desc(vb2, pln);
		if (!buff->plns[pln].sgt || !buff->plns[pln].sgt->sgl)
			return -EINVAL;

		buff->plns[pln].dma = sg_dma_address(buff->plns[pln].sgt->sgl);
		if (!buff->plns[pln].dma)
			return -EINVAL;

		buff->plns[pln].size = vb2_plane_size(vb2, pln);
		if (!buff->plns[pln].size)
			return -EINVAL;
	}

	return 0;
}

static int jpeg_setup_fe_size(struct jenc_context *ectx, struct qcom_jenc_queue *q)
{
	struct qcom_jenc_dev *jenc = ectx->jenc;
	struct v4l2_pix_format_mplane *sfmt = &q->vf;
	const struct qcom_jpeg_reg_offs *offs = jenc->res->hw_offs;
	u8 pln;

	for (pln = 0; pln < QCOM_JPEG_MAX_PLANES; pln++) {
		jpeg_rw_bits(jenc, offs->fe.bsize[pln], JMSK_PLNS_RD_BUF_SIZE_WIDTH, 0);
		jpeg_rw_bits(jenc, offs->fe.bsize[pln], JMSK_PLNS_RD_BUF_SIZE_HEIGHT, 0);
		jpeg_rw_bits(jenc, offs->fe.bsize[pln], JMSK_PLNS_RD_STRIDE, 0);
	}

	for (pln = 0; pln < sfmt->num_planes; pln++) {
		jpeg_rw_bits(jenc, offs->fe.bsize[pln], JMSK_PLNS_RD_BUF_SIZE_WIDTH,
			     sfmt->width  - 1);
		jpeg_rw_bits(jenc, offs->fe.bsize[pln], JMSK_PLNS_RD_BUF_SIZE_HEIGHT,
			     sfmt->height  - 1);
		jpeg_rw_bits(jenc, offs->fe.stride[pln], JMSK_PLNS_RD_STRIDE,
			     sfmt->plane_fmt[pln].bytesperline);

		dev_dbg(ectx->dev, "%s: ctx=%p pln=%d width=%d height=%d stride=%d\n",
			__func__, ectx, pln,
			jpeg_rd_bits(jenc, offs->fe.bsize[pln], JMSK_PLNS_RD_BUF_SIZE_WIDTH),
			jpeg_rd_bits(jenc, offs->fe.bsize[pln], JMSK_PLNS_RD_BUF_SIZE_HEIGHT),
			jpeg_rd_bits(jenc, offs->fe.stride[pln], JMSK_PLNS_RD_STRIDE));
	}

	return 0;
}

static int jpeg_setup_fe_hinit(struct jenc_context *ectx, struct qcom_jenc_queue *q)
{
	struct qcom_jenc_dev *jenc = ectx->jenc;
	const struct qcom_jpeg_reg_offs *offs = jenc->res->hw_offs;
	u8 pln;

	for (pln = 0; pln < QCOM_JPEG_MAX_PLANES; pln++)
		jpeg_io_write(jenc, offs->fe.hinit[pln], 0);

	return 0;
}

static int jpeg_setup_fe_vinit(struct jenc_context *ectx, struct qcom_jenc_queue *q)
{
	struct qcom_jenc_dev *jenc = ectx->jenc;
	const struct qcom_jpeg_reg_offs *offs = jenc->res->hw_offs;
	u8 pln;

	for (pln = 0; pln < QCOM_JPEG_MAX_PLANES; pln++)
		jpeg_io_write(jenc, offs->fe.vinit[pln], 0);

	return 0;
}

static int jpeg_setup_fe_params(struct jenc_context *ectx, struct qcom_jenc_queue *q)
{
	struct qcom_jenc_dev *jenc = ectx->jenc;
	struct v4l2_pix_format_mplane *sfmt = &q->vf;
	struct v4l2_pix_format_mplane *dfmt = &ectx->bufq[JENC_DST_QUEUE].vf;
	const struct qcom_jpeg_reg_offs *offs = jenc->res->hw_offs;
	u8 expected_planes, pln;
	int rval;

	jpeg_rw_bits(jenc, offs->fe_cfg, JMSK_FE_CFG_MAL_EN, 1);
	jpeg_rw_bits(jenc, offs->fe_cfg, JMSK_FE_CFG_BOTTOM_VPAD_EN, 1);

	rval = jpeg_get_memory_fmt(sfmt->pixelformat);
	if (rval < 0) {
		dev_err(ectx->dev, "%s: invalid memory format for v4l2 format:0x%x\n",
			__func__, sfmt->pixelformat);
		return -EINVAL;
	}

	switch (rval) {
	case JPEG_MEM_FMT_MONO:
		expected_planes = 1;
		break;
	case JPEG_MEM_FMT_PPLANAR:
		expected_planes = 2;
		break;
	case JPEG_MEM_FMT_PLANAR:
		expected_planes = 3;
		break;
	default:
		return -EINVAL;
	}

	if (sfmt->num_planes != expected_planes) {
		dev_err(ectx->dev, "plane mismatch fmt=%u expected=%u got=%u\n",
			rval, expected_planes, sfmt->num_planes);
		return -EINVAL;
	}

	jpeg_rw_bits(jenc, offs->fe_cfg, JMSK_FE_CFG_MEMORY_FORMAT, rval);

	jpeg_rw_bits(jenc, offs->fe_cfg, JMSK_FE_CFG_PLN0_EN, 0);
	jpeg_rw_bits(jenc, offs->fe_cfg, JMSK_FE_CFG_PLN1_EN, 0);
	jpeg_rw_bits(jenc, offs->fe_cfg, JMSK_FE_CFG_PLN2_EN, 0);

	if (sfmt->width == dfmt->width && sfmt->height == dfmt->height) {
		/* No scaling */
		jpeg_rw_bits(jenc, offs->fe_cfg, JMSK_FE_CFG_SIXTEEN_MCU_EN, 1);
		jpeg_rw_bits(jenc, offs->fe_cfg, JMSK_FE_CFG_MCUS_PER_BLOCK, 0);
	} else {
		u8 mcu_per_blks;

		/* Scaling */
		jpeg_rw_bits(jenc, offs->fe_cfg, JMSK_FE_CFG_SIXTEEN_MCU_EN, 0);
		/* get value according to image width */
		mcu_per_blks = jpeg_get_mcu_per_block(sfmt->width, dfmt->width);
		/* get value according to image height assign the bigger */
		mcu_per_blks = max_t(u8, mcu_per_blks,
				     jpeg_get_mcu_per_block(sfmt->height, dfmt->height));

		jpeg_rw_bits(jenc, offs->fe_cfg, JMSK_FE_CFG_MCUS_PER_BLOCK, mcu_per_blks);
	}

	dev_dbg(ectx->dev, "%s: sixteen MCU enabled=%d, %d MCU per blocks\n", __func__,
		jpeg_rd_bits(jenc, offs->fe_cfg, JMSK_FE_CFG_SIXTEEN_MCU_EN),
		jpeg_rd_bits(jenc, offs->fe_cfg, JMSK_FE_CFG_MCUS_PER_BLOCK));

	rval = jpeg_get_mal_boundary(sfmt->width, jpeg_mal_bounds, ARRAY_SIZE(jpeg_mal_bounds));
	if (rval < 0) {
		dev_err(ectx->dev, "%s: failed to get FE mal boundary width=%u\n", __func__,
			sfmt->width);
		return -EINVAL;
	}
	jpeg_rw_bits(jenc, offs->fe_cfg, JMSK_FE_CFG_MAL_BOUNDARY, rval);

	dev_dbg(ectx->dev, "%s: optimal FE mal boundary=%d\n", __func__,
		jpeg_rd_bits(jenc, offs->fe_cfg, JMSK_FE_CFG_MAL_BOUNDARY));

	rval = jpeg_get_encode_fmt(sfmt->pixelformat);
	if (rval < 0) {
		dev_err(ectx->dev, "%s: unsupported encode format fourcc=0x%x\n",
			__func__, sfmt->pixelformat);
		return -EINVAL;
	}

	switch (rval) {
	case JPEG_ENCODE_MONO:
	case JPEG_ENCODE_H1V1:
	case JPEG_ENCODE_H2V1:
		jpeg_rw_bits(jenc, offs->fe.vbpad_cfg, JMSK_FE_VBPAD_CFG_BLOCK_ROW,
			     DIV_ROUND_UP(sfmt->height, JPEG_MCU_BLOCK_8));
		break;
	case JPEG_ENCODE_H1V2:
	case JPEG_ENCODE_H2V2:
		jpeg_rw_bits(jenc, offs->fe.vbpad_cfg, JMSK_FE_VBPAD_CFG_BLOCK_ROW,
			     DIV_ROUND_UP(sfmt->height, JPEG_MCU_BLOCK_16));
		break;
	default:
		dev_err(ectx->dev, "%s: unsupported encode format fourcc=0x%x\n", __func__, rval);
		return -EINVAL;
	}

	dev_dbg(ectx->dev, "%s: FE vpad config=%d\n", __func__,
		jpeg_rd_bits(jenc, offs->fe.vbpad_cfg, JMSK_FE_VBPAD_CFG_BLOCK_ROW));

	if (sfmt->pixelformat == V4L2_PIX_FMT_NV21M || sfmt->pixelformat == V4L2_PIX_FMT_NV61M)
		jpeg_rw_bits(jenc, offs->fe_cfg, JMSK_FE_CFG_CBCR_ORDER, 1);
	else
		jpeg_rw_bits(jenc, offs->fe_cfg, JMSK_FE_CFG_CBCR_ORDER, 0);

	for (pln = 0; pln < sfmt->num_planes; pln++) {
		if (sfmt->width && sfmt->height) {
			switch (pln) {
			case 0:
				jpeg_rw_bits(jenc, offs->fe_cfg, JMSK_FE_CFG_PLN0_EN, 1);
				break;
			case 1:
				jpeg_rw_bits(jenc, offs->fe_cfg, JMSK_FE_CFG_PLN1_EN, 1);
				break;
			case 2:
				jpeg_rw_bits(jenc, offs->fe_cfg, JMSK_FE_CFG_PLN2_EN, 1);
				break;
			}
		}
	}

	jpeg_rw_bits(jenc, offs->core_cfg, JMSK_CORE_CFG_FE_ENABLE, 1);

	return 0;
}

static int jpeg_setup_fe(struct jenc_context *ectx, struct qcom_jenc_queue *q)
{
	int rc;

	rc = jpeg_setup_fe_size(ectx, q);
	if (rc)
		return rc;

	rc = jpeg_setup_fe_hinit(ectx, q);
	if (rc)
		return rc;

	rc = jpeg_setup_fe_vinit(ectx, q);
	if (rc)
		return rc;

	rc = jpeg_setup_fe_params(ectx, q);
	if (rc)
		return rc;

	return 0;
}

static int jpeg_ensure_header_cache(struct jenc_context *ectx)
{
	struct qcom_jenc_queue *sq = &ectx->bufq[JENC_SRC_QUEUE];
	int rc;

	if (ectx->hdr_cache.size)
		return 0;

	rc = qcom_jenc_header_init(&ectx->hdr_cache, sq->vf.pixelformat);
	if (rc) {
		dev_err(ectx->dev, "JFIF header lazy init failed\n");
		return rc;
	}

	return 0;
}

static int jpeg_apply_we_addr(struct jenc_context *ectx, struct qcom_jenc_queue *q,
			      struct vb2_buffer *vb)
{
	struct qcom_jenc_dev *jenc = ectx->jenc;
	const struct qcom_jpeg_reg_offs *offs = jenc->res->hw_offs;
	struct qcom_jpeg_buff *frame = &q->buff[vb->index];
	void *mptr = vb2_plane_vaddr(vb, 0);
	dma_addr_t dma = frame->plns[0].dma;
	unsigned long flags;
	int rc;
	u8 pln = 0;

	if (WARN_ON_ONCE(!dma))
		return -EPERM;

	if (WARN_ON_ONCE(!mptr))
		return -EPERM;

	rc = jpeg_ensure_header_cache(ectx);
	if (rc)
		return rc;

	/*
	 * Under quality_mutex: force a DQT refresh if the header was just
	 * (re)created (quality_programmed == 0) or if quality changed since
	 * the last frame.  Both the cache update and the HW DMI upload are
	 * done here so that hdr_cache and the hardware are always in sync
	 * before jpeg_exec() fires.
	 */
	mutex_lock(&ectx->quality_mutex);
	if (!ectx->hdr_cache.size || ectx->quality_programmed != ectx->quality_requested) {
		jpeg_update_dqt_cache(ectx);
		jpeg_upload_dmi_table(ectx);
	}
	mutex_unlock(&ectx->quality_mutex);

	/*
	 * Invalidate stale CPU cache lines before writing the JPEG header
	 * with the CPU into the destination buffer.
	 */
	jpeg_sync_sg(jenc->dev, frame, DMA_BIDIRECTIONAL, false);

	dma += qcom_jenc_header_emit(&ectx->hdr_cache, mptr,
				     min_t(size_t, vb->planes[0].length, ectx->hdr_cache.size),
				     q->vf.width, q->vf.height);
	qcom_jenc_dqts_emit(&ectx->hdr_cache, mptr);

	/*
	 * Flush CPU writes to the header before handing the buffer to the
	 * hardware DMA engine.
	 */
	jpeg_sync_sg(jenc->dev, frame, DMA_BIDIRECTIONAL, true);

	jpeg_io_write(jenc, offs->we.pntr[pln], dma);

	dev_dbg(jenc->dev, "%s: pln=%d addr=0x%llx idx:%d\n", __func__,
		pln, dma, vb->index);

	spin_lock_irqsave(&jenc->hw_lock, flags);
	q->buff_id = vb->index;
	spin_unlock_irqrestore(&jenc->hw_lock, flags);

	return 0;
}

static int jpeg_store_we_next(struct jenc_context *ectx, struct vb2_buffer *vb2)
{
	struct qcom_jenc_queue *q = &ectx->bufq[TYPE2QID(vb2->type)];
	struct qcom_jpeg_buff *frame = &q->buff[vb2->index];
	struct sg_table *sgt;
	dma_addr_t dma;

	sgt = vb2_dma_sg_plane_desc(vb2, 0);
	if (!sgt || !sgt->sgl)
		return -EINVAL;

	dma = sg_dma_address(sgt->sgl);
	if (!dma)
		return -EINVAL;

	if (!vb2_plane_vaddr(vb2, 0))
		return -EINVAL;

	frame->plns[0].sgt = sgt;
	frame->plns[0].dma = dma;
	frame->plns[0].size = vb2_plane_size(vb2, 0);

	return 0;
}

static int jpeg_setup_we_size(struct jenc_context *ectx, struct qcom_jenc_queue *q)
{
	struct qcom_jenc_dev *jenc = ectx->jenc;
	const struct qcom_jpeg_reg_offs *offs = jenc->res->hw_offs;
	struct v4l2_pix_format_mplane *dfmt = &q->vf;
	u8 pln;

	for (pln = 0; pln < QCOM_JPEG_MAX_PLANES; pln++)
		jpeg_rw_bits(jenc, offs->we.stride[pln], JMSK_PLNS_WR_STRIDE, 0);

	jpeg_io_write(jenc, offs->we.bsize[0], dfmt->plane_fmt[0].sizeimage);

	dev_dbg(ectx->dev, "%s: ctx=%p size=%u\n", __func__,
		ectx, dfmt->plane_fmt[0].sizeimage);

	return 0;
}

static int jpeg_setup_we_hinit(struct jenc_context *ectx, struct qcom_jenc_queue *q)
{
	struct qcom_jenc_dev *jenc = ectx->jenc;
	const struct qcom_jpeg_reg_offs *offs = jenc->res->hw_offs;
	struct v4l2_pix_format_mplane *dfmt = &q->vf;
	u8 pln;

	if (!dfmt->width) {
		dev_err(ectx->dev, "%s: invalid destination width=%d\n", __func__, dfmt->width);
		return -EINVAL;
	}

	for (pln = 0; pln < QCOM_JPEG_MAX_PLANES; pln++) {
		jpeg_rw_bits(jenc, offs->we.hinit[pln], JMSK_PLNS_WR_HINIT, 0);
		jpeg_rw_bits(jenc, offs->we.hstep[pln], JMSK_PLNS_WR_HSTEP, 0);
	}

	jpeg_rw_bits(jenc, offs->we.hstep[0], JMSK_PLNS_WR_HSTEP, dfmt->width);

	dev_dbg(ectx->dev, "%s: ctx=%p hstep=%u\n", __func__, ectx,
		jpeg_rd_bits(jenc, offs->we.hstep[0], JMSK_PLNS_WR_HSTEP));

	return 0;
}

static int jpeg_setup_we_vinit(struct jenc_context *ectx, struct qcom_jenc_queue *q)
{
	struct qcom_jenc_dev *jenc = ectx->jenc;
	const struct qcom_jpeg_reg_offs *offs = jenc->res->hw_offs;
	struct v4l2_pix_format_mplane *dfmt = &q->vf;
	u8 pln;

	if (!dfmt->height) {
		dev_err(ectx->dev, "%s: invalid destination height=%d\n", __func__, dfmt->height);
		return -EINVAL;
	}

	for (pln = 0; pln < QCOM_JPEG_MAX_PLANES; pln++) {
		jpeg_rw_bits(jenc, offs->we.vinit[pln], JMSK_PLNS_WR_VINIT, 0);
		jpeg_rw_bits(jenc, offs->we.vstep[pln], JMSK_PLNS_WR_VSTEP, 0);
	}

	jpeg_rw_bits(jenc, offs->we.vstep[0], JMSK_PLNS_WR_VSTEP, dfmt->height);

	dev_dbg(ectx->dev, "%s: ctx=%p vstep=%u\n", __func__, ectx,
		jpeg_rd_bits(jenc, offs->we.vstep[0], JMSK_PLNS_WR_VSTEP));

	return 0;
}

static int jpeg_setup_we_params(struct jenc_context *ectx, struct qcom_jenc_queue *q)
{
	struct qcom_jenc_dev *jenc = ectx->jenc;
	const struct qcom_jpeg_reg_offs *offs = jenc->res->hw_offs;
	struct v4l2_pix_format_mplane *dfmt = &q->vf;
	u32 blk_w, blk_h, mcu_cols, mcu_rows;
	int rval;

	rval = jpeg_get_memory_fmt(dfmt->pixelformat);
	if (rval < 0) {
		dev_err(ectx->dev, "%s: invalid memory format for v4l2 format:0x%x\n",
			__func__, dfmt->pixelformat);
		return -EINVAL;
	}
	jpeg_rw_bits(jenc, offs->we_cfg, JMSK_WE_CFG_MEMORY_FORMAT, rval);

	rval = jpeg_get_mal_boundary(dfmt->width, jpeg_mal_bounds, ARRAY_SIZE(jpeg_mal_bounds));
	if (rval < 0) {
		dev_err(ectx->dev, "%s: failed to get WE mal boundary width=%u\n",
			__func__, dfmt->width);
		return -EINVAL;
	}
	jpeg_rw_bits(jenc, offs->we_cfg, JMSK_WE_CFG_MAL_BOUNDARY, rval);

	dev_dbg(ectx->dev, "%s: optimal WE mal boundary=%d\n", __func__,
		jpeg_rd_bits(jenc, offs->we_cfg, JMSK_WE_CFG_MAL_BOUNDARY));

	rval = jpeg_get_encode_fmt(dfmt->pixelformat);
	if (rval < 0) {
		dev_err(ectx->dev, "%s: unsupported encode format fourcc=0x%x\n",
			__func__, dfmt->pixelformat);
		return rval;
	}

	rval = jpeg_get_mcu_geometry(rval, dfmt->width, dfmt->height, &blk_w, &blk_h,
				     &mcu_cols, &mcu_rows);
	if (rval < 0) {
		dev_err(ectx->dev, "%s: invalid MCU geometry mcu_cols=%d mcu_rows=%d\n",
			__func__, mcu_cols, mcu_rows);
		return rval;
	}

	dev_dbg(ectx->dev, "%s blk_w=%u blk_h=%u cols=%u rows=%u\n", __func__,
		blk_w, blk_h, mcu_cols, mcu_rows);

	jpeg_rw_bits(jenc, offs->we.blocks[0], JMSK_PLNS_WR_BLOCK_CFG_PER_RAW, mcu_rows - 1);
	jpeg_rw_bits(jenc, offs->we.blocks[0], JMSK_PLNS_WR_BLOCK_CFG_PER_COL, mcu_cols - 1);

	jpeg_rw_bits(jenc, offs->we_cfg, JMSK_WE_CFG_CBCR_ORDER, 1);
	jpeg_rw_bits(jenc, offs->we_cfg, JMSK_WE_CFG_MAL_EN, 1);
	jpeg_rw_bits(jenc, offs->we_cfg, JMSK_WE_CFG_POP_BUFF_ON_EOS, 1);
	jpeg_rw_bits(jenc, offs->we_cfg, JMSK_WE_CFG_PLN0_EN, 1);

	jpeg_rw_bits(jenc, offs->core_cfg, JMSK_CORE_CFG_MODE, 1);
	jpeg_rw_bits(jenc, offs->core_cfg, JMSK_CORE_CFG_WE_ENABLE, 1);

	return 0;
}

static int jpeg_setup_we(struct jenc_context *ectx, struct qcom_jenc_queue *q)
{
	int rc;

	rc = jpeg_setup_we_size(ectx, q);
	if (rc)
		return rc;

	rc = jpeg_setup_we_hinit(ectx, q);
	if (rc)
		return rc;

	rc = jpeg_setup_we_vinit(ectx, q);
	if (rc)
		return rc;

	return jpeg_setup_we_params(ectx, q);
}

static int jpeg_setup_scale(struct jenc_context *ectx)
{
	struct qcom_jenc_dev *jenc = ectx->jenc;
	const struct qcom_jpeg_reg_offs *offs = jenc->res->hw_offs;
	struct qcom_jenc_queue *sq = &ectx->bufq[JENC_SRC_QUEUE];
	struct qcom_jenc_queue *dq = &ectx->bufq[JENC_DST_QUEUE];
	struct v4l2_pix_format_mplane *sfmt = &sq->vf;
	struct v4l2_pix_format_mplane *dfmt = &dq->vf;
	u32 blk_w, blk_h, mcu_cols, mcu_rows;
	int rval;
	u8 pln;

	jpeg_rw_bits(jenc, offs->reset_cmd, JMSK_RST_CMD_SCALE_RESET, 1);

	/* explicit no scaling */
	jpeg_rw_bits(jenc, offs->scale_cfg, JMSK_SCALE_CFG_HSCALE_ENABLE, 0);
	jpeg_rw_bits(jenc, offs->scale_cfg, JMSK_SCALE_CFG_VSCALE_ENABLE, 0);

	for (pln = 0; pln < QCOM_JPEG_MAX_PLANES; pln++) {
		jpeg_io_write(jenc, offs->scale.hstep[pln], JPEG_DEFAULT_SCALE_STEP);
		jpeg_io_write(jenc, offs->scale.vstep[pln], JPEG_DEFAULT_SCALE_STEP);
	}

	if (jpeg_rd_bits(jenc, offs->scale_cfg, JMSK_SCALE_CFG_HSCALE_ENABLE)) {
		for (pln = 0; pln < sq->vf.num_planes; pln++) {
			jpeg_rw_bits(jenc, offs->scale.hstep[pln],
				     JMSK_SCALE_PLNS_HSTEP_INTEGER,
				     jpeg_calc_scale_int(sfmt->width, dfmt->width));
			jpeg_rw_bits(jenc, offs->scale.hstep[pln],
				     JMSK_SCALE_PLNS_HSTEP_FRACTIONAL,
				     jpeg_calc_scale_frac(sfmt->width, dfmt->width));

			dev_dbg(ectx->dev, "%s: ctx=%p hint=%d hfrac=%d\n",
				__func__, ectx,
				jpeg_rd_bits(jenc, offs->scale.hstep[pln],
					     JMSK_SCALE_PLNS_HSTEP_INTEGER),
				jpeg_rd_bits(jenc, offs->scale.hstep[pln],
					     JMSK_SCALE_PLNS_HSTEP_FRACTIONAL));
		}
	}

	if (jpeg_rd_bits(jenc, offs->scale_cfg, JMSK_SCALE_CFG_VSCALE_ENABLE)) {
		for (pln = 0; pln < sq->vf.num_planes; pln++) {
			jpeg_rw_bits(jenc, offs->scale.vstep[pln],
				     JMSK_SCALE_PLNS_VSTEP_INTEGER,
				     jpeg_calc_scale_int(sfmt->height, dfmt->height));
			jpeg_rw_bits(jenc, offs->scale.vstep[pln],
				     JMSK_SCALE_PLNS_VSTEP_FRACTIONAL,
				     jpeg_calc_scale_frac(sfmt->height, dfmt->height));

			dev_dbg(ectx->dev, "%s: ctx=%p vint=%d vfrac=%d\n",
				__func__, ectx,
				jpeg_rd_bits(jenc, offs->scale.vstep[pln],
					     JMSK_SCALE_PLNS_VSTEP_INTEGER),
				jpeg_rd_bits(jenc, offs->scale.vstep[pln],
					     JMSK_SCALE_PLNS_VSTEP_FRACTIONAL));
		}
	}

	rval = jpeg_get_encode_fmt(sfmt->pixelformat);
	if (rval < 0) {
		dev_err(ectx->dev, "%s: unsupported encode format fourcc=0x%x\n",
			__func__, sfmt->pixelformat);
		return -EINVAL;
	}

	rval = jpeg_get_mcu_geometry(rval, dfmt->width, dfmt->height, &blk_w, &blk_h,
				     &mcu_cols, &mcu_rows);
	if (rval < 0) {
		dev_err(ectx->dev, "%s: invalid MCU geometry blk_w=%d blk_h=%d\n",
			__func__, blk_w, blk_h);
		return -EINVAL;
	}

	dev_dbg(ectx->dev, "%s blk_w=%u blk_h=%u cols=%u rows=%u\n", __func__, blk_w, blk_h,
		mcu_cols, mcu_rows);

	for (pln = 0; pln < sq->vf.num_planes; pln++) {
		jpeg_rw_bits(jenc, offs->scale_out_cfg[pln],
			     JMSK_SCALE_PLNS_OUT_CFG_BLK_WIDTH, mcu_cols - 1);
		jpeg_rw_bits(jenc, offs->scale_out_cfg[pln],
			     JMSK_SCALE_PLNS_OUT_CFG_BLK_HEIGHT, mcu_rows - 1);
	}

	dev_dbg(ectx->dev, "%s: ctx=%p scale src=%ux%u dst=%ux%u enable=%d/%d\n",
		__func__, ectx, sfmt->width, sfmt->height, dfmt->width, dfmt->height,
		jpeg_rd_bits(jenc, offs->scale_cfg, JMSK_SCALE_CFG_HSCALE_ENABLE),
		jpeg_rd_bits(jenc, offs->scale_cfg, JMSK_SCALE_CFG_VSCALE_ENABLE));

	/* Disabled, but must be configured */
	jpeg_rw_bits(jenc, offs->core_cfg, JMSK_CORE_CFG_SCALE_ENABLE, 0);

	return 0;
}

static int jpeg_setup_encode(struct jenc_context *ectx)
{
	struct qcom_jenc_dev *jenc = ectx->jenc;
	struct qcom_jenc_queue *sq = &ectx->bufq[JENC_SRC_QUEUE];
	struct v4l2_pix_format_mplane *sfmt = &sq->vf;
	const struct qcom_jpeg_reg_offs *offs = jenc->res->hw_offs;
	u32 blk_w, blk_h, mcu_cols, mcu_rows;
	int rval;

	if (!sfmt->width || !sfmt->height)
		return -EINVAL;

	jpeg_rw_bits(jenc, offs->reset_cmd, JMSK_RST_CMD_ENCODER_RESET, 1);

	rval = jpeg_get_encode_fmt(sfmt->pixelformat);
	if (rval < 0) {
		dev_err(ectx->dev, "%s: unsupported encode format fourcc=0x%x\n",
			__func__, sfmt->pixelformat);
		return -EINVAL;
	}
	jpeg_rw_bits(jenc, offs->enc_cfg, JMSK_ENC_CFG_IMAGE_FORMAT, rval);

	rval = jpeg_get_mcu_geometry(rval, sfmt->width, sfmt->height, &blk_w, &blk_h,
				     &mcu_cols, &mcu_rows);
	if (rval < 0) {
		dev_err(ectx->dev, "%s: invalid MCU geometry mcu_cols=%d mcu_rows=%d\n",
			__func__, mcu_cols, mcu_rows);
		return -EINVAL;
	}

	dev_dbg(ectx->dev, "%s blk_w=%u blk_h=%u cols=%u rows=%u\n", __func__,
		blk_w, blk_h, mcu_cols, mcu_rows);

	jpeg_rw_bits(jenc, offs->enc_img_size, JMSK_ENC_IMAGE_SIZE_WIDTH, mcu_cols - 1);
	jpeg_rw_bits(jenc, offs->enc_img_size, JMSK_ENC_IMAGE_SIZE_HEIGHT, mcu_rows - 1);

	dev_dbg(ectx->dev, "%s: ctx=%p width=%d height=%d\n", __func__, ectx,
		jpeg_rd_bits(jenc, offs->enc_img_size, JMSK_ENC_IMAGE_SIZE_WIDTH),
		jpeg_rd_bits(jenc, offs->enc_img_size, JMSK_ENC_IMAGE_SIZE_HEIGHT));

	jpeg_rw_bits(jenc, offs->enc_cfg, JMSK_ENC_CFG_APPLY_EOI, 1);
	jpeg_rw_bits(jenc, offs->core_cfg, JMSK_CORE_CFG_ENC_ENABLE, 1);

	return 0;
}

static irqreturn_t op_jpeg_irq_bot(int irq, void *data)
{
	struct qcom_jenc_dev *jenc = data;
	const struct qcom_jpeg_reg_offs *offs = jenc->res->hw_offs;
	u32 irq_status;
	u32 irq_mask;
	unsigned long flags;

	irq_status = READ_ONCE(jenc->pending_irq_status);

	irq_mask = jenc->res->hw_mask[JMSK_IRQ_STATUS_SESSION_DONE];
	if (jpeg_bits_get(irq_mask, irq_status)) {
		struct jenc_context *ctx = jenc->actx;
		struct qcom_jenc_queue *dq;
		size_t out_size;

		spin_lock_irqsave(&jenc->hw_lock, flags);
		jenc->actx = NULL;
		spin_unlock_irqrestore(&jenc->hw_lock, flags);

		if (!ctx)
			return IRQ_HANDLED;

		dq = &ctx->bufq[JENC_DST_QUEUE];
		if (dq->buff_id >= 0) {
			struct qcom_jpeg_buff *frame;
			unsigned long flags;

			spin_lock_irqsave(&jenc->hw_lock, flags);
			frame = &dq->buff[dq->buff_id];
			out_size = jpeg_io_read(jenc, offs->enc_out_size);
			spin_unlock_irqrestore(&jenc->hw_lock, flags);

			dev_dbg(jenc->dev, "complete idx:%d addr=0x%llx size=%zu\n",
				dq->buff_id, frame->plns[0].dma, out_size);

			jenc->enc_hw_irq_cb(ctx, VB2_BUF_STATE_DONE,
					    out_size + JPEG_HEADER_MAX);
			jpeg_stop(jenc);
		}
	}

	irq_mask = jenc->res->hw_mask[JMSK_IRQ_STATUS_SESSION_ERROR];
	if (jpeg_bits_get(irq_mask, irq_status)) {
		struct jenc_context *ctx = jenc->actx;

		spin_lock_irqsave(&jenc->hw_lock, flags);
		jenc->actx = NULL;
		spin_unlock_irqrestore(&jenc->hw_lock, flags);

		dev_err(jenc->dev, "encoder hardware failure=0x%x\n",
			jpeg_bits_get(irq_mask, irq_status));
		if (ctx)
			jenc->enc_hw_irq_cb(ctx, VB2_BUF_STATE_ERROR, 0);

		jpeg_stop(jenc);
	}

	return IRQ_HANDLED;
}

static irqreturn_t op_jpeg_irq_top(int irq, void *data)
{
	struct qcom_jenc_dev *jenc = data;
	const struct qcom_jpeg_reg_offs *offs = jenc->res->hw_offs;
	u32 irq_status;
	u32 irq_mask;
	unsigned long flags;

	spin_lock_irqsave(&jenc->hw_lock, flags);

	irq_status = jpeg_io_read(jenc, offs->int_status);
	jpeg_wo_bits(jenc, offs->int_clr, JMSK_IRQ_STATUS_ALL_BITS, irq_status);

	irq_mask = jenc->res->hw_mask[JMSK_IRQ_STATUS_RESET_ACK];
	if (jpeg_bits_get(irq_mask, irq_status)) {
		complete(&jenc->reset_complete);
		spin_unlock_irqrestore(&jenc->hw_lock, flags);
		return IRQ_HANDLED;
	}

	irq_mask = jenc->res->hw_mask[JMSK_IRQ_STATUS_STOP_ACK];
	if (jpeg_bits_get(irq_mask, irq_status)) {
		complete(&jenc->stop_complete);
		dev_dbg(jenc->dev, "hardware stop acknowledged\n");
		spin_unlock_irqrestore(&jenc->hw_lock, flags);
		return IRQ_HANDLED;
	}

	WRITE_ONCE(jenc->pending_irq_status, irq_status);

	spin_unlock_irqrestore(&jenc->hw_lock, flags);

	return IRQ_WAKE_THREAD;
}

static void op_jpeg_get_hw_caps(struct qcom_jenc_dev *jenc, u32 *caps)
{
	const struct qcom_jpeg_reg_offs *offs = jenc->res->hw_offs;
	u32 hw_caps;

	hw_caps = jpeg_io_read(jenc, offs->hw_capability);
	dev_dbg(jenc->dev, "CAPS: encode=%d decode=%d upscale=%d downscale=%d\n",
		jpeg_bits_get(jenc->res->hw_mask[JMSK_HW_CAP_ENCODE], hw_caps),
		jpeg_bits_get(jenc->res->hw_mask[JMSK_HW_CAP_DECODE], hw_caps),
		jpeg_bits_get(jenc->res->hw_mask[JMSK_HW_CAP_UPSCALE], hw_caps),
		jpeg_bits_get(jenc->res->hw_mask[JMSK_HW_CAP_DOWNSCALE], hw_caps));

	*caps = hw_caps;
}

static struct qcom_jenc_queue *op_jpeg_get_buff_queue(struct jenc_context *ectx,
						      enum qcom_enc_qid id)
{
	return &ectx->bufq[id];
}

static int op_jpeg_queue_setup(struct jenc_context *ectx, enum qcom_enc_qid id)
{
	int rc;

	if (id == JENC_SRC_QUEUE) {
		struct qcom_jenc_queue *q = &ectx->bufq[id];

		rc = qcom_jenc_header_init(&ectx->hdr_cache, q->vf.pixelformat);
		if (rc) {
			dev_err(ectx->dev, "JFIF header init failed\n");
			return rc;
		}
	}

	return 0;
}

static int op_jpeg_src_fmt_update(struct jenc_context *ectx, u32 old_fourcc, u32 new_fourcc)
{
	bool old_is_mono = (old_fourcc == V4L2_PIX_FMT_GREY);
	bool new_is_mono = (new_fourcc == V4L2_PIX_FMT_GREY);
	int rc;

	/* Header layout changes only for mono <-> color source format switch. */
	if (old_is_mono == new_is_mono)
		return 0;

	rc = qcom_jenc_header_init(&ectx->hdr_cache, new_fourcc);
	if (rc) {
		dev_err(ectx->dev, "JFIF header reinit failed\n");
		return rc;
	}

	/* Force DQT upload after source profile switch. */
	ectx->quality_programmed = 0;

	return 0;
}

static int op_jpeg_buffer_prepare(struct jenc_context *ectx, struct vb2_buffer *vb2)
{
	int rc;

	if (V4L2_TYPE_IS_OUTPUT(vb2->type)) {
		rc = jpeg_store_fe_next(ectx, vb2);
		if (rc)
			dev_err(ectx->dev, "%s: cannot set up fetch addr\n", __func__);
	} else {
		rc = jpeg_store_we_next(ectx, vb2);
		if (rc)
			dev_err(ectx->dev, "%s: cannot set up write addr\n", __func__);
	}

	return rc;
}

static int op_jpeg_process_exec(struct qcom_jenc_dev *jenc, struct jenc_context *ectx,
				struct vb2_buffer *vb)
{
	struct qcom_jenc_queue *sq = &ectx->bufq[JENC_SRC_QUEUE];
	struct qcom_jenc_queue *dq = &ectx->bufq[JENC_DST_QUEUE];
	unsigned long flags;
	int rc;

	spin_lock_irqsave(&jenc->hw_lock, flags);
	jenc->actx = ectx;
	spin_unlock_irqrestore(&jenc->hw_lock, flags);

	if (V4L2_TYPE_IS_OUTPUT(vb->type)) {
		rc = jpeg_setup_fe(ectx, sq);
		if (rc)
			goto err_clear_ctx;

		rc = jpeg_apply_fe_addr(ectx, sq, vb);
		if (rc)
			goto err_clear_ctx;
	} else {
		rc = jpeg_setup_we(ectx, dq);
		if (rc)
			goto err_clear_ctx;

		rc = jpeg_apply_we_addr(ectx, dq, vb);
		if (rc)
			goto err_clear_ctx;
	}

	if (sq->sequence == dq->sequence) {
		rc = jpeg_setup_scale(ectx);
		if (rc)
			goto err_clear_ctx;

		rc = jpeg_setup_encode(ectx);
		if (rc)
			goto err_clear_ctx;

		jpeg_exec(jenc);
	}

	return 0;

err_clear_ctx:
	spin_lock_irqsave(&jenc->hw_lock, flags);
	if (jenc->actx == ectx)
		jenc->actx = NULL;
	spin_unlock_irqrestore(&jenc->hw_lock, flags);

	return rc;
}

static int op_jpeg_prepare(struct qcom_jenc_dev *jenc)
{
	const struct qcom_jpeg_reg_offs *offs = jenc->res->hw_offs;

	jpeg_rw_bits(jenc, offs->reset_cmd, JMSK_RST_CMD_DECODER_RESET, 1);
	jpeg_rw_bits(jenc, offs->reset_cmd, JMSK_RST_CMD_BLOCK_FORMATTER_RST, 1);
	jpeg_rw_bits(jenc, offs->reset_cmd, JMSK_RST_CMD_CORE_RESET, 1);

	return 0;
}

static enum qcom_soc_perf_level jpeg_select_perf_level(struct jenc_context *ectx)
{
	struct qcom_jenc_queue *sq = &ectx->bufq[JENC_SRC_QUEUE];
	u64 pixels, load;
	u32 quality;
	u32 qscale;

	if (!sq->vf.width || !sq->vf.height)
		return QCOM_SOC_PERF_NOMINAL;

	mutex_lock(&ectx->quality_mutex);
	quality = ectx->quality_requested;
	mutex_unlock(&ectx->quality_mutex);

	/* Minimal v1 quality weighting: low/medium/high quality buckets. */
	if (quality <= 70)
		qscale = 90;
	else if (quality <= 90)
		qscale = 100;
	else
		qscale = 115;

	pixels = (u64)sq->vf.width * sq->vf.height;
	load = pixels * qscale;

	/* Keep a conservative floor for active encode sessions. */
	if (load <= (u64)1920 * 1088 * 100)
		return QCOM_SOC_PERF_SVS_L1;

	if (load <= (u64)3840 * 2160 * 100)
		return QCOM_SOC_PERF_NOMINAL;

	return QCOM_SOC_PERF_TURBO;
}

static int op_jpeg_acquire(struct jenc_context *ectx, struct vb2_queue *q)
{
	struct qcom_jenc_dev *jenc = ectx->jenc;
	struct qcom_jenc_queue *sq = &ectx->bufq[JENC_SRC_QUEUE];
	struct qcom_jenc_queue *dq = &ectx->bufq[JENC_DST_QUEUE];
	int rc;

	/* Reset per-context stream state for each (re)acquire. */
	sq->sequence = 0;
	sq->buff_id = -1;
	dq->sequence = 0;
	dq->buff_id = -1;
	/*
	 * Recreate JPEG header lazily per destination buffer to tolerate
	 * different valid V4L2 call orders (e.g. STREAMON before first QBUF).
	 */
	ectx->hdr_cache.size = 0;
	/* Force DQT upload on first frame after (re)acquire. */
	ectx->quality_programmed = 0;

	if (atomic_inc_return(&jenc->ref_count) == 1) {
		jenc->perf = jpeg_select_perf_level(ectx);
		dev_dbg(jenc->dev, "%s: perf=%u src=%ux%u\n", __func__, jenc->perf,
			sq->vf.width, sq->vf.height);

		rc = pm_runtime_resume_and_get(jenc->dev);
		if (rc < 0) {
			dev_err(jenc->dev, "PM runtime get failed\n");
			atomic_dec(&jenc->ref_count);
			return rc;
		}

		rc = jpeg_init(jenc);
		if (rc) {
			dev_err(jenc->dev, "hardware init failed\n");
			atomic_dec(&jenc->ref_count);
			pm_runtime_put(jenc->dev);
			return rc;
		}
	}

	return 0;
}

static int op_jpeg_release(struct jenc_context *ectx, struct vb2_queue *q)
{
	struct qcom_jenc_dev *jenc = ectx->jenc;
	int rc = 0;
	int pm_rc;
	int ref;

	ref = atomic_dec_if_positive(&jenc->ref_count);
	if (ref < 0) {
		WARN_ON_ONCE(1);
		return 0;
	}

	if (!ref) {
		rc = jpeg_deinit(jenc);
		if (rc)
			dev_err(jenc->dev, "hardware exit failed\n");

		pm_rc = pm_runtime_put_sync(jenc->dev);
		if (pm_rc < 0) {
			dev_err(jenc->dev, "PM runtime put failed\n");
			if (!rc)
				rc = pm_rc;
		}

		dev_dbg(jenc->dev, "JPEG HW encoder released\n");
	}

	return rc;
}

const struct qcom_jpeg_hw_ops qcom_jpeg_default_ops = {
	.hw_get_cap	= op_jpeg_get_hw_caps,
	.hw_acquire	= op_jpeg_acquire,
	.hw_release	= op_jpeg_release,
	.hw_prepare	= op_jpeg_prepare,
	.get_queue	= op_jpeg_get_buff_queue,
	.queue_setup	= op_jpeg_queue_setup,
	.src_fmt_update	= op_jpeg_src_fmt_update,
	.buf_prepare	= op_jpeg_buffer_prepare,
	.process_exec	= op_jpeg_process_exec,
	.hw_irq_top	= op_jpeg_irq_top,
	.hw_irq_bot	= op_jpeg_irq_bot
};
