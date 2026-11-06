// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/bitfield.h>
#include <linux/math64.h>
#include <linux/pm_runtime.h>
#include <linux/scatterlist.h>
#include <linux/string.h>

#include <media/jpeg.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-jpeg.h>
#include <media/videobuf2-dma-sg.h>

#include "qcom_jenc_dev.h"
#include "qcom_jenc_v420.h"
#include "qcom_jenc_v420_hw_info.h"

/* JPEG quality constants */
#define QCOM_JPEG_QUALITY_MID	(QCOM_JPEG_QUALITY_MAX / 2)

/* Per-plane register offset lookup arrays (indexed by plane number 0..2) */
static const u32 jpeg_v420_fe_pntr[] = {
	JPEG_V420_REG_FE_PNTR_0,
	JPEG_V420_REG_FE_PNTR_1,
	JPEG_V420_REG_FE_PNTR_2,
};

static const u32 jpeg_v420_fe_offs[] = {
	JPEG_V420_REG_FE_OFFS_0,
	JPEG_V420_REG_FE_OFFS_1,
	JPEG_V420_REG_FE_OFFS_2,
};

static const u32 jpeg_v420_fe_bsize[] = {
	JPEG_V420_REG_FE_BSIZE_0,
	JPEG_V420_REG_FE_BSIZE_1,
	JPEG_V420_REG_FE_BSIZE_2,
};

static const u32 jpeg_v420_fe_stride[] = {
	JPEG_V420_REG_FE_STRIDE_0,
	JPEG_V420_REG_FE_STRIDE_1,
	JPEG_V420_REG_FE_STRIDE_2,
};

static const u32 jpeg_v420_fe_hinit[] = {
	JPEG_V420_REG_FE_HINIT_0,
	JPEG_V420_REG_FE_HINIT_1,
	JPEG_V420_REG_FE_HINIT_2,
};

static const u32 jpeg_v420_fe_vinit[] = {
	JPEG_V420_REG_FE_VINIT_0,
	JPEG_V420_REG_FE_VINIT_1,
	JPEG_V420_REG_FE_VINIT_2,
};

static const u32 jpeg_v420_we_pntr[] = {
	JPEG_V420_REG_WE_PNTR_0,
	JPEG_V420_REG_WE_PNTR_1,
	JPEG_V420_REG_WE_PNTR_2,
};

static const u32 jpeg_v420_we_stride[] = {
	JPEG_V420_REG_WE_STRIDE_0,
	JPEG_V420_REG_WE_STRIDE_1,
	JPEG_V420_REG_WE_STRIDE_2,
};

static const u32 jpeg_v420_we_hinit[] = {
	JPEG_V420_REG_WE_HINIT_0,
	JPEG_V420_REG_WE_HINIT_1,
	JPEG_V420_REG_WE_HINIT_2,
};

static const u32 jpeg_v420_we_hstep[] = {
	JPEG_V420_REG_WE_HSTEP_0,
	JPEG_V420_REG_WE_HSTEP_1,
	JPEG_V420_REG_WE_HSTEP_2,
};

static const u32 jpeg_v420_we_vinit[] = {
	JPEG_V420_REG_WE_VINIT_0,
	JPEG_V420_REG_WE_VINIT_1,
	JPEG_V420_REG_WE_VINIT_2,
};

static const u32 jpeg_v420_we_vstep[] = {
	JPEG_V420_REG_WE_VSTEP_0,
	JPEG_V420_REG_WE_VSTEP_1,
	JPEG_V420_REG_WE_VSTEP_2,
};

static const u32 jpeg_v420_scale_hstep[] = {
	JPEG_V420_REG_SCALE_HSTEP_0,
	JPEG_V420_REG_SCALE_HSTEP_1,
	JPEG_V420_REG_SCALE_HSTEP_2,
};

static const u32 jpeg_v420_scale_vstep[] = {
	JPEG_V420_REG_SCALE_VSTEP_0,
	JPEG_V420_REG_SCALE_VSTEP_1,
	JPEG_V420_REG_SCALE_VSTEP_2,
};

static const u32 jpeg_v420_scale_out_cfg[] = {
	JPEG_V420_REG_SCALE_OUT_CFG_0,
	JPEG_V420_REG_SCALE_OUT_CFG_1,
	JPEG_V420_REG_SCALE_OUT_CFG_2,
};

#define JPEG_RESET_TIMEOUT_MS	300
#define JPEG_STOP_TIMEOUT_MS	200

#define JPEG_DQT_SHIFT		20

#define JPEG_MCU_BLOCK_8	8
#define JPEG_MCU_BLOCK_16	16

#define JPEG_DEFAULT_SCALE_STEP	0x200000

/* The core encodes 430 Mpixel/s at 600 MHz; the clock is scaled for 60 fps */
#define JPEG_REF_CLK_HZ			600000000UL
#define JPEG_REF_THROUGHPUT_MPPS	430UL
#define JPEG_REF_FPS			60UL

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
	enum qcom_jpeg_encode_fmt encode;
	enum qcom_jpeg_memory_fmt memory;
	u32 fourcc;
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
	{ .bytes = 2048, .boundary = JPEG_CFG_MAL_BOUND_2K_BYTES  },
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

static inline int jpeg_get_mcu_geometry(enum qcom_jpeg_encode_fmt fmt, u32 width, u32 height,
					u32 *blk_w, u32 *blk_h, u32 *mcu_cols, u32 *mcu_rows)
{
	const struct qcom_jpeg_scale_blocks *blks;
	u32 bw = 0, bh = 0;
	u8 pln;

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

static inline u32 jpeg_io_read(struct qcom_jenc_dev *jenc, u32 offset)
{
	return readl(jenc->jpeg_base + offset);
}

static inline void jpeg_io_write(struct qcom_jenc_dev *jenc, u32 offset, u32 value)
{
	writel(value, jenc->jpeg_base + offset);
}

static inline u32 jpeg_rd_bits(struct qcom_jenc_dev *jenc, u32 offs, u32 mask)
{
	u32 reg = jpeg_io_read(jenc, offs);

	return field_get(mask, reg);
}

/*
 * Read-modify-write (for R/W registers)
 */
static inline void jpeg_rw_bits(struct qcom_jenc_dev *jenc, u32 offs, u32 mask, u32 val)
{
	u32 reg = jpeg_io_read(jenc, offs);

	reg &= ~mask;
	reg |= field_prep(mask, val);

	jpeg_io_write(jenc, offs, reg);
}

static u8 jpeg_calculate_dqt(struct jenc_context *ectx, u8 dqt_value)
{
	u64 calc_val;
	u64 ratio;

	ratio = (QCOM_JPEG_QUALITY_MAX - ectx->quality_requested) << JPEG_DQT_SHIFT;
	ratio = max_t(u64, 1, ratio);
	do_div(ratio, QCOM_JPEG_QUALITY_MID);

	calc_val = DIV64_U64_ROUND_CLOSEST(ratio * dqt_value, 1LU << JPEG_DQT_SHIFT);

	return clamp_t(u64, calc_val, 1, U8_MAX);
}

/*
 * jpeg_update_dqt_cache - compute scaled DQT coefficients and store them in
 * the software JPEG header cache (hdr_cache).  Safe to call before the
 * hardware is powered on; no MMIO access is performed here. The DQT segment
 * lists coefficients in zigzag order, so each zigzag position is mapped back
 * to the natural position of the reference table.
 */
static void jpeg_update_dqt_cache(struct jenc_context *ectx)
{
	u8 *base;
	u8 dqt_val, nat;
	int zz;

	/* Luma DQT cache update */
	if (ectx->hdr_cache.layout.dqt_offset[0]) {
		base = &ectx->hdr_cache.data[ectx->hdr_cache.layout.dqt_offset[0]];
		for (zz = 0; zz < V4L2_JPEG_PIXELS_IN_BLOCK; zz++) {
			nat = v4l2_jpeg_zigzag_scan_index[zz];
			dqt_val = jpeg_calculate_dqt(ectx, v4l2_jpeg_ref_table_luma_qt[nat]);
			base[zz] = dqt_val;
		}
	}

	/* Chroma DQT cache update */
	if (ectx->hdr_cache.layout.dqt_offset[1]) {
		base = &ectx->hdr_cache.data[ectx->hdr_cache.layout.dqt_offset[1]];
		for (zz = 0; zz < V4L2_JPEG_PIXELS_IN_BLOCK; zz++) {
			nat = v4l2_jpeg_zigzag_scan_index[zz];
			dqt_val = jpeg_calculate_dqt(ectx, v4l2_jpeg_ref_table_chroma_qt[nat]);
			base[zz] = dqt_val;
		}
	}
}

/*
 * jpeg_dqt_to_natural - reorder one DQT table from the zigzag order of the
 * JPEG header to the natural (raster) order taken by the hardware DMI.
 */
static void jpeg_dqt_to_natural(const u8 *zz_qt, u8 *nat_qt)
{
	int zz;

	for (zz = 0; zz < V4L2_JPEG_PIXELS_IN_BLOCK; zz++)
		nat_qt[v4l2_jpeg_zigzag_scan_index[zz]] = zz_qt[zz];
}

/*
 * jpeg_upload_dmi_table - write the scaled DQT coefficients to the hardware
 * DMI registers.  Must only be called from the job execution path where
 * runtime PM has already been acquired (pm_runtime_resume_and_get).
 *
 * Reads precomputed values from hdr_cache (populated by jpeg_update_dqt_cache)
 * to avoid redundant per-coefficient recalculation on the hot encode path.
 * The hardware takes them in natural order, as 65536 / q reciprocals.
 */
static void jpeg_upload_dmi_table(struct jenc_context *ectx)
{
	const u8 *luma_qt = &ectx->hdr_cache.data[ectx->hdr_cache.layout.dqt_offset[0]];
	u8 nat_qt[V4L2_JPEG_PIXELS_IN_BLOCK];
	u32 pcfg = JPEG_DMI_ENABLE_UPLOAD;
	u32 addr = 0x00000000;
	u32 reg_val;
	int i;

	/* DMI upload start sequence */
	jpeg_io_write(ectx->jenc, JPEG_V420_REG_DMI_ADDR, addr);
	jpeg_io_write(ectx->jenc, JPEG_V420_REG_DMI_CFG, pcfg);

	/* DMI Luma upload - values are stored in zigzag order in hdr_cache */
	jpeg_dqt_to_natural(luma_qt, nat_qt);
	for (i = 0; i < V4L2_JPEG_PIXELS_IN_BLOCK; i++) {
		reg_val = div_u64(U16_MAX + 1U, nat_qt[i]);
		reg_val = clamp_t(u32, reg_val, 0, U16_MAX);
		jpeg_io_write(ectx->jenc, JPEG_V420_REG_DMI_DATA, reg_val);
	}

	/* DMI Chroma upload - only present for color formats */
	if (ectx->hdr_cache.layout.dqt_offset[1]) {
		const u8 *chroma_qt = &ectx->hdr_cache.data[ectx->hdr_cache.layout.dqt_offset[1]];

		jpeg_dqt_to_natural(chroma_qt, nat_qt);
		for (i = 0; i < V4L2_JPEG_PIXELS_IN_BLOCK; i++) {
			reg_val = div_u64(U16_MAX + 1U, nat_qt[i]);
			reg_val = clamp_t(u32, reg_val, 0, U16_MAX);
			jpeg_io_write(ectx->jenc, JPEG_V420_REG_DMI_DATA, reg_val);
		}
	}

	/* DMI upload end sequence */
	jpeg_io_write(ectx->jenc, JPEG_V420_REG_DMI_CFG, addr);

	ectx->quality_programmed = ectx->quality_requested;

	dev_dbg(ectx->dev, "quality_programmed=%d\n", ectx->quality_programmed);
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

/*
 * Only an explicit CLEAR_RD/WR_PLNS_QUEUE command drains the FE/WE hardware
 * pointer queues; a core reset or HW_STOP does not.
 */
static void jpeg_clear_planes_queues(struct qcom_jenc_dev *jenc)
{
	jpeg_io_write(jenc, JPEG_V420_REG_HW_CMD, JPEG_V420_CMD_CLR_RD_PLNS_QUEUE_MSK);
	jpeg_io_write(jenc, JPEG_V420_REG_HW_CMD, 0);

	jpeg_io_write(jenc, JPEG_V420_REG_HW_CMD, JPEG_V420_CMD_CLR_WR_PLNS_QUEUE_MSK);
	jpeg_io_write(jenc, JPEG_V420_REG_HW_CMD, 0);
}

static int jpeg_init(struct qcom_jenc_dev *jenc)
{
	unsigned long rtime;
	u32 hw_ver;

	if (WARN_ON_ONCE(!jenc || !jenc->dev || !jenc->jpeg_base))
		return -EINVAL;

	jpeg_io_write(jenc, JPEG_V420_REG_INT_CLR, JPEG_V420_IRQ_STATUS_ALL_BITS);

	reinit_completion(&jenc->reset_complete);
	jpeg_rw_bits(jenc, JPEG_V420_REG_INT_MASK, JPEG_V420_IRQ_STATUS_RESET_ACK, 1);

	jpeg_io_write(jenc, JPEG_V420_REG_RESET_CMD, JPEG_V420_RST_CMD_ALL_BITS);

	rtime = wait_for_completion_timeout(&jenc->reset_complete,
					    msecs_to_jiffies(JPEG_RESET_TIMEOUT_MS));
	if (!rtime) {
		dev_err(jenc->dev, "encoder HW reset timeout\n");
		return -ETIME;
	}

	hw_ver = jpeg_io_read(jenc, JPEG_V420_REG_HW_VERSION);
	dev_dbg(jenc->dev, "JPEG HW encoder version %lu.%lu.%lu\n",
		FIELD_GET(JPEG_V420_HW_VER_MAJOR_MSK, hw_ver),
		FIELD_GET(JPEG_V420_HW_VER_MINOR_MSK, hw_ver),
		FIELD_GET(JPEG_V420_HW_VER_STEP_MSK, hw_ver));

	jpeg_clear_planes_queues(jenc);

	jpeg_io_write(jenc, JPEG_V420_REG_INT_CLR, JPEG_V420_IRQ_STATUS_ALL_BITS);
	jpeg_io_write(jenc, JPEG_V420_REG_INT_MASK, JPEG_V420_IRQ_STATUS_ALL_BITS);

	return 0;
}

static void jpeg_exec(struct qcom_jenc_dev *jenc)
{
	jpeg_io_write(jenc, JPEG_V420_REG_HW_CMD, FIELD_PREP(JPEG_V420_CMD_HW_START, 1));
}

static void jpeg_stop(struct qcom_jenc_dev *jenc)
{
	jpeg_io_write(jenc, JPEG_V420_REG_HW_CMD, FIELD_PREP(JPEG_V420_CMD_HW_START, 0));

	jpeg_clear_planes_queues(jenc);
}

static int jpeg_deinit(struct qcom_jenc_dev *jenc)
{
	unsigned long rtime;

	jpeg_io_write(jenc, JPEG_V420_REG_INT_CLR, JPEG_V420_IRQ_STATUS_ALL_BITS);

	reinit_completion(&jenc->stop_complete);
	jpeg_rw_bits(jenc, JPEG_V420_REG_INT_MASK, JPEG_V420_IRQ_STATUS_STOP_ACK, 1);

	jpeg_io_write(jenc, JPEG_V420_REG_HW_CMD, FIELD_PREP(JPEG_V420_CMD_HW_STOP, 1));

	rtime = wait_for_completion_timeout(&jenc->stop_complete,
					    msecs_to_jiffies(JPEG_STOP_TIMEOUT_MS));
	jpeg_io_write(jenc, JPEG_V420_REG_INT_MASK, 0);
	jpeg_io_write(jenc, JPEG_V420_REG_INT_CLR, JPEG_V420_IRQ_STATUS_ALL_BITS);

	if (!rtime) {
		dev_err(jenc->dev, "encoder HW stop timeout\n");
		return -ETIME;
	}

	/*
	 * Only after STOP_ACK: drop the addresses an aborted job left in the
	 * FE/WE queues. A completed job clears them in jpeg_stop().
	 */
	jpeg_clear_planes_queues(jenc);

	return 0;
}

static void jpeg_apply_fe_addr(struct jenc_context *ectx, struct qcom_jenc_queue *q,
			       struct vb2_buffer *vb)
{
	struct qcom_jpeg_buff *frame = &q->buff[vb->index];
	struct v4l2_pix_format_mplane *fmt = &q->vf;
	struct qcom_jenc_dev *jenc = ectx->jenc;
	u8 pln;

	for (pln = 0; pln < fmt->num_planes; pln++) {
		if (!frame->plns[pln].sgt || !frame->plns[pln].sgt->sgl)
			break;

		jpeg_io_write(jenc, jpeg_v420_fe_pntr[pln], frame->plns[pln].dma);
		jpeg_io_write(jenc, jpeg_v420_fe_offs[pln], 0);

		dev_dbg(jenc->dev, "FE pln=%d addr=%pad idx:%d\n",
			pln, &frame->plns[pln].dma, vb->index);
	}
}

static int jpeg_store_fe_next(struct jenc_context *ectx, struct vb2_buffer *vb2)
{
	struct qcom_jenc_queue *q = &ectx->bufq[TYPE2QID(vb2->type)];
	struct qcom_jpeg_buff *buff = &q->buff[vb2->index];
	u8 pln;

	for (pln = 0; pln < q->vf.num_planes; pln++) {
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

static void jpeg_setup_fe_size(struct jenc_context *ectx, struct qcom_jenc_queue *q)
{
	struct v4l2_pix_format_mplane *sfmt = &q->vf;
	struct qcom_jenc_dev *jenc = ectx->jenc;
	u8 pln;

	for (pln = 0; pln < QCOM_JPEG_MAX_PLANES; pln++) {
		jpeg_rw_bits(jenc, jpeg_v420_fe_bsize[pln],
			     JPEG_V420_PLN_RD_BUFF_SIZE_WIDTH_MSK, 0);
		jpeg_rw_bits(jenc, jpeg_v420_fe_bsize[pln],
			     JPEG_V420_PLN_RD_BUFF_SIZE_HEIGHT_MSK, 0);
		jpeg_rw_bits(jenc, jpeg_v420_fe_stride[pln], JPEG_V420_PLN_RD_STRIDE_MSK, 0);
	}

	for (pln = 0; pln < sfmt->num_planes; pln++) {
		jpeg_rw_bits(jenc, jpeg_v420_fe_bsize[pln], JPEG_V420_PLN_RD_BUFF_SIZE_WIDTH_MSK,
			     sfmt->width - 1);
		jpeg_rw_bits(jenc, jpeg_v420_fe_bsize[pln], JPEG_V420_PLN_RD_BUFF_SIZE_HEIGHT_MSK,
			     sfmt->height - 1);
		jpeg_rw_bits(jenc, jpeg_v420_fe_stride[pln], JPEG_V420_PLN_RD_STRIDE_MSK,
			     sfmt->plane_fmt[pln].bytesperline);

		dev_dbg(ectx->dev, "FE pln=%d width=%d height=%d stride=%d\n",
			pln,
			jpeg_rd_bits(jenc, jpeg_v420_fe_bsize[pln],
				     JPEG_V420_PLN_RD_BUFF_SIZE_WIDTH_MSK),
			jpeg_rd_bits(jenc, jpeg_v420_fe_bsize[pln],
				     JPEG_V420_PLN_RD_BUFF_SIZE_HEIGHT_MSK),
			jpeg_rd_bits(jenc, jpeg_v420_fe_stride[pln],
				     JPEG_V420_PLN_RD_STRIDE_MSK));
	}
}

static void jpeg_setup_fe_hinit(struct jenc_context *ectx, struct qcom_jenc_queue *q)
{
	struct qcom_jenc_dev *jenc = ectx->jenc;
	u8 pln;

	for (pln = 0; pln < QCOM_JPEG_MAX_PLANES; pln++)
		jpeg_io_write(jenc, jpeg_v420_fe_hinit[pln], 0);
}

static void jpeg_setup_fe_vinit(struct jenc_context *ectx, struct qcom_jenc_queue *q)
{
	struct qcom_jenc_dev *jenc = ectx->jenc;
	u8 pln;

	for (pln = 0; pln < QCOM_JPEG_MAX_PLANES; pln++)
		jpeg_io_write(jenc, jpeg_v420_fe_vinit[pln], 0);
}

static int jpeg_setup_fe_params(struct jenc_context *ectx, struct qcom_jenc_queue *q)
{
	struct v4l2_pix_format_mplane *sfmt = &q->vf;
	struct qcom_jenc_dev *jenc = ectx->jenc;
	u8 expected_planes, pln;
	int rval;

	jpeg_rw_bits(jenc, JPEG_V420_REG_FE_CFG, JPEG_V420_FE_CFG_MAL_EN, 1);
	jpeg_rw_bits(jenc, JPEG_V420_REG_FE_CFG, JPEG_V420_FE_CFG_BOTTOM_VPAD_EN, 1);

	rval = jpeg_get_memory_fmt(sfmt->pixelformat);
	if (rval < 0) {
		dev_err(ectx->dev, "invalid memory format for v4l2 format: %p4cc\n",
			&sfmt->pixelformat);
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

	jpeg_rw_bits(jenc, JPEG_V420_REG_FE_CFG, JPEG_V420_FE_CFG_MEMORY_FORMAT_MSK, rval);

	jpeg_rw_bits(jenc, JPEG_V420_REG_FE_CFG, JPEG_V420_FE_CFG_PLNS_EN_MSK, 0);

	/* Scaling is not supported; always configure for 1:1 */
	jpeg_rw_bits(jenc, JPEG_V420_REG_FE_CFG, JPEG_V420_FE_CFG_SIXTEEN_MCU_EN, 1);
	jpeg_rw_bits(jenc, JPEG_V420_REG_FE_CFG, JPEG_V420_FE_CFG_MCUS_PER_BLOCK, 0);

	dev_dbg(ectx->dev, "sixteen MCU enabled=%d, %d MCU per blocks\n",
		jpeg_rd_bits(jenc, JPEG_V420_REG_FE_CFG, JPEG_V420_FE_CFG_SIXTEEN_MCU_EN),
		jpeg_rd_bits(jenc, JPEG_V420_REG_FE_CFG, JPEG_V420_FE_CFG_MCUS_PER_BLOCK));

	rval = jpeg_get_mal_boundary(sfmt->width, jpeg_mal_bounds, ARRAY_SIZE(jpeg_mal_bounds));
	if (rval < 0) {
		dev_err(ectx->dev, "failed to get FE mal boundary width=%u\n",
			sfmt->width);
		return -EINVAL;
	}
	jpeg_rw_bits(jenc, JPEG_V420_REG_FE_CFG, JPEG_V420_FE_CFG_MAL_BOUNDARY, rval);

	dev_dbg(ectx->dev, "optimal FE mal boundary=%d\n",
		jpeg_rd_bits(jenc, JPEG_V420_REG_FE_CFG, JPEG_V420_FE_CFG_MAL_BOUNDARY));

	rval = jpeg_get_encode_fmt(sfmt->pixelformat);
	if (rval < 0) {
		dev_err(ectx->dev, "unsupported encode format %p4cc\n",
			&sfmt->pixelformat);
		return -EINVAL;
	}

	switch (rval) {
	case JPEG_ENCODE_MONO:
	case JPEG_ENCODE_H1V1:
	case JPEG_ENCODE_H2V1:
		jpeg_rw_bits(jenc, JPEG_V420_REG_FE_VBPAD_CFG, JPEG_V420_FE_VBPAD_CFG_BLOCK_ROW_MSK,
			     DIV_ROUND_UP(sfmt->height, JPEG_MCU_BLOCK_8));
		break;
	case JPEG_ENCODE_H1V2:
	case JPEG_ENCODE_H2V2:
		jpeg_rw_bits(jenc, JPEG_V420_REG_FE_VBPAD_CFG, JPEG_V420_FE_VBPAD_CFG_BLOCK_ROW_MSK,
			     DIV_ROUND_UP(sfmt->height, JPEG_MCU_BLOCK_16));
		break;
	default:
		dev_err(ectx->dev, "unsupported encode format: %d\n", rval);
		return -EINVAL;
	}

	dev_dbg(ectx->dev, "FE vpad config=%d\n",
		jpeg_rd_bits(jenc, JPEG_V420_REG_FE_VBPAD_CFG,
			     JPEG_V420_FE_VBPAD_CFG_BLOCK_ROW_MSK));

	if (sfmt->pixelformat == V4L2_PIX_FMT_NV21M)
		jpeg_rw_bits(jenc, JPEG_V420_REG_FE_CFG, JPEG_V420_FE_CFG_CBCR_ORDER, 1);
	else
		jpeg_rw_bits(jenc, JPEG_V420_REG_FE_CFG, JPEG_V420_FE_CFG_CBCR_ORDER, 0);

	for (pln = 0; pln < sfmt->num_planes; pln++) {
		if (sfmt->width && sfmt->height) {
			switch (pln) {
			case 0:
				jpeg_rw_bits(jenc, JPEG_V420_REG_FE_CFG,
					     JPEG_V420_FE_CFG_PLN0_EN, 1);
				break;
			case 1:
				jpeg_rw_bits(jenc, JPEG_V420_REG_FE_CFG,
					     JPEG_V420_FE_CFG_PLN1_EN, 1);
				break;
			case 2:
				jpeg_rw_bits(jenc, JPEG_V420_REG_FE_CFG,
					     JPEG_V420_FE_CFG_PLN2_EN, 1);
				break;
			}
		}
	}

	jpeg_rw_bits(jenc, JPEG_V420_REG_CORE_CFG, JPEG_V420_CORE_CFG_FE_ENABLE, 1);

	return 0;
}

static int jpeg_setup_fe(struct jenc_context *ectx, struct qcom_jenc_queue *q)
{
	int rc;

	jpeg_setup_fe_size(ectx, q);
	jpeg_setup_fe_hinit(ectx, q);
	jpeg_setup_fe_vinit(ectx, q);

	rc = jpeg_setup_fe_params(ectx, q);
	if (rc)
		return rc;

	return 0;
}

/*
 * jpeg_header_init - build the JPEG header for a source format: one component
 * for GREY, otherwise YCbCr with 2x2 luma sampling.
 */
static int jpeg_header_init(struct qcom_jenc_header *hdr, u32 fourcc)
{
	bool mono = fourcc == V4L2_PIX_FMT_GREY;
	struct v4l2_jpeg_hdr_params params = {
		.num_components = mono ? JPEG_COMP_MONO : JPEG_COMP_COLOR,
		.luma_sampling = mono ? JPEG_SAMPLING_H1V1 : JPEG_SAMPLING_H2V2,
		.jfif = true,
	};

	return v4l2_jpeg_write_header(hdr->data, sizeof(hdr->data), &params, &hdr->layout);
}

static int jpeg_ensure_header_cache(struct jenc_context *ectx)
{
	struct qcom_jenc_queue *sq = &ectx->bufq[JENC_SRC_QUEUE];
	int rc;

	if (ectx->hdr_cache.layout.size)
		return 0;

	rc = jpeg_header_init(&ectx->hdr_cache, sq->vf.pixelformat);
	if (rc) {
		dev_err(ectx->dev, "JFIF header lazy init failed\n");
		return rc;
	}

	return 0;
}

static int jpeg_apply_we_addr(struct jenc_context *ectx, struct qcom_jenc_queue *q,
			      struct vb2_buffer *vb)
{
	struct qcom_jpeg_buff *frame = &q->buff[vb->index];
	struct qcom_jenc_dev *jenc = ectx->jenc;
	void *mptr = vb2_plane_vaddr(vb, 0);
	dma_addr_t dma = frame->plns[0].dma;
	size_t header_size;
	u8 pln = 0;
	int rc;

	if (WARN_ON_ONCE(!dma))
		return -EPERM;

	if (WARN_ON_ONCE(!mptr))
		return -EPERM;

	rc = jpeg_ensure_header_cache(ectx);
	if (rc)
		return rc;

	/* Update DQT cache if quality changed, then upload to HW DMI. */
	mutex_lock(&ectx->quality_mutex);
	if (!ectx->hdr_cache.layout.size || ectx->quality_programmed != ectx->quality_requested)
		jpeg_update_dqt_cache(ectx);
	jpeg_upload_dmi_table(ectx);
	mutex_unlock(&ectx->quality_mutex);

	header_size = ectx->hdr_cache.layout.size;
	if (vb->planes[0].length < header_size)
		return -EINVAL;

	memcpy(mptr, ectx->hdr_cache.data, header_size);
	v4l2_jpeg_hdr_set_size(mptr, &ectx->hdr_cache.layout, q->vf.width, q->vf.height);

	dma += header_size;

	/*
	 * Flush CPU writes to the header before handing the buffer to the
	 * hardware DMA engine.
	 */
	jpeg_sync_sg(jenc->dev, frame, DMA_BIDIRECTIONAL, true);

	jpeg_io_write(jenc, jpeg_v420_we_pntr[pln], dma);

	dev_dbg(jenc->dev, "WE pln=%d addr=%pad idx:%d\n",
		pln, &dma, vb->index);

	q->buff_id = vb->index;

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

static void jpeg_setup_we_size(struct jenc_context *ectx, struct qcom_jenc_queue *q)
{
	struct v4l2_pix_format_mplane *dfmt = &q->vf;
	struct qcom_jenc_dev *jenc = ectx->jenc;
	u8 pln;

	for (pln = 0; pln < QCOM_JPEG_MAX_PLANES; pln++)
		jpeg_rw_bits(jenc, jpeg_v420_we_stride[pln], JPEG_V420_PLN_WR_STRIDE_MSK, 0);

	jpeg_io_write(jenc, JPEG_V420_REG_WE_BSIZE_0, dfmt->plane_fmt[0].sizeimage);

	dev_dbg(ectx->dev, "WE size=%u\n", dfmt->plane_fmt[0].sizeimage);
}

static int jpeg_setup_we_hinit(struct jenc_context *ectx, struct qcom_jenc_queue *q)
{
	struct v4l2_pix_format_mplane *dfmt = &q->vf;
	struct qcom_jenc_dev *jenc = ectx->jenc;
	u8 pln;

	if (!dfmt->width) {
		dev_err(ectx->dev, "invalid destination width=%d\n", dfmt->width);
		return -EINVAL;
	}

	for (pln = 0; pln < QCOM_JPEG_MAX_PLANES; pln++) {
		jpeg_rw_bits(jenc, jpeg_v420_we_hinit[pln], JPEG_V420_PLN_WR_HINIT_INTEGER_MSK, 0);
		jpeg_rw_bits(jenc, jpeg_v420_we_hstep[pln], JPEG_V420_PLN_WR_HSTEP_INTEGER_MSK, 0);
	}

	jpeg_rw_bits(jenc, JPEG_V420_REG_WE_HSTEP_0,
		     JPEG_V420_PLN_WR_HSTEP_INTEGER_MSK, dfmt->width);

	dev_dbg(ectx->dev, "WE hstep=%u\n",
		jpeg_rd_bits(jenc, JPEG_V420_REG_WE_HSTEP_0, JPEG_V420_PLN_WR_HSTEP_INTEGER_MSK));

	return 0;
}

static int jpeg_setup_we_vinit(struct jenc_context *ectx, struct qcom_jenc_queue *q)
{
	struct v4l2_pix_format_mplane *dfmt = &q->vf;
	struct qcom_jenc_dev *jenc = ectx->jenc;
	u8 pln;

	if (!dfmt->height) {
		dev_err(ectx->dev, "invalid destination height=%d\n", dfmt->height);
		return -EINVAL;
	}

	for (pln = 0; pln < QCOM_JPEG_MAX_PLANES; pln++) {
		jpeg_rw_bits(jenc, jpeg_v420_we_vinit[pln], JPEG_V420_PLN_WR_VINIT_INTEGER_MSK, 0);
		jpeg_rw_bits(jenc, jpeg_v420_we_vstep[pln], JPEG_V420_PLN_WR_VSTEP_INTEGER_MSK, 0);
	}

	jpeg_rw_bits(jenc, JPEG_V420_REG_WE_VSTEP_0,
		     JPEG_V420_PLN_WR_VSTEP_INTEGER_MSK, dfmt->height);

	dev_dbg(ectx->dev, "WE vstep=%u\n",
		jpeg_rd_bits(jenc, JPEG_V420_REG_WE_VSTEP_0, JPEG_V420_PLN_WR_VSTEP_INTEGER_MSK));

	return 0;
}

static int jpeg_setup_we_params(struct jenc_context *ectx, struct qcom_jenc_queue *q)
{
	struct v4l2_pix_format_mplane *dfmt = &q->vf;
	struct qcom_jenc_dev *jenc = ectx->jenc;
	u32 blk_w = 0, blk_h = 0, mcu_cols = 0, mcu_rows = 0;
	int rval;

	rval = jpeg_get_memory_fmt(dfmt->pixelformat);
	if (rval < 0) {
		dev_err(ectx->dev, "invalid memory format for v4l2 format: %p4cc\n",
			&dfmt->pixelformat);
		return -EINVAL;
	}
	jpeg_rw_bits(jenc, JPEG_V420_REG_WE_CFG, JPEG_V420_WE_CFG_MEMORY_FORMAT_MSK, rval);

	rval = jpeg_get_mal_boundary(dfmt->width, jpeg_mal_bounds, ARRAY_SIZE(jpeg_mal_bounds));
	if (rval < 0) {
		dev_err(ectx->dev, "failed to get WE mal boundary width=%u\n",
			dfmt->width);
		return -EINVAL;
	}
	jpeg_rw_bits(jenc, JPEG_V420_REG_WE_CFG, JPEG_V420_WE_CFG_MAL_BOUNDARY, rval);

	dev_dbg(ectx->dev, "optimal WE mal boundary=%d\n",
		jpeg_rd_bits(jenc, JPEG_V420_REG_WE_CFG, JPEG_V420_WE_CFG_MAL_BOUNDARY));

	rval = jpeg_get_encode_fmt(dfmt->pixelformat);
	if (rval < 0) {
		dev_err(ectx->dev, "unsupported encode format %p4cc\n",
			&dfmt->pixelformat);
		return rval;
	}

	rval = jpeg_get_mcu_geometry(rval, dfmt->width, dfmt->height, &blk_w, &blk_h,
				     &mcu_cols, &mcu_rows);
	if (rval < 0) {
		dev_err(ectx->dev, "invalid MCU geometry mcu_cols=%d mcu_rows=%d\n",
			mcu_cols, mcu_rows);
		return rval;
	}

	dev_dbg(ectx->dev, "WE blk_w=%u blk_h=%u cols=%u rows=%u\n",
		blk_w, blk_h, mcu_cols, mcu_rows);

	jpeg_rw_bits(jenc, JPEG_V420_REG_WE_BLOCKS_0,
		     JPEG_V420_PLN_WR_BLK_CFG_BLOCKS_PER_ROW_MSK, mcu_rows - 1);
	jpeg_rw_bits(jenc, JPEG_V420_REG_WE_BLOCKS_0,
		     JPEG_V420_PLN_WR_BLK_CFG_BLOCKS_PER_COL_MSK, mcu_cols - 1);

	jpeg_rw_bits(jenc, JPEG_V420_REG_WE_CFG, JPEG_V420_WE_CFG_CBCR_ORDER, 1);
	jpeg_rw_bits(jenc, JPEG_V420_REG_WE_CFG, JPEG_V420_WE_CFG_MAL_EN, 1);
	jpeg_rw_bits(jenc, JPEG_V420_REG_WE_CFG, JPEG_V420_WE_CFG_POP_BUFF_ON_EOS, 1);
	jpeg_rw_bits(jenc, JPEG_V420_REG_WE_CFG, JPEG_V420_WE_CFG_PLN0_EN, 1);

	jpeg_rw_bits(jenc, JPEG_V420_REG_CORE_CFG, JPEG_V420_CORE_CFG_MODE, 1);
	jpeg_rw_bits(jenc, JPEG_V420_REG_CORE_CFG, JPEG_V420_CORE_CFG_WE_ENABLE, 1);

	return 0;
}

static int jpeg_setup_we(struct jenc_context *ectx, struct qcom_jenc_queue *q)
{
	int rc;

	jpeg_setup_we_size(ectx, q);

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
	struct qcom_jenc_queue *sq = &ectx->bufq[JENC_SRC_QUEUE];
	struct qcom_jenc_queue *dq = &ectx->bufq[JENC_DST_QUEUE];
	struct v4l2_pix_format_mplane *sfmt = &sq->vf;
	struct v4l2_pix_format_mplane *dfmt = &dq->vf;
	struct qcom_jenc_dev *jenc = ectx->jenc;
	u32 blk_w = 0, blk_h = 0, mcu_cols = 0, mcu_rows = 0;
	int rval;
	u8 pln;

	jpeg_rw_bits(jenc, JPEG_V420_REG_RESET_CMD, JPEG_V420_RST_CMD_SCALE_RESET, 1);

	/* explicit no scaling */
	jpeg_rw_bits(jenc, JPEG_V420_REG_SCALE_CFG, JPEG_V420_SCALE_CFG_HSCALE_ENABLE, 0);
	jpeg_rw_bits(jenc, JPEG_V420_REG_SCALE_CFG, JPEG_V420_SCALE_CFG_VSCALE_ENABLE, 0);

	for (pln = 0; pln < QCOM_JPEG_MAX_PLANES; pln++) {
		jpeg_io_write(jenc, jpeg_v420_scale_hstep[pln], JPEG_DEFAULT_SCALE_STEP);
		jpeg_io_write(jenc, jpeg_v420_scale_vstep[pln], JPEG_DEFAULT_SCALE_STEP);
	}

	rval = jpeg_get_encode_fmt(sfmt->pixelformat);
	if (rval < 0) {
		dev_err(ectx->dev, "unsupported encode format %p4cc\n",
			&sfmt->pixelformat);
		return -EINVAL;
	}

	rval = jpeg_get_mcu_geometry(rval, dfmt->width, dfmt->height, &blk_w, &blk_h,
				     &mcu_cols, &mcu_rows);
	if (rval < 0) {
		dev_err(ectx->dev, "invalid MCU geometry blk_w=%d blk_h=%d\n",
			blk_w, blk_h);
		return -EINVAL;
	}

	dev_dbg(ectx->dev, "scale blk_w=%u blk_h=%u cols=%u rows=%u\n",
		blk_w, blk_h, mcu_cols, mcu_rows);

	for (pln = 0; pln < sq->vf.num_planes; pln++) {
		jpeg_rw_bits(jenc, jpeg_v420_scale_out_cfg[pln],
			     JPEG_V420_SCALE_OUT_CFG_BLOCK_WIDTH_MSK, blk_w - 1);
		jpeg_rw_bits(jenc, jpeg_v420_scale_out_cfg[pln],
			     JPEG_V420_SCALE_OUT_CFG_BLOCK_HEIGHT_MSK, blk_h - 1);
	}

	dev_dbg(ectx->dev, "scale src=%ux%u dst=%ux%u enable=%d/%d\n",
		sfmt->width, sfmt->height, dfmt->width, dfmt->height,
		jpeg_rd_bits(jenc, JPEG_V420_REG_SCALE_CFG, JPEG_V420_SCALE_CFG_HSCALE_ENABLE),
		jpeg_rd_bits(jenc, JPEG_V420_REG_SCALE_CFG, JPEG_V420_SCALE_CFG_VSCALE_ENABLE));

	/* Disabled, but must be configured */
	jpeg_rw_bits(jenc, JPEG_V420_REG_CORE_CFG, JPEG_V420_CORE_CFG_SCALE_ENABLE, 0);

	return 0;
}

static int jpeg_setup_encode(struct jenc_context *ectx)
{
	struct qcom_jenc_queue *sq = &ectx->bufq[JENC_SRC_QUEUE];
	struct v4l2_pix_format_mplane *sfmt = &sq->vf;
	struct qcom_jenc_dev *jenc = ectx->jenc;
	u32 blk_w = 0, blk_h = 0, mcu_cols = 0, mcu_rows = 0;
	int rval;

	if (!sfmt->width || !sfmt->height)
		return -EINVAL;

	jpeg_rw_bits(jenc, JPEG_V420_REG_RESET_CMD, JPEG_V420_RST_CMD_ENCODER_RESET, 1);

	rval = jpeg_get_encode_fmt(sfmt->pixelformat);
	if (rval < 0) {
		dev_err(ectx->dev, "unsupported encode format %p4cc\n",
			&sfmt->pixelformat);
		return -EINVAL;
	}
	jpeg_rw_bits(jenc, JPEG_V420_REG_ENC_CFG, JPEG_V420_ENC_CFG_IMAGE_FORMAT_MSK, rval);

	rval = jpeg_get_mcu_geometry(rval, sfmt->width, sfmt->height, &blk_w, &blk_h,
				     NULL, &mcu_rows);
	if (rval < 0) {
		dev_err(ectx->dev, "invalid MCU geometry mcu_rows=%d\n", mcu_rows);
		return -EINVAL;
	}

	mcu_cols = sfmt->width / blk_w;

	dev_dbg(ectx->dev, "encode blk_w=%u blk_h=%u cols=%u rows=%u\n",
		blk_w, blk_h, mcu_cols, mcu_rows);

	jpeg_rw_bits(jenc, JPEG_V420_REG_ENC_IMG_SIZE,
		     JPEG_V420_ENC_IMG_SIZE_ENCODE_WIDTH_MSK, mcu_cols - 1);
	jpeg_rw_bits(jenc, JPEG_V420_REG_ENC_IMG_SIZE,
		     JPEG_V420_ENC_IMG_SIZE_ENCODE_HEIGHT_MSK, mcu_rows - 1);

	dev_dbg(ectx->dev, "encode width=%d height=%d\n",
		jpeg_rd_bits(jenc, JPEG_V420_REG_ENC_IMG_SIZE,
			     JPEG_V420_ENC_IMG_SIZE_ENCODE_WIDTH_MSK),
		jpeg_rd_bits(jenc, JPEG_V420_REG_ENC_IMG_SIZE,
			     JPEG_V420_ENC_IMG_SIZE_ENCODE_HEIGHT_MSK));

	jpeg_rw_bits(jenc, JPEG_V420_REG_ENC_CFG, JPEG_V420_ENC_CFG_APPLY_EOI, 1);
	jpeg_rw_bits(jenc, JPEG_V420_REG_CORE_CFG, JPEG_V420_CORE_CFG_ENC_ENABLE, 1);

	return 0;
}

irqreturn_t qcom_jenc_v420_irq_bot(int irq, void *data)
{
	struct qcom_jenc_dev *jenc = data;
	unsigned long flags;
	u32 irq_status;

	irq_status = READ_ONCE(jenc->pending_irq_status);

	if (FIELD_GET(JPEG_V420_IRQ_STATUS_SESSION_DONE, irq_status)) {
		struct jenc_context *ctx;

		spin_lock_irqsave(&jenc->hw_lock, flags);
		ctx = jenc->actx;
		jenc->actx = NULL;
		spin_unlock_irqrestore(&jenc->hw_lock, flags);

		if (ctx && ctx->bufq[JENC_DST_QUEUE].buff_id >= 0) {
			struct qcom_jenc_queue *dq = &ctx->bufq[JENC_DST_QUEUE];
			struct qcom_jpeg_buff *frame = &dq->buff[dq->buff_id];
			size_t out_size = jpeg_rd_bits(jenc, JPEG_V420_REG_ENC_OUT_SIZE,
						       JPEG_V420_STATUS_OUT_SIZE_BYTES_MSK);

			dev_dbg(jenc->dev, "complete idx:%d addr=%pad size=%zu\n",
				dq->buff_id, &frame->plns[0].dma, out_size);

			/*
			 * Stop the hardware before enc_hw_irq_cb(): it leads to
			 * v4l2_m2m_job_finish(), which can start the next job's device_run().
			 */
			jpeg_stop(jenc);
			jenc->enc_hw_irq_cb(ctx, VB2_BUF_STATE_DONE,
					    out_size + ctx->hdr_cache.layout.size);
		} else {
			jpeg_stop(jenc);
		}
	}

	if (FIELD_GET(JPEG_V420_IRQ_STATUS_SESION_ERROR_MSK, irq_status)) {
		struct jenc_context *ctx;

		spin_lock_irqsave(&jenc->hw_lock, flags);
		ctx = jenc->actx;
		jenc->actx = NULL;
		spin_unlock_irqrestore(&jenc->hw_lock, flags);

		dev_err(jenc->dev, "encoder hardware failure=0x%lx\n",
			FIELD_GET(JPEG_V420_IRQ_STATUS_SESION_ERROR_MSK, irq_status));

		jpeg_stop(jenc);
		if (ctx)
			jenc->enc_hw_irq_cb(ctx, VB2_BUF_STATE_ERROR, 0);
	}

	return IRQ_HANDLED;
}

irqreturn_t qcom_jenc_v420_irq_top(int irq, void *data)
{
	struct qcom_jenc_dev *jenc = data;
	unsigned long flags;
	u32 irq_status;

	spin_lock_irqsave(&jenc->hw_lock, flags);

	irq_status = jpeg_io_read(jenc, JPEG_V420_REG_INT_STATUS);
	jpeg_io_write(jenc, JPEG_V420_REG_INT_CLR, irq_status & JPEG_V420_IRQ_STATUS_ALL_BITS);

	if (FIELD_GET(JPEG_V420_IRQ_STATUS_RESET_ACK, irq_status))
		complete(&jenc->reset_complete);

	if (FIELD_GET(JPEG_V420_IRQ_STATUS_STOP_ACK, irq_status)) {
		complete(&jenc->stop_complete);
		dev_dbg(jenc->dev, "hardware stop acknowledged\n");
	}

	/* The ACKs are handled above; wake the threaded handler only for the rest */
	irq_status &= ~(JPEG_V420_IRQ_STATUS_RESET_ACK | JPEG_V420_IRQ_STATUS_STOP_ACK);
	if (!irq_status) {
		spin_unlock_irqrestore(&jenc->hw_lock, flags);
		return IRQ_HANDLED;
	}

	WRITE_ONCE(jenc->pending_irq_status, irq_status);

	spin_unlock_irqrestore(&jenc->hw_lock, flags);

	return IRQ_WAKE_THREAD;
}

void qcom_jenc_v420_get_hw_caps(struct qcom_jenc_dev *jenc, u32 *caps)
{
	u32 hw_caps;

	hw_caps = jpeg_io_read(jenc, JPEG_V420_REG_HW_CAPABILITY);
	dev_dbg(jenc->dev, "CAPS: encode=%lu decode=%lu upscale=%lu downscale=%lu\n",
		FIELD_GET(JPEG_V420_HW_CAP_ENCODE, hw_caps),
		FIELD_GET(JPEG_V420_HW_CAP_DECODE, hw_caps),
		FIELD_GET(JPEG_V420_HW_CAP_UPSCALE_MSK, hw_caps),
		FIELD_GET(JPEG_V420_HW_CAP_DOWNSCALE_MSK, hw_caps));

	*caps = hw_caps;
}

int qcom_jenc_v420_get_block_width(u32 fourcc, u32 *blk_w)
{
	int fmt = jpeg_get_encode_fmt(fourcc);

	if (fmt < 0)
		return fmt;

	return jpeg_get_mcu_geometry(fmt, 0, 0, blk_w, NULL, NULL, NULL);
}

int qcom_jenc_v420_get_block_height(u32 fourcc, u32 *blk_h)
{
	int fmt = jpeg_get_encode_fmt(fourcc);

	if (fmt < 0)
		return fmt;

	return jpeg_get_mcu_geometry(fmt, 0, 0, NULL, blk_h, NULL, NULL);
}

struct qcom_jenc_queue *qcom_jenc_v420_get_buff_queue(struct jenc_context *ectx,
						      enum qcom_enc_qid id)
{
	return &ectx->bufq[id];
}

int qcom_jenc_v420_queue_setup(struct jenc_context *ectx, enum qcom_enc_qid id)
{
	struct qcom_jenc_queue *q = &ectx->bufq[id];
	int rc;

	/*
	 * REQBUFS has freed the previous buffers: drop their cached sg_table
	 * and DMA addresses.
	 */
	memset(q->buff, 0, sizeof(q->buff));

	if (id == JENC_SRC_QUEUE) {
		rc = jpeg_header_init(&ectx->hdr_cache, q->vf.pixelformat);
		if (rc) {
			dev_err(ectx->dev, "JFIF header init failed\n");
			return rc;
		}
	}

	return 0;
}

int qcom_jenc_v420_src_fmt_update(struct jenc_context *ectx, u32 old_fourcc, u32 new_fourcc)
{
	bool old_is_mono = (old_fourcc == V4L2_PIX_FMT_GREY);
	bool new_is_mono = (new_fourcc == V4L2_PIX_FMT_GREY);
	int rc;

	/* header layout changes only for mono <-> color source format switch */
	if (old_is_mono == new_is_mono)
		return 0;

	rc = jpeg_header_init(&ectx->hdr_cache, new_fourcc);
	if (rc) {
		dev_err(ectx->dev, "JFIF header reinit failed\n");
		return rc;
	}

	/* force DQT upload after source profile switch */
	mutex_lock(&ectx->quality_mutex);
	ectx->quality_programmed = 0;
	mutex_unlock(&ectx->quality_mutex);

	return 0;
}

int qcom_jenc_v420_buffer_prepare(struct jenc_context *ectx, struct vb2_buffer *vb2)
{
	int rc;

	if (V4L2_TYPE_IS_OUTPUT(vb2->type)) {
		rc = jpeg_store_fe_next(ectx, vb2);
		if (rc)
			dev_err(ectx->dev, "cannot set up fetch engine address\n");
	} else {
		rc = jpeg_store_we_next(ectx, vb2);
		if (rc)
			dev_err(ectx->dev, "cannot set up write engine address\n");
	}

	return rc;
}

int qcom_jenc_v420_process_exec(struct qcom_jenc_dev *jenc, struct jenc_context *ectx,
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

		jpeg_apply_fe_addr(ectx, sq, vb);
	} else {
		rc = jpeg_setup_we(ectx, dq);
		if (rc)
			goto err_clear_ctx;

		rc = jpeg_apply_we_addr(ectx, dq, vb);
		if (rc)
			goto err_clear_ctx;
	}

	/* Both src and dst buffers queued for this frame - trigger encode. */
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
	/* HW_START was not issued: drop the addresses already queued for this job */
	jpeg_stop(jenc);

	spin_lock_irqsave(&jenc->hw_lock, flags);
	if (jenc->actx == ectx)
		jenc->actx = NULL;
	spin_unlock_irqrestore(&jenc->hw_lock, flags);

	return rc;
}

void qcom_jenc_v420_prepare(struct qcom_jenc_dev *jenc)
{
	jpeg_rw_bits(jenc, JPEG_V420_REG_RESET_CMD, JPEG_V420_RST_CMD_ENCODER_RESET, 1);
	jpeg_rw_bits(jenc, JPEG_V420_REG_RESET_CMD, JPEG_V420_RST_CMD_BLOCK_FORMATTER_RST, 1);
	jpeg_rw_bits(jenc, JPEG_V420_REG_RESET_CMD, JPEG_V420_RST_CMD_CORE_RESET, 1);
}

static unsigned long jpeg_calc_optimal_clk(struct qcom_jenc_dev *jenc,
					   u32 width, u32 height, u32 fps)
{
	unsigned long ref_pixel_rate;
	unsigned long optimal_clk;
	unsigned long pixel_rate;

	if (!width || !height || !fps)
		return jenc->max_freq;

	ref_pixel_rate = JPEG_REF_THROUGHPUT_MPPS * 1000000UL;
	pixel_rate = (unsigned long)width * height * fps;

	optimal_clk = DIV_ROUND_UP_ULL(pixel_rate * JPEG_REF_CLK_HZ, ref_pixel_rate);

	return min_t(unsigned long, jenc->max_freq, optimal_clk);
}

int qcom_jenc_v420_retune_clk(struct jenc_context *ectx)
{
	struct qcom_jenc_queue *sq = &ectx->bufq[JENC_SRC_QUEUE];
	struct qcom_jenc_dev *jenc = ectx->jenc;
	unsigned long freq;
	int rc;

	freq = jpeg_calc_optimal_clk(jenc, sq->vf.width, sq->vf.height, JPEG_REF_FPS);

	rc = qcom_jpeg_clk_retune(jenc, &freq);
	if (rc)
		return rc;

	jenc->opt_freq = freq;

	return 0;
}

int qcom_jenc_v420_acquire(struct jenc_context *ectx)
{
	struct qcom_jenc_queue *sq = &ectx->bufq[JENC_SRC_QUEUE];
	struct qcom_jenc_queue *dq = &ectx->bufq[JENC_DST_QUEUE];
	struct qcom_jenc_dev *jenc = ectx->jenc;
	int rc = 0;

	/* reset per-context stream state for each (re)acquire */
	sq->sequence = 0;
	dq->sequence = 0;
	dq->buff_id = -1;
	/*
	 * Recreate JPEG header lazily per destination buffer to tolerate
	 * different valid V4L2 call orders (e.g. STREAMON before first QBUF).
	 */
	ectx->hdr_cache.layout.size = 0;
	/* force DQT upload on first frame after (re)acquire */
	mutex_lock(&ectx->quality_mutex);
	ectx->quality_programmed = 0;
	mutex_unlock(&ectx->quality_mutex);

	mutex_lock(&jenc->hw_ref_lock);

	if (!jenc->ref_count) {
		jenc->opt_freq = jpeg_calc_optimal_clk(jenc, sq->vf.width, sq->vf.height,
						       JPEG_REF_FPS);

		dev_dbg(jenc->dev, "freq=%lu src=%ux%u\n", jenc->opt_freq,
			sq->vf.width, sq->vf.height);

		rc = pm_runtime_resume_and_get(jenc->dev);
		if (rc < 0) {
			dev_err(jenc->dev, "PM runtime get failed\n");
			goto out_unlock;
		}

		enable_irq(jenc->irq);

		rc = jpeg_init(jenc);
		if (rc) {
			dev_err(jenc->dev, "hardware init failed\n");
			disable_irq(jenc->irq);
			pm_runtime_put_sync(jenc->dev);
			goto out_unlock;
		}
	}

	jenc->ref_count++;

out_unlock:
	mutex_unlock(&jenc->hw_ref_lock);

	return rc;
}

int qcom_jenc_v420_release(struct jenc_context *ectx)
{
	struct qcom_jenc_dev *jenc = ectx->jenc;
	unsigned long flags;
	int rc = 0;
	int pm_rc;

	mutex_lock(&jenc->hw_ref_lock);

	if (WARN_ON_ONCE(!jenc->ref_count))
		goto out_unlock;

	jenc->ref_count--;
	if (!jenc->ref_count) {
		rc = jpeg_deinit(jenc);
		if (rc)
			dev_err(jenc->dev, "hardware exit failed\n");

		/*
		 * A job aborted by HW_STOP raises no SESSION_DONE: drop its
		 * active context reference here.
		 */
		spin_lock_irqsave(&jenc->hw_lock, flags);
		if (jenc->actx == ectx)
			jenc->actx = NULL;
		spin_unlock_irqrestore(&jenc->hw_lock, flags);

		disable_irq(jenc->irq);

		pm_rc = pm_runtime_put_sync(jenc->dev);
		if (pm_rc < 0) {
			dev_err(jenc->dev, "PM runtime put failed\n");
			if (!rc)
				rc = pm_rc;
		}

		dev_dbg(jenc->dev, "JPEG HW encoder released\n");
	}

out_unlock:
	mutex_unlock(&jenc->hw_ref_lock);

	return rc;
}
