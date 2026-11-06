/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef QCOM_JENC_DEV_H
#define QCOM_JENC_DEV_H

#include <linux/device.h>
#include <linux/mutex.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-jpeg.h>
#include <media/videobuf2-core.h>

#define QCOM_JPEG_ENC_NAME "qcom-jpeg-enc"

#define JPEG_HEADER_MAX	1024

/* JPEG header built once per source format, copied into every output buffer */
struct qcom_jenc_header {
	u8				data[JPEG_HEADER_MAX];
	struct v4l2_jpeg_hdr_layout	layout;
};

#define QCOM_JPEG_MAX_PLANES	3

#define QCOM_JPEG_QUALITY_MIN	1
#define QCOM_JPEG_QUALITY_DEF	98
#define QCOM_JPEG_QUALITY_MAX	100
#define QCOM_JPEG_QUALITY_UNT	1

#define TYPE2QID(t) \
	(V4L2_TYPE_IS_OUTPUT(t) ? JENC_SRC_QUEUE : JENC_DST_QUEUE)

enum qcom_enc_qid {
	JENC_SRC_QUEUE = 0,
	JENC_DST_QUEUE,
	JENC_QUEUE_MAX
};

struct jenc_enc_format {
	u32 type;
	u32 fourcc;
};

struct qcom_jpeg_buff {
	struct {
		struct sg_table		*sgt;
		dma_addr_t		dma;
		unsigned long		size;

	} plns[QCOM_JPEG_MAX_PLANES];
};

struct qcom_jenc_queue {
	struct v4l2_pix_format_mplane	vf;
	u32				sequence;
	struct qcom_jpeg_buff		buff[VB2_MAX_FRAME];
	int				buff_id;
};

struct qcom_jenc_dev {
	struct device			*dev;
	struct v4l2_device		v4l2_dev;
	struct v4l2_m2m_dev		*m2m_dev;
	struct video_device		*vdev;
	int				irq;
	void __iomem			*jpeg_base;
	struct clk_bulk_data		*clks;
	int				num_clks;
	struct clk			*camnoc_axi_src;
	unsigned long			camnoc_axi_min_rate;
	struct dev_pm_domain_list	*pd_list;
	struct dev_pm_domain_list	*opp_pd_list;
	unsigned long			max_freq;
	unsigned long			opt_freq;
	/* device mutex lock */
	struct mutex			dev_mutex;
	/* hardware reference count lock */
	struct mutex			hw_ref_lock;
	unsigned int			ref_count;
	struct completion		reset_complete;
	struct completion		stop_complete;
	/* encoder hardware lock */
	spinlock_t			hw_lock;
	struct jenc_context		*actx;

	u32				pending_irq_status;

	void (*enc_hw_irq_cb)
		(void *data, enum vb2_buffer_state ev, size_t out_size);
};

int qcom_jpeg_clk_retune(struct qcom_jenc_dev *jenc, unsigned long *freq);

struct jenc_context {
	struct device		 *dev;
	struct qcom_jenc_dev	 *jenc;
	struct v4l2_fh		 fh;

	/* quality update lock */
	struct mutex		 quality_mutex;
	struct v4l2_ctrl	 *quality_ctl;
	u32			 quality_requested;
	u32			 quality_programmed;
	struct v4l2_ctrl_handler ctrl_hdl;

	/* session context lock */
	struct mutex		 ctx_lock;

	bool			 hw_acquired;

	struct qcom_jenc_queue	bufq[JENC_QUEUE_MAX];
	struct qcom_jenc_header	hdr_cache;

	struct work_struct finish_work;
};

#endif
