/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/* Toshiba Visconti Video Capture Support
 *
 * (C) Copyright 2023 TOSHIBA CORPORATION
 * (C) Copyright 2023 Toshiba Electronic Devices & Storage Corporation
 */

#ifndef VIIF_H
#define VIIF_H

#include <linux/visconti_viif.h>
#include <linux/workqueue.h>
#include <media/v4l2-async.h>
#include <media/v4l2-common.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mediabus.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#define VIIF_DRIVER_NAME   "visconti-viif"
#define VIIF_BUS_INFO_BASE "platform:" VIIF_DRIVER_NAME

#define VIIF_ISP_REGBUF_0 0
#define VIIF_L2ISP_POST_0 0
#define VIIF_L2ISP_POST_1 1
#define VIIF_MAX_POST_NUM 2U

#define VIIF_CAPTURE_PAD_SINK  0
#define VIIF_ISP_PAD_SINK      0
#define VIIF_ISP_PAD_SRC_PATH0 1
#define VIIF_ISP_PAD_SRC_PATH1 2
#define VIIF_ISP_PAD_SRC_PATH2 3
#define VIIF_ISP_PAD_NUM       4

#define VIIF_CSI2RX_PAD_SINK 0
#define VIIF_CSI2RX_PAD_SRC  1
#define VIIF_CSI2RX_PAD_NUM  2

#define CAPTURE_PATH_MAIN_POST0 0
#define CAPTURE_PATH_MAIN_POST1 1
#define CAPTURE_PATH_SUB	2

#define VIIF_DPC_TABLE_BYTES	   8192
#define VIIF_LSC_TABLE_BYTES	   1536
#define VIIF_UNDIST_TABLE_BYTES	   8192
#define VIIF_L2_GAMMA_TABLE_BYTES  512
#define VIIF_L2_GAMMA_TABLE_CH_NUM 6

#define VIIF_HW_AVAILABLE_IRQS 4

#define VIIF_SYS_CLK 500000UL

enum viif_output_color_mode {
	VIIF_COLOR_Y_G = 0,
	VIIF_COLOR_U_B = 1U,
	VIIF_COLOR_V_R = 2U,
	VIIF_COLOR_YUV_RGB = 4U
};

/**
 * struct viif_img_area - image area definition
 * @x: x position. Range: [0..8062]
 * @y: y position. Range: [0..3966]
 * @w: image width. Range: [128..8190]
 * @h: image height. Range: [128..4094]
 */
struct viif_img_area {
	u32 x;
	u32 y;
	u32 w;
	u32 h;
};

/**
 * struct viif_out_process - configuration of output process of MAIN unit and L2ISP
 * @half_scale: true to enable half scaling
 * @select_color: viif_output_color_mode "select output color"
 * @alpha: alpha value used in case of ARGB8888 output. Range: [0..255]
 */
struct viif_out_process {
	bool half_scale;
	enum viif_output_color_mode select_color;
	u8 alpha;
};

/**
 * struct viif_fmt - description of supported output image format
 * @fourcc: V4L2 fourcc format ID
 * @bpp: bits per pixel for each plane
 * @num_planes: number of planes in a image
 * @colorspace: colorspace ID
 * @pitch_align: alignment constraint of pitch
 */
struct viif_fmt {
	u32 fourcc;
	u8 bpp[3];
	u8 num_planes;
	u32 colorspace;
	u32 pitch_align;
};

/**
 * struct viif_sensor_async -  sensor information handled by v4l2_async APIs
 * @asc:      async_connection for the sensor
 * @v4l2_sd:  v4l2_subdev for the sensor
 * @num_lane: number of lanes provided by the sensor
 * @index:    index of the sensor
 */
struct viif_sensor_async {
	struct v4l2_async_connection asc;
	struct v4l2_subdev *v4l2_sd;
	unsigned int num_lane;
	unsigned int index;
};

/*
 * struct viif_table_area - table for ISP features.
 *
 * The memory block for this structure must be allocated with dma_alloc_wc()
 * so that the allocated block will be phisically continuous.
 */
struct viif_table_area {
	/* L1ISP DPC */
	u32 dpc_table_h[VIIF_DPC_TABLE_BYTES / sizeof(u32)];
	u32 dpc_table_m[VIIF_DPC_TABLE_BYTES / sizeof(u32)];
	u32 dpc_table_l[VIIF_DPC_TABLE_BYTES / sizeof(u32)];
	/* L1ISP LSC */
	u16 lsc_table_gr[VIIF_LSC_TABLE_BYTES / sizeof(u16)];
	u16 lsc_table_r[VIIF_LSC_TABLE_BYTES / sizeof(u16)];
	u16 lsc_table_b[VIIF_LSC_TABLE_BYTES / sizeof(u16)];
	u16 lsc_table_gb[VIIF_LSC_TABLE_BYTES / sizeof(u16)];
	/* L2ISP UNDIST */
	u32 undist_write_g[VIIF_UNDIST_TABLE_BYTES / sizeof(u32)];
	u32 undist_read_b[VIIF_UNDIST_TABLE_BYTES / sizeof(u32)];
	u32 undist_read_g[VIIF_UNDIST_TABLE_BYTES / sizeof(u32)];
	u32 undist_read_r[VIIF_UNDIST_TABLE_BYTES / sizeof(u32)];
	/* L2ISP GAMMA */
	u16 l2_gamma_table[VIIF_MAX_POST_NUM][VIIF_L2_GAMMA_TABLE_CH_NUM]
			  [VIIF_L2_GAMMA_TABLE_BYTES / sizeof(u16)];
};

/**
 * struct cap_dev - device node for capture device
 * @pathid: 0 for MAIN POST0, 1 for MAIN POST1, 2 for SUB
 * @vdev: video node
 * @capture_pad: media pad
 * @ctrl_handler: v4l2 control handler
 * @vlock: serialize ioctl to vb2_queue and video_device
 * @vb2_vq: queue of buffers
 * @buf_queue: list of available buffers
 * @active: VDMAC will start writing to this bufffer at the next VSYNC
 * @dma_active: VDMAC will complete writing to this buffer at the next VSYNC
 * @buf_cnt: number of queued buffers
 * @sequence: total count of processed frames
 * @buf_lock: serialize queue access (including ISR's)
 * @v4l2_pix: current picture format (set by S_FMT)
 * @out_format: output format for VDMAC
 * @img_area: crop of output picture
 * @out_process: output configuration
 * @fmts: format supported by this capture device
 * @fmt_size: sizeof fmts
 * @viif_dev: reference to viif device
 */
struct cap_dev {
	u32 pathid;
	struct video_device vdev;
	struct media_pad capture_pad;
	struct v4l2_ctrl_handler ctrl_handler;
	struct mutex vlock; /*serialize ioctl to vb2_queue and video_device*/

	/* vb2 queue, capture buffer list and active buffer pointer */
	struct vb2_queue vb2_vq;
	struct list_head buf_queue;
	struct vb2_v4l2_buffer *active;
	struct vb2_v4l2_buffer *dma_active;
	int buf_cnt;
	unsigned int sequence;
	spinlock_t buf_lock; /* serialize queue access (including ISR's) */

	/* current configuration of frame and pixel format */
	struct v4l2_pix_format_mplane v4l2_pix;
	unsigned int out_format;
	struct viif_img_area img_area;
	struct viif_out_process out_process;

	/* format supported by this cap device */
	const struct viif_fmt *fmts;
	int fmt_size;

	struct viif_device *viif_dev;
};

/**
 * struct isp_subdev - device node for ISP suddevice
 * @sd: v4l2 subdevice
 * @pads: media pad
 * @pad_cfg: configuration of media pad
 * @ops_lock: serialize V4L2 query
 * @viif_dev: reference to viif device
 * @ctrl_handler: v4l2 control handler
 * @ctrl_rawpack_mode: control
 * @ctrl_input_mode: control
 * @ctrl_last_capture_status: control
 */
struct isp_subdev {
	struct v4l2_subdev sd;
	struct media_pad pads[VIIF_ISP_PAD_NUM];
	struct v4l2_subdev_pad_config pad_cfg[VIIF_ISP_PAD_NUM];
	struct mutex ops_lock; /* serialize V4L2 query */
	struct viif_device *viif_dev;
	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *ctrl_rawpack_mode;
	struct v4l2_ctrl *ctrl_input_mode;
	struct v4l2_ctrl *ctrl_last_capture_status;
};

/**
 * struct csi2rx_subdev - device node for CSI2RX suddevice
 * @sd: v4l2 subdevice
 * @pads: media pad
 * @pad_cfg: configuration of media pad
 * @ops_lock: serialize V4L2 query
 * @viif_dev: reference to viif device
 */
struct csi2rx_subdev {
	struct v4l2_subdev sd;
	struct media_pad pads[VIIF_CSI2RX_PAD_NUM];
	struct v4l2_subdev_pad_config pad_cfg[VIIF_CSI2RX_PAD_NUM];
	struct mutex ops_lock; /* serialize V4L2 query */
	struct viif_device *viif_dev;
};

/**
 * struct viif_l2_roi_path_info - crop information of main paths
 * @roi_num:
 *
 * - 1: crops of MAIN POST0 and POST1 share the same ROI
 * - 2: crops of MAIN POST0 and POST1 have independent ROIs
 *
 * @post_enable_flag: flag to enable corresponding main path
 * @post_crop_x: left of crop rect for a POST
 * @post_crop_y: top of crop rect for a POST
 * @post_crop_w: width of crop rect for a POST
 * @post_crop_h: height of crop rect for a POST
 */
struct viif_l2_roi_path_info {
	u32 roi_num;
	bool post_enable_flag[VIIF_MAX_POST_NUM];
	u32 post_crop_x[VIIF_MAX_POST_NUM];
	u32 post_crop_y[VIIF_MAX_POST_NUM];
	u32 post_crop_w[VIIF_MAX_POST_NUM];
	u32 post_crop_h[VIIF_MAX_POST_NUM];
};

/**
 * struct viif_img_clk - relation between realtime duration and number of lines
 * @pixel_clock: picture transfer clock frequency
 * @htotal_size: width of picture including blanking period
 *
 * These values are used to convert realtime duration (such as HW specific setup time)
 * into number of lines in a picture.
 * See sysclk_to_numlines() called at the reconfiguration of L1ISP HDRC feature.
 */
struct viif_img_clk {
	unsigned int pixel_clock;
	unsigned int htotal_size;
};

/**
 * struct viif_device - driver information of Visconti VIIF
 * @dev: device
 * @v4l2_dev: v4l2 device
 * @media_dev: media device
 * @pipe: media pipeline
 * @masked_gamma_path: flag to ignore L2_GAMMA error just after capture error
 * @subdevs: sensor subdevice specified in device tree
 * @asds: async subdevices for subdevs
 * @notifier: async subdev notification helper
 * @sd: points current image sensor subdevice
 * @cap_dev0: capture device for MAIN PATH 0
 * @cap_dev1: capture device for MAIN PATH 1
 * @cap_dev2: capture device for MAIN PATH 2
 * @isp_subdev: ISP subdevice
 * @csi2rx_subdev: CSI2RX subdevice
 * @sensor_sd: currently active sensor subdevice
 * @sensor_num_lane: number of lanes for currently active sensor
 * @stream_lock: serialize stream ON/OFF sequence
 * @regbuf_lock: Serialize VIIF Register Buffer Access
 * @l2_roi_path_info: crop information of main paths
 * @img_clk: relation between realtime duration and number of lines
 * @run_flag_main: flag to check if the stream is ON
 * @capture_reg: HW capture registers
 * @csi2host_reg: HW CSI2RX registers
 * @hwaif_reg: HW bus interface registers
 * @mpu_reg: HW memory protection unit registers
 * @irq: IRQ number
 * @tables: table for ISP features (virtual address)
 * @tables_dma: table for ISP features (IOVA)
 * @rawpack_mode: rawpack mode
 * @wq: workqueue queue
 * @work: workqueue task to update V4L2_CID_VISCONTI_VIIF_GET_LAST_CAPTURE_STATUS
 * @status_err: error of Main path in a frame
 * @reported_err_main: accumerated error flags for MAIN path
 * @reported_err_sub: accumerated error flags for SUB path
 * @reported_err_csi2rx: accumerated error flags for CSI2RX
 */
struct viif_device {
	struct device *dev;
	struct v4l2_device v4l2_dev;
	struct media_device media_dev;
	struct media_pipeline pipe;
	u32 masked_gamma_path;

	struct v4l2_async_notifier notifier;

	struct cap_dev cap_dev0;
	struct cap_dev cap_dev1;
	struct cap_dev cap_dev2;
	struct isp_subdev isp_subdev;
	struct csi2rx_subdev csi2rx_subdev;
	struct v4l2_subdev *sensor_sd;
	unsigned int sensor_num_lane;

	/* stream_lock - Serialize stream ON/OFF sequence */
	struct mutex stream_lock;

	/* regbuf_lock - Serialize VIIF Register Buffer Access */
	spinlock_t regbuf_lock;

	struct viif_l2_roi_path_info l2_roi_path_info;
	struct viif_img_clk img_clk;
	bool run_flag_main;

	void __iomem *capture_reg;
	void __iomem *csi2host_reg;
	void __iomem *hwaif_reg;
	void __iomem *mpu_reg;
	unsigned int irq[VIIF_HW_AVAILABLE_IRQS];

	/* Un-cache table area */
	struct viif_table_area *tables;
	struct viif_table_area *tables_dma;

	/* Rawpack mode */
	u32 rawpack_mode;

	/* work queue to save L1 status of the latest frame */
	struct workqueue_struct *wq;
	struct work_struct work;

	/* Error flag checked at delayed vsync handler  */
	u32 status_err;

	/* Error flag checked at compound control GET_REPORTED_ERRORS  */
	u32 reported_err_main;
	u32 reported_err_sub;
	u32 reported_err_csi2rx;
};

static inline void viif_capture_write(struct viif_device *viif_dev, unsigned int regid, u32 val)
{
	writel(val, viif_dev->capture_reg + regid);
}

static inline u32 viif_capture_read(struct viif_device *viif_dev, unsigned int regid)
{
	return readl(viif_dev->capture_reg + regid);
}

#endif /* VIIF_H */
