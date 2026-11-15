/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __ROCKCHIP_IEP2_H__
#define __ROCKCHIP_IEP2_H__

#include <linux/clk.h>
#include <linux/mutex.h>
#include <linux/reset.h>
#include <linux/sizes.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-v4l2.h>

#define IEP_NAME		"rockchip-iep2"

#define IEP_TILE_W		16
#define IEP_TILE_H		4

#define IEP_MIN_WIDTH		32U
#define IEP_MAX_WIDTH		1920U
#define IEP_MIN_HEIGHT		16U
#define IEP_MAX_HEIGHT		1088U

#define IEP_MAX_STRIDE		(0xffff * 4)
#define IEP_STRIDE_ALIGN	16

/* The IEP2 reads past the end of the OUTPUT buffer */
#define IEP_SRC_TAIL		SZ_4K

enum iep_deint_mode {
	IEP_DEINT_BOB_FRAME,
	IEP_DEINT_BOB_FIELD,
	IEP_DEINT_MA_FRAME,
	IEP_DEINT_MA_FIELD,
};

#define IEP_MD_BUF_SIZE		(IEP_MAX_WIDTH * IEP_MAX_HEIGHT)
#define IEP_MV_BUF_SIZE		(120 * 480)
#define IEP_ROI_BUF_SIZE	(120 * 272)

struct iep_fmt {
	u32 fourcc;
	u8 hw_fmt;
	u8 hw_swap;
	u8 vdiv;
};

struct iep_dev {
	struct v4l2_device v4l2_dev;
	struct v4l2_m2m_dev *m2m_dev;
	struct video_device vfd;
	struct device *dev;
	/* serialises ioctls and vb2 queue operations */
	struct mutex mutex;

	void __iomem *regs;
	struct clk_bulk_data *clks;
	int num_clks;
	struct reset_control *rstc;
	struct iommu_domain *empty_domain;

	void *roi_cpu;
	dma_addr_t roi_dma;

	struct delayed_work watchdog_work;
	/* protects busy and irq_status */
	spinlock_t irq_lock;
	bool busy;
	u32 irq_status;
	unsigned int foreign_irqs;
};

struct iep_ctx {
	struct v4l2_fh fh;
	struct iep_dev *iep;
	struct v4l2_ctrl_handler ctrl_handler;

	const struct iep_fmt *fmt;
	struct v4l2_pix_format_mplane src_fmt;
	struct v4l2_pix_format_mplane dst_fmt;

	bool motion_adaptive;
	bool field_rate;

	void *md_cpu;
	dma_addr_t md_dma;
	void *mv_cpu;
	dma_addr_t mv_dma;

	struct vb2_v4l2_buffer *prev;

	struct vb2_v4l2_buffer *job_src;
	struct vb2_v4l2_buffer *job_dst[2];
	unsigned int job_ndst;
	bool job_keep_src;

	u32 out_sequence;
	u32 cap_sequence;
};

#endif /* __ROCKCHIP_IEP2_H__ */
