/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef HWS_PCIE_H
#define HWS_PCIE_H

#include <linux/types.h>
#include <linux/dma-mapping.h>
#include <linux/kthread.h>
#include <linux/pci.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/sizes.h>

#include <sound/pcm.h>
#include <sound/core.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/videobuf2-dma-sg.h>

#include "hws_reg.h"

struct snd_pcm_substream;

struct hwsmem_param {
	u32 index;
	u32 type;
	u32 status;
};

struct hws_pix_state {
	u32 width;
	u32 height;
	u32 fourcc;		/* V4L2_PIX_FMT_* (YUYV only here) */
	u32 bytesperline;	/* stride */
	u32 sizeimage;		/* full frame */
	enum v4l2_field field;	/* V4L2_FIELD_NONE or INTERLACED */
	enum v4l2_colorspace colorspace;	/* e.g., REC709 */
	enum v4l2_ycbcr_encoding ycbcr_enc;	/* V4L2_YCBCR_ENC_DEFAULT */
	enum v4l2_quantization quantization;	/* V4L2_QUANTIZATION_LIM_RANGE */
	enum v4l2_xfer_func xfer_func;	/* V4L2_XFER_FUNC_DEFAULT */
	bool interlaced;	/* cached hardware state */
	u32 half_size;		/* optional: if your HW needs it */
};

#define	UNSET	(-1U)

struct hws_pcie_dev;
struct hws_adapter;

struct hwsvideo_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
	int slot;		/* for two-buffer approach */
};

struct hws_video {
	/* ───── linkage ───── */
	struct hws_pcie_dev *parent;	/* parent device */
	struct video_device *video_device;

	struct vb2_queue buffer_queue;
	struct list_head capture_queue;
	struct hwsvideo_buffer *active;

	/* ───── locking ───── */
	struct mutex state_lock;	/* primary state */
	spinlock_t irq_lock;	/* ISR-side */
	struct mutex qlock;

	/* ───── indices ───── */
	int channel_index;

	/* ───── async helpers ───── */
	struct tasklet_struct bh_tasklet;

	/* ───── colour controls ───── */
	int current_brightness;
	int current_contrast;
	int current_saturation;
	int current_hue;

	/* ───── V4L2 controls ───── */
	struct v4l2_ctrl_handler control_handler;
	struct v4l2_ctrl *hotplug_detect_control;
	struct v4l2_ctrl *ctrl_brightness;
	struct v4l2_ctrl *ctrl_contrast;
	struct v4l2_ctrl *ctrl_saturation;
	struct v4l2_ctrl *ctrl_hue;
	/* ───── capture queue status ───── */
	struct hws_pix_state pix;
	u32 alloc_sizeimage;

	/* ───── per-channel capture state ───── */
	bool cap_active;
	bool stop_requested;
	u8 last_buf_half_toggle;
	bool half_seen;
	u32 sequence_number;

	/* ───── timeout and error handling ───── */
	u32 timeout_count;
	u32 error_count;

	/* ───── two-buffer approach ───── */
	dma_addr_t ring_dma;
	void *ring_cpu;
	size_t ring_size;
	u32 ring_toggle_prev;
	u32 ring_toggle_hw;
	bool ring_first_half_copied;
	unsigned long ring_last_toggle_jiffies;
	u32 queued_count;

	/* ───── misc counters ───── */
	int signal_loss_cnt;
};

struct hws_audio {
	/* linkage */
	struct hws_pcie_dev *parent;
	int channel_index;

	/* ALSA */
	struct snd_pcm_substream *pcm_substream;
	/* ring geometry (set in prepare/hw_params) */
	u32 periods;
	u32 period_bytes;
	u32 next_period;

	/* stream state */
	bool cap_active;
	bool stream_running;
	bool stop_requested;

	/* minimal HW period tracking  */
	u8 last_period_toggle;
	snd_pcm_uframes_t ring_wpos_byframes;
	/* PCM format (for HW programming) */
	u32 output_sample_rate;
	u16 channel_count;
	u16 bits_per_sample;
};

struct hws_scratch_dma {
	void *cpu;
	dma_addr_t dma;
	size_t size;
};

struct hws_pcie_dev {
	/* ───── core objects ───── */
	struct pci_dev *pdev;
	struct hws_audio audio[MAX_VID_CHANNELS];
	struct hws_video video[MAX_VID_CHANNELS];

	/* ───── BAR & workqueues ───── */
	void __iomem *bar0_base;

	/* ───── device identity / capabilities ───── */
	u16 vendor_id;
	u16 device_id;
	u16 device_ver;
	u16 hw_ver;
	u32 sub_ver;
	u32 port_id;
	// TriState, used in `set_video_format_size`
	u32 support_yv12;
	u32 max_hw_video_buf_sz;
	u8 max_channels;
	u8 cur_max_video_ch;
	u8 cur_max_linein_ch;
	bool start_run;

	bool buf_allocated;
	u32 audio_pkt_size;

	/* ───── V4L2 framework objects ───── */
	struct v4l2_device v4l2_device;

	struct snd_card *snd_card;

	/* ───── kernel thread ───── */
	struct task_struct *main_task;
	struct hws_scratch_dma scratch_vid[MAX_VID_CHANNELS];

	bool suspended;
	int irq;

	/* ───── error flags ───── */
	int pci_lost;

};

#endif
