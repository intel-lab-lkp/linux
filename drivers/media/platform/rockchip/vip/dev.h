/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Rockchip VIP Driver
 *
 * Copyright (C) 2018 Rockchip Electronics Co., Ltd.
 * Copyright (C) 2023 Mehdi Djait <mehdi.djait@bootlin.com>
 */

#ifndef _RK_VIP_DEV_H
#define _RK_VIP_DEV_H

#include <linux/clk.h>
#include <linux/mutex.h>
#include <media/media-device.h>
#include <media/media-entity.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/videobuf2-v4l2.h>

#define VIP_DRIVER_NAME		"rk_vip"
#define VIP_VIDEODEVICE_NAME	"stream_vip"

#define RK_VIP_MAX_BUS_CLK	8
#define RK_VIP_MAX_SENSOR	2
#define RK_VIP_MAX_RESET	5
#define RK_VIP_MAX_CSI_CHANNEL	4

#define RK_VIP_DEFAULT_WIDTH	640
#define RK_VIP_DEFAULT_HEIGHT	480

#define write_vip_reg(base, addr, val)  writel(val, (addr) + (base))
#define read_vip_reg(base, addr) readl((addr) + (base))

enum rk_vip_state {
	RK_VIP_STATE_DISABLED,
	RK_VIP_STATE_READY,
	RK_VIP_STATE_STREAMING
};

enum rk_vip_chip_id {
	CHIP_PX30_VIP,
	CHIP_RK1808_VIP,
	CHIP_RK3128_VIP,
	CHIP_RK3288_VIP
};

enum host_type_t {
	RK_CSI_RXHOST,
	RK_DSI_RXHOST
};

struct rk_vip_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head queue;
	union {
		u32 buff_addr[VIDEO_MAX_PLANES];
		void *vaddr[VIDEO_MAX_PLANES];
	};
};

struct rk_vip_scratch_buffer {
	void *vaddr;
	dma_addr_t dma_addr;
	u32 size;
};

static inline struct rk_vip_buffer *to_rk_vip_buffer(struct vb2_v4l2_buffer *vb)
{
	return container_of(vb, struct rk_vip_buffer, vb);
}

struct rk_vip_sensor_info {
	struct v4l2_subdev *sd;
	int pad;
	struct v4l2_mbus_config mbus;
	int lanes;
	v4l2_std_id std;
};

struct vip_output_fmt {
	u32 fourcc;
	u32 mbus;
	u32 fmt_val;
	u8 cplanes;
};

enum vip_fmt_type {
	VIP_FMT_TYPE_YUV = 0,
	VIP_FMT_TYPE_RAW,
};

struct vip_input_fmt {
	u32 mbus_code;
	u32 dvp_fmt_val;
	u32 csi_fmt_val;
	enum vip_fmt_type fmt_type;
	enum v4l2_field field;
};

struct rk_vip_stream {
	struct rk_vip_device		*vipdev;
	enum rk_vip_state		state;
	bool				stopping;
	wait_queue_head_t		wq_stopped;
	int				frame_idx;
	int				frame_phase;

	/* lock between irq and buf_queue */
	spinlock_t			vbq_lock;
	struct vb2_queue		buf_queue;
	struct list_head		buf_head;
	struct rk_vip_scratch_buffer	scratch_buf;
	struct rk_vip_buffer		*buffs[2];

	/* vfd lock */
	struct mutex			vlock;
	struct video_device		vdev;
	struct media_pad		pad;

	struct vip_output_fmt		*vip_fmt_out;
	const struct vip_input_fmt	*vip_fmt_in;
	struct v4l2_pix_format_mplane	pixm;
};

static inline struct rk_vip_stream *to_rk_vip_stream(struct video_device *vdev)
{
	return container_of(vdev, struct rk_vip_stream, vdev);
}

struct rk_vip_device {
	struct list_head		list;
	struct device			*dev;
	int				irq;
	void __iomem			*base_addr;
	void __iomem			*csi_base;
	struct clk_bulk_data		clks[RK_VIP_MAX_BUS_CLK];
	int				num_clk;
	struct vb2_alloc_ctx		*alloc_ctx;
	bool				iommu_en;
	struct iommu_domain		*domain;
	struct reset_control		*vip_rst;

	struct v4l2_device		v4l2_dev;
	struct media_device		media_dev;
	struct v4l2_ctrl_handler	ctrl_handler;
	struct v4l2_async_notifier	notifier;
	struct v4l2_async_connection	asd;
	struct rk_vip_sensor_info	sensor;

	struct rk_vip_stream		stream;

	int				chip_id;
};

void rk_vip_unregister_stream_vdev(struct rk_vip_device *dev);
int rk_vip_register_stream_vdev(struct rk_vip_device *dev);
void rk_vip_stream_init(struct rk_vip_device *dev);
void rk_vip_set_default_format(struct rk_vip_device *dev);

irqreturn_t rk_vip_irq_pingpong(int irq, void *ctx);
void rk_vip_soft_reset(struct rk_vip_device *vip_dev);

#endif
