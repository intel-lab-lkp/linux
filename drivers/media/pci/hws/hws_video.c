// SPDX-License-Identifier: GPL-2.0-only
#include <linux/pci.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/compiler.h>
#include <linux/overflow.h>
#include <linux/delay.h>
#include <linux/bits.h>
#include <linux/jiffies.h>

#include <media/v4l2-ioctl.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-event.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-core.h>
#include <media/v4l2-device.h>
#include <media/videobuf2-dma-contig.h>

#include "hws.h"
#include "hws_reg.h"
#include "hws_video.h"
#include "hws_audio.h"
#include "hws_irq.h"
#include "hws_v4l2_ioctl.h"

#include <sound/core.h>
#include <sound/control.h>
#include <sound/pcm.h>
#include <sound/rawmidi.h>
#include <sound/initval.h>

#define HWS_MAX_BUFS 32
#define HWS_REMAP_SLOT_OFF(ch)   (0x208 + (ch) * 8)	/* one 64-bit slot per ch */
#define HWS_BUF_BASE_OFF(ch)     (CVBS_IN_BUF_BASE  + (ch) * PCIE_BARADDROFSIZE)
#define HWS_HALF_SZ_OFF(ch)      (CVBS_IN_BUF_BASE2 + (ch) * PCIE_BARADDROFSIZE)

static void update_live_resolution(struct hws_pcie_dev *pdx, unsigned int ch);
static bool hws_update_active_interlace(struct hws_pcie_dev *pdx,
					unsigned int ch);
static void handle_hwv2_path(struct hws_pcie_dev *hws, unsigned int ch);
static void handle_legacy_path(struct hws_pcie_dev *hws, unsigned int ch);
static u32 hws_calc_sizeimage(struct hws_video *v, u16 w, u16 h,
			      bool interlaced);

/* Two-buffer approach helper functions */
static void hws_ring_release(struct hws_video *vid);
static int hws_ring_setup(struct hws_video *vid);
static bool hws_use_ring(struct hws_video *vid);
static void hws_set_dma_doorbell(struct hws_pcie_dev *hws, unsigned int ch,
				 dma_addr_t dma, const char *tag);
static void hws_program_dma_window(struct hws_video *vid, dma_addr_t dma);

static bool hws_use_ring(struct hws_video *vid)
{
	/* Use ring buffer approach for legacy hardware compatibility */
	return true;
}

static void hws_set_dma_doorbell(struct hws_pcie_dev *hws, unsigned int ch,
				 dma_addr_t dma, const char *tag)
{
	iowrite32(lower_32_bits(dma), hws->bar0_base + HWS_REG_DMA_ADDR(ch));
	dev_dbg(&hws->pdev->dev, "dma_doorbell ch%u: dma=0x%llx tag=%s\n", ch,
		(u64)dma, tag ? tag : "");
}

static void hws_program_dma_window(struct hws_video *vid, dma_addr_t dma)
{
	const u32 addr_mask = PCI_E_BAR_ADD_MASK;	/* 0xE0000000 */
	const u32 addr_low_mask = PCI_E_BAR_ADD_LOWMASK;	/* 0x1FFFFFFF */
	struct hws_pcie_dev *hws = vid->parent;
	unsigned int ch = vid->channel_index;
	u32 table_off = HWS_REMAP_SLOT_OFF(ch);
	u32 lo = lower_32_bits(dma);
	u32 hi = upper_32_bits(dma);
	u32 pci_addr = lo & addr_low_mask;	/* low 29 bits inside 512MB window */
	u32 page_lo = lo & addr_mask;	/* bits 31..29 only (page bits) */

	/* Remap entry: HI then page_LO (legacy order) */
	writel(hi, hws->bar0_base + PCI_ADDR_TABLE_BASE + table_off);
	(void)readl(hws->bar0_base + PCI_ADDR_TABLE_BASE + table_off);
	writel(page_lo,
	       hws->bar0_base + PCI_ADDR_TABLE_BASE + table_off +
	       PCIE_BARADDROFSIZE);
	(void)readl(hws->bar0_base + PCI_ADDR_TABLE_BASE + table_off +
		    PCIE_BARADDROFSIZE);

	/* Program per-channel base and half-size */
	writel((ch + 1) * PCIEBAR_AXI_BASE + pci_addr,
	       hws->bar0_base + HWS_BUF_BASE_OFF(ch));
	(void)readl(hws->bar0_base + HWS_BUF_BASE_OFF(ch));

	writel(vid->pix.half_size / 16, hws->bar0_base + HWS_HALF_SZ_OFF(ch));
	(void)readl(hws->bar0_base + HWS_HALF_SZ_OFF(ch));

	/* Optional: verify and log */
	{
		u32 r_hi =
		    readl(hws->bar0_base + PCI_ADDR_TABLE_BASE + table_off);
		u32 r_lo =
		    readl(hws->bar0_base + PCI_ADDR_TABLE_BASE + table_off +
			  PCIE_BARADDROFSIZE);
		u32 r_base = readl(hws->bar0_base + HWS_BUF_BASE_OFF(ch));
		u32 r_half = readl(hws->bar0_base + HWS_HALF_SZ_OFF(ch));

		dev_dbg(&hws->pdev->dev,
			"ch%u remap: hi=0x%08x(lo exp 0x%08x got 0x%08x) base=0x%08x exp=0x%08x half16B=0x%08x\n",
			ch, r_hi, page_lo, r_lo, r_base,
			(ch + 1) * PCIEBAR_AXI_BASE + pci_addr, r_half);
	}
}

static int hws_ring_setup(struct hws_video *vid)
{
	struct hws_pcie_dev *hws = vid->parent;
	size_t need;

	if (vid->ring_cpu)
		return 0;	/* already allocated */

	need = PAGE_ALIGN(vid->pix.sizeimage * 2);	/* double buffer */
	vid->ring_cpu =
	    dma_alloc_coherent(&hws->pdev->dev, need, &vid->ring_dma,
			       GFP_KERNEL);
	if (!vid->ring_cpu)
		return -ENOMEM;

	vid->ring_size = need;
	vid->ring_toggle_prev = 0;
	vid->ring_toggle_hw = 0;
	vid->ring_first_half_copied = false;
	vid->ring_last_toggle_jiffies = jiffies;

	dev_dbg(&hws->pdev->dev,
		"ring_setup: ch%u allocated %zu bytes dma=0x%llx\n",
		vid->channel_index, need, (u64)vid->ring_dma);
	return 0;
}

static void hws_ring_release(struct hws_video *vid)
{
	struct hws_pcie_dev *hws = vid->parent;

	if (vid->ring_cpu) {
		dma_free_coherent(&hws->pdev->dev, vid->ring_size,
				  vid->ring_cpu, vid->ring_dma);
		vid->ring_cpu = NULL;
		vid->ring_size = 0;
		vid->ring_dma = 0;
	}
}

static bool hws_force_no_signal_frame(struct hws_video *v, const char *tag)
{
	struct hws_pcie_dev *hws;
	unsigned long flags;
	struct hwsvideo_buffer *buf = NULL, *next = NULL;
	bool ring_mode;
	bool have_next = false;
	bool doorbell = false;

	if (!v)
		return false;
	hws = v->parent;
	if (!hws || READ_ONCE(v->stop_requested) || !READ_ONCE(v->cap_active))
		return false;
	ring_mode = hws_use_ring(v);
	spin_lock_irqsave(&v->irq_lock, flags);
	if (v->active) {
		buf = v->active;
		v->active = NULL;
		buf->slot = 0;
	} else if (!list_empty(&v->capture_queue)) {
		buf = list_first_entry(&v->capture_queue,
				       struct hwsvideo_buffer, list);
		list_del_init(&buf->list);
		if (v->queued_count)
			v->queued_count--;
		buf->slot = 0;
	}
	if (!list_empty(&v->capture_queue)) {
		next = list_first_entry(&v->capture_queue,
					struct hwsvideo_buffer, list);
		list_del_init(&next->list);
		if (v->queued_count)
			v->queued_count--;
		next->slot = 0;
		v->active = next;
		have_next = true;
	} else {
		v->active = NULL;
	}
	spin_unlock_irqrestore(&v->irq_lock, flags);
	/* Reset toggle tracking so the next real VDONE is observed */
	WRITE_ONCE(v->ring_toggle_prev, 0);
	WRITE_ONCE(v->ring_toggle_hw, 0);
	if (!buf)
		return false;
	/* Complete buffer with a neutral frame so dequeuers keep running. */
	{
		struct vb2_v4l2_buffer *vb2v = &buf->vb;
		void *dst = vb2_plane_vaddr(&vb2v->vb2_buf, 0);

		if (dst)
			memset(dst, 0x10, v->pix.sizeimage);
		vb2_set_plane_payload(&vb2v->vb2_buf, 0, v->pix.sizeimage);
		vb2v->sequence = ++v->sequence_number;
		vb2v->vb2_buf.timestamp = ktime_get_ns();
		vb2_buffer_done(&vb2v->vb2_buf, VB2_BUF_STATE_DONE);
	}
	WRITE_ONCE(v->ring_first_half_copied, false);
	WRITE_ONCE(v->ring_last_toggle_jiffies, jiffies);
	if (ring_mode && v->ring_cpu) {
		hws_program_dma_window(v, v->ring_dma);
		hws_set_dma_doorbell(hws, v->channel_index, v->ring_dma,
				     tag ? tag : "nosignal_ring");
		doorbell = true;
	} else if (have_next && next) {
		dma_addr_t dma =
		    vb2_dma_contig_plane_dma_addr(&next->vb.vb2_buf, 0);
		hws_program_dma_for_addr(hws, v->channel_index, dma);
		hws_set_dma_doorbell(hws, v->channel_index, dma,
				     tag ? tag : "nosignal_zero");
		doorbell = true;
	} else if (ring_mode && !v->ring_cpu) {
		dev_warn(&hws->pdev->dev,
			 "nosignal: ch%u ring buffer missing, cannot doorbell\n",
			 v->channel_index);
	}
	if (doorbell) {
		wmb(); /* ensure descriptors visible before enabling capture */
		hws_enable_video_capture(hws, v->channel_index, true);
	}
	return true;
}

static int hws_ctrls_init(struct hws_video *vid)
{
	struct v4l2_ctrl_handler *hdl = &vid->control_handler;

	/* Create BCHS + one DV status control */
	v4l2_ctrl_handler_init(hdl, 4);

	vid->ctrl_brightness = v4l2_ctrl_new_std(hdl, &hws_ctrl_ops,
						 V4L2_CID_BRIGHTNESS,
						 MIN_VAMP_BRIGHTNESS_UNITS,
						 MAX_VAMP_BRIGHTNESS_UNITS, 1,
						 HWS_BRIGHTNESS_DEFAULT);

	vid->ctrl_contrast =
	    v4l2_ctrl_new_std(hdl, &hws_ctrl_ops, V4L2_CID_CONTRAST,
			      MIN_VAMP_CONTRAST_UNITS, MAX_VAMP_CONTRAST_UNITS,
			      1, HWS_CONTRAST_DEFAULT);

	vid->ctrl_saturation = v4l2_ctrl_new_std(hdl, &hws_ctrl_ops,
						 V4L2_CID_SATURATION,
						 MIN_VAMP_SATURATION_UNITS,
						 MAX_VAMP_SATURATION_UNITS, 1,
						 HWS_SATURATION_DEFAULT);

	vid->ctrl_hue = v4l2_ctrl_new_std(hdl, &hws_ctrl_ops, V4L2_CID_HUE,
					  MIN_VAMP_HUE_UNITS,
					  MAX_VAMP_HUE_UNITS, 1,
					  HWS_HUE_DEFAULT);

	if (hdl->error) {
		int err = hdl->error;

		v4l2_ctrl_handler_free(hdl);
		return err;
	}
	return 0;
}

int hws_video_init_channel(struct hws_pcie_dev *pdev, int ch)
{
	struct hws_video *vid;

	/* basic sanity */
	if (!pdev || ch < 0 || ch >= pdev->max_channels)
		return -EINVAL;

	vid = &pdev->video[ch];

	/* hard reset the per-channel struct (safe here since we init everything next) */
	memset(vid, 0, sizeof(*vid));

	/* identity */
	vid->parent = pdev;
	vid->channel_index = ch;

	/* locks & lists */
	mutex_init(&vid->state_lock);
	mutex_init(&vid->qlock);
	spin_lock_init(&vid->irq_lock);
	INIT_LIST_HEAD(&vid->capture_queue);
	vid->sequence_number = 0;
	vid->active = NULL;

	/* typed tasklet: bind handler once */
	tasklet_setup(&vid->bh_tasklet, hws_bh_video);

	/* DMA watchdog removed; retain counters for diagnostics */
	vid->timeout_count = 0;
	vid->error_count = 0;

	/* Two-buffer approach initialization */
	vid->ring_cpu = NULL;
	vid->ring_dma = 0;
	vid->ring_size = 0;
	vid->ring_toggle_prev = 0;
	vid->ring_toggle_hw = 0;
	vid->ring_first_half_copied = false;
	vid->ring_last_toggle_jiffies = jiffies;
	vid->queued_count = 0;

	/* default format (adjust to your HW) */
	vid->pix.width = 1920;
	vid->pix.height = 1080;
	vid->pix.fourcc = V4L2_PIX_FMT_YUYV;
	vid->pix.bytesperline = ALIGN(vid->pix.width * 2, 64);
	vid->pix.sizeimage = vid->pix.bytesperline * vid->pix.height;
	vid->pix.field = V4L2_FIELD_NONE;
	vid->pix.colorspace = V4L2_COLORSPACE_REC709;
	vid->pix.ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	vid->pix.quantization = V4L2_QUANTIZATION_LIM_RANGE;
	vid->pix.xfer_func = V4L2_XFER_FUNC_DEFAULT;
	vid->pix.interlaced = false;
	vid->pix.half_size = vid->pix.sizeimage / 2;	/* if HW uses halves */
	vid->alloc_sizeimage = vid->pix.sizeimage;

	/* color controls default (mid-scale) */
	vid->current_brightness = 0x80;
	vid->current_contrast = 0x80;
	vid->current_saturation = 0x80;
	vid->current_hue = 0x80;

	/* capture state */
	vid->cap_active = false;
	vid->stop_requested = false;
	vid->last_buf_half_toggle = 0;
	vid->half_seen = false;
	vid->signal_loss_cnt = 0;

	/* Create BCHS + DV power-present as modern controls */
	{
		int err = hws_ctrls_init(vid);

		if (err) {
			dev_err(&pdev->pdev->dev,
				"v4l2 ctrl init failed on ch%d: %d\n", ch, err);
			return err;
		}
	}

	return 0;
}

static void hws_video_drain_queue_locked(struct hws_video *vid)
{
	/* Return in-flight first */
	if (vid->active) {
		vb2_buffer_done(&vid->active->vb.vb2_buf, VB2_BUF_STATE_ERROR);
		vid->active = NULL;
	}

	/* Then everything queued */
	while (!list_empty(&vid->capture_queue)) {
		struct hwsvideo_buffer *b =
		    list_first_entry(&vid->capture_queue,
				     struct hwsvideo_buffer,
				     list);
		list_del_init(&b->list);
		vb2_buffer_done(&b->vb.vb2_buf, VB2_BUF_STATE_ERROR);
	}
}

void hws_video_cleanup_channel(struct hws_pcie_dev *pdev, int ch)
{
	struct hws_video *vid;
	unsigned long flags;

	if (!pdev || ch < 0 || ch >= pdev->max_channels)
		return;

	vid = &pdev->video[ch];

	/* 1) Stop HW best-effort for this channel */
	hws_enable_video_capture(vid->parent, vid->channel_index, false);

	/* 2) Flip software state so IRQ/BH will be no-ops if they run */
	WRITE_ONCE(vid->stop_requested, true);
	WRITE_ONCE(vid->cap_active, false);

	/* 3) Make sure the tasklet can’t run anymore (prevents races with drain) */
	tasklet_kill(&vid->bh_tasklet);

	/* 4) Drain SW capture queue & in-flight under lock */
	spin_lock_irqsave(&vid->irq_lock, flags);
	hws_video_drain_queue_locked(vid);
	spin_unlock_irqrestore(&vid->irq_lock, flags);

	/* 4.5) Release ring buffer */
	hws_ring_release(vid);

	/* 5) Release VB2 queue if initialized */
	if (vid->buffer_queue.ops)
		vb2_queue_release(&vid->buffer_queue);

	/* 6) Free V4L2 controls */
	v4l2_ctrl_handler_free(&vid->control_handler);

	/* 7) Unregister the video_device if we own it */
	if (vid->video_device && video_is_registered(vid->video_device))
		video_unregister_device(vid->video_device);
	/* If you allocated it with video_device_alloc(), release it here:
	 * video_device_release(vid->video_device);
	 */
	vid->video_device = NULL;

	/* 8) Reset simple state (don’t memset the whole struct here) */
	mutex_destroy(&vid->state_lock);
	mutex_destroy(&vid->qlock);
	INIT_LIST_HEAD(&vid->capture_queue);
	vid->active = NULL;
	vid->stop_requested = false;
	vid->last_buf_half_toggle = 0;
	vid->half_seen = false;
	vid->signal_loss_cnt = 0;
}

/* Convenience cast */
static inline struct hwsvideo_buffer *to_hwsbuf(struct vb2_buffer *vb)
{
	return container_of(to_vb2_v4l2_buffer(vb), struct hwsvideo_buffer, vb);
}

static int hws_buf_init(struct vb2_buffer *vb)
{
	struct hwsvideo_buffer *b = to_hwsbuf(vb);

	INIT_LIST_HEAD(&b->list);
	return 0;
}

static void hws_buf_finish(struct vb2_buffer *vb)
{
	/* vb2 core handles cache maintenance for dma-contig buffers */
	(void)vb;
}

static void hws_buf_cleanup(struct vb2_buffer *vb)
{
	struct hwsvideo_buffer *b = to_hwsbuf(vb);

	if (!list_empty(&b->list))
		list_del_init(&b->list);
}

void hws_program_dma_for_addr(struct hws_pcie_dev *hws, unsigned int ch,
			      dma_addr_t dma)
{
	struct hws_video *vid = &hws->video[ch];

	hws_program_dma_window(vid, dma);
}

void hws_enable_video_capture(struct hws_pcie_dev *hws, unsigned int chan,
			      bool on)
{
	u32 status;

	if (!hws || hws->pci_lost || chan >= hws->max_channels)
		return;

	status = readl(hws->bar0_base + HWS_REG_VCAP_ENABLE);
	status = on ? (status | BIT(chan)) : (status & ~BIT(chan));
	writel(status, hws->bar0_base + HWS_REG_VCAP_ENABLE);
	(void)readl(hws->bar0_base + HWS_REG_VCAP_ENABLE);

	hws->video[chan].cap_active = on;

	dev_dbg(&hws->pdev->dev, "vcap %s ch%u (reg=0x%08x)\n",
		on ? "ON" : "OFF", chan, status);
}

static void hws_seed_dma_windows(struct hws_pcie_dev *hws)
{
	const u32 addr_mask = PCI_E_BAR_ADD_MASK;
	const u32 addr_low_mask = PCI_E_BAR_ADD_LOWMASK;
	u32 table = 0x208;	/* one 64-bit entry per channel */
	unsigned int ch;

	if (!hws || !hws->bar0_base)
		return;

	/* If cur_max_video_ch isn’t set yet, default to max_channels */
	if (!hws->cur_max_video_ch || hws->cur_max_video_ch > hws->max_channels)
		hws->cur_max_video_ch = hws->max_channels;

	for (ch = 0; ch < hws->cur_max_video_ch; ch++, table += 8) {
		/* 1) Ensure a tiny, valid DMA buf exists (1 page is plenty) */
		if (!hws->scratch_vid[ch].cpu) {
			hws->scratch_vid[ch].size = PAGE_SIZE;
			hws->scratch_vid[ch].cpu =
			    dma_alloc_coherent(&hws->pdev->dev,
					       hws->scratch_vid[ch].size,
					       &hws->scratch_vid[ch].dma,
					       GFP_KERNEL);
			if (!hws->scratch_vid[ch].cpu) {
				dev_warn(&hws->pdev->dev,
					 "ch%u: scratch DMA alloc failed, skipping seed\n",
					 ch);
				continue;
			}
		}

		/* 2) Program 64-bit BAR remap entry for this channel */
		{
			dma_addr_t p = hws->scratch_vid[ch].dma;
			u32 lo = lower_32_bits(p) & addr_mask;
			u32 hi = upper_32_bits(p);
			u32 pci_addr_low = lower_32_bits(p) & addr_low_mask;

			writel_relaxed(hi,
				       hws->bar0_base + PCI_ADDR_TABLE_BASE +
				       table);
			writel_relaxed(lo,
				       hws->bar0_base + PCI_ADDR_TABLE_BASE +
				       table + PCIE_BARADDROFSIZE);

			/* 3) Per-channel AXI base + PCI low */
			writel_relaxed((ch + 1) * PCIEBAR_AXI_BASE +
				       pci_addr_low,
				       hws->bar0_base + CVBS_IN_BUF_BASE +
				       ch * PCIE_BARADDROFSIZE);

			/* 4) Half-frame length in /16 units.
			 * Prefer the current channel’s computed half_size if available.
			 * Fall back to PAGE_SIZE/2.
			 */
			{
				u32 half_bytes = hws->video[ch].pix.half_size ?
				    hws->video[ch].pix.half_size :
				    (PAGE_SIZE / 2);
				writel_relaxed(half_bytes / 16,
					       hws->bar0_base +
					       CVBS_IN_BUF_BASE2 +
					       ch * PCIE_BARADDROFSIZE);
			}
		}
	}

	/* Post writes so device sees them before we move on */
	(void)readl(hws->bar0_base + HWS_REG_INT_STATUS);
}

#define LOG_DEC(tag)							\
	dev_info(&hws->pdev->dev, "DEC_MODE %s = 0x%08x\n",		\
		 (tag), readl(hws->bar0_base + HWS_REG_DEC_MODE))

static void hws_ack_all_irqs(struct hws_pcie_dev *hws)
{
	u32 st = readl(hws->bar0_base + HWS_REG_INT_STATUS);

	if (st) {
		writel(st, hws->bar0_base + HWS_REG_INT_STATUS);	/* W1C */
		(void)readl(hws->bar0_base + HWS_REG_INT_STATUS);
	}
}

static void hws_open_irq_fabric(struct hws_pcie_dev *hws)
{
	/* Route all sources to vector 0 (same value you’re already using) */
	writel(0x00000000, hws->bar0_base + PCIE_INT_DEC_REG_BASE);
	(void)readl(hws->bar0_base + PCIE_INT_DEC_REG_BASE);

	/* Turn on the bridge if your IP needs it */
	writel(0x00000001, hws->bar0_base + PCIEBR_EN_REG_BASE);
	(void)readl(hws->bar0_base + PCIEBR_EN_REG_BASE);

	/* Open the global/bridge gate (legacy 0x3FFFF) */
	writel(HWS_INT_EN_MASK, hws->bar0_base + INT_EN_REG_BASE);
	(void)readl(hws->bar0_base + INT_EN_REG_BASE);
}

void hws_init_video_sys(struct hws_pcie_dev *hws, bool enable)
{
	int i;

	if (hws->start_run && !enable)
		return;

	/* 1) reset the decoder mode register to 0 */
	writel(0x00000000, hws->bar0_base + HWS_REG_DEC_MODE);
	LOG_DEC("after reset");
	hws_seed_dma_windows(hws);

	/* 3) on a full reset, clear all per-channel status and indices */
	if (!enable) {
		for (i = 0; i < hws->max_channels; i++) {
			/* helpers to arm/disable capture engines */
			hws_enable_video_capture(hws, i, false);
			hws_enable_audio_capture(hws, i, false);
		}
	}

	/* 4) “Start run”: set bit31, wait a bit, then program low 24 bits */
	writel(0x80000000, hws->bar0_base + HWS_REG_DEC_MODE);
	LOG_DEC("start run");
	/* udelay(500); */
	writel(0x80FFFFFF, hws->bar0_base + HWS_REG_DEC_MODE);
	LOG_DEC("after mask");
	writel(0x13, hws->bar0_base + HWS_REG_DEC_MODE);
	LOG_DEC("final");
	hws_ack_all_irqs(hws);
	hws_open_irq_fabric(hws);
	/* 6) record that we're now running */
	hws->start_run = true;
}

int hws_check_card_status(struct hws_pcie_dev *hws)
{
	u32 status;

	if (!hws || !hws->bar0_base)
		return -ENODEV;

	status = readl(hws->bar0_base + HWS_REG_SYS_STATUS);

	/* Common “device missing” pattern */
	if (unlikely(status == 0xFFFFFFFF)) {
		hws->pci_lost = true;
		dev_err(&hws->pdev->dev, "PCIe device not responding\n");
		return -ENODEV;
	}

	/* If RUN/READY bit (bit0) isn’t set, (re)initialize the video core */
	if (!(status & BIT(0))) {
		dev_dbg(&hws->pdev->dev,
			"SYS_STATUS not ready (0x%08x), reinitializing\n",
			status);
		hws_init_video_sys(hws, true);
		/* Optional: verify the core cleared its busy bit, if you have one */
		/* int ret = hws_check_busy(hws); */
		/* if (ret) return ret; */
	}

	return 0;
}

void check_video_format(struct hws_pcie_dev *pdx)
{
	int i;

	for (i = 0; i < pdx->cur_max_video_ch; i++) {
		if (!hws_update_active_interlace(pdx, i)) {
			/* return 1; */                       /* no active video */
			if (pdx->video[i].signal_loss_cnt == 0)
				pdx->video[i].signal_loss_cnt = 1;
		} else {
			if (pdx->hw_ver > 0)
				handle_hwv2_path(pdx, i);
			else
				/* FIXME: legacy struct names in subfunction */
				handle_legacy_path(pdx, i);

			update_live_resolution(pdx, i);
			pdx->video[i].signal_loss_cnt = 0;
		}

		/* If we just detected a loss on an active capture channel… */
		if (pdx->video[i].signal_loss_cnt == 1 &&
		    pdx->video[i].cap_active) {
			/* Use the two-buffer approach for signal loss handling */
			hws_force_no_signal_frame(&pdx->video[i],
						  "monitor_nosignal");
			pdx->video[i].signal_loss_cnt = 2;
		}
	}
}

static inline void hws_write_if_diff(struct hws_pcie_dev *hws, u32 reg_off,
				     u32 new_val)
{
	void __iomem *addr;
	u32 old;

	if (!hws || !hws->bar0_base)
		return;

	addr = hws->bar0_base + reg_off;

	old = readl(addr);
	/* Treat all-ones as device gone; avoid writing garbage. */
	if (unlikely(old == 0xFFFFFFFF)) {
		hws->pci_lost = true;
		return;
	}

	if (old != new_val) {
		writel(new_val, addr);
		/* Post the write on some bridges / enforce ordering. */
		(void)readl(addr);
	}
}

static bool hws_update_active_interlace(struct hws_pcie_dev *pdx,
					unsigned int ch)
{
	u32 reg;
	bool active, interlace;

	if (ch >= pdx->cur_max_video_ch)
		return false;

	reg = readl(pdx->bar0_base + HWS_REG_ACTIVE_STATUS);
	active = !!(reg & BIT(ch));
	interlace = !!(reg & BIT(8 + ch));

	WRITE_ONCE(pdx->video[ch].pix.interlaced, interlace);
	return active;
}

/* Modern hardware path: keep HW registers in sync with current per-channel
 * software state. Adjust the OUT_* bits below to match your HW contract.
 */
static void handle_hwv2_path(struct hws_pcie_dev *hws, unsigned int ch)
{
	struct hws_video *vid;
	u32 reg, in_fps, cur_out_res, want_out_res;

	if (!hws || !hws->bar0_base || ch >= hws->max_channels)
		return;

	vid = &hws->video[ch];

	/* 1) Input frame rate (read-only; log for debugging) */
	in_fps = readl(hws->bar0_base + HWS_REG_FRAME_RATE(ch));
	dev_dbg(&hws->pdev->dev, "ch%u input fps=%u\n", ch, in_fps);

	/* 2) Output resolution programming
	 * If your HW expects a separate “scaled” size, add fields to track it.
	 * For now, mirror the current format (fmt_curr) to OUT_RES.
	 */
	want_out_res = (vid->pix.height << 16) | vid->pix.width;
	cur_out_res = readl(hws->bar0_base + HWS_REG_OUT_RES(ch));
	if (cur_out_res != want_out_res)
		hws_write_if_diff(hws, HWS_REG_OUT_RES(ch), want_out_res);

	/* 3) Output FPS: only program if you actually track a target.
	 * Example heuristic (disabled by default):
	 *
	 *   u32 out_fps = (vid->fmt_curr.height >= 1080) ? 60 : 30;
	 *   hws_write_if_diff(hws, HWS_REG_OUT_FRAME_RATE(ch), out_fps);
	 */

	/* 4) BCHS controls: pack from per-channel current_* fields */
	reg = readl(hws->bar0_base + HWS_REG_BCHS(ch));
	{
		u8 br = reg & 0xFF;
		u8 co = (reg >> 8) & 0xFF;
		u8 hu = (reg >> 16) & 0xFF;
		u8 sa = (reg >> 24) & 0xFF;

		if (br != vid->current_brightness ||
		    co != vid->current_contrast || hu != vid->current_hue ||
		    sa != vid->current_saturation) {
			u32 packed = (vid->current_saturation << 24) |
			    (vid->current_hue << 16) |
			    (vid->current_contrast << 8) |
			    vid->current_brightness;
			hws_write_if_diff(hws, HWS_REG_BCHS(ch), packed);
		}
	}

	/* 5) HDCP detect: read only (no cache field in your structs today) */
	reg = readl(hws->bar0_base + HWS_REG_HDCP_STATUS);
	/* bool hdcp = !!(reg & BIT(ch)); use if you later add a field/control */
}

static void handle_legacy_path(struct hws_pcie_dev *hws, unsigned int ch)
{
	/* No-op by default. If you introduce a SW FPS accumulator, map it here.
	 *
	 * Example skeleton:
	 *
	 *   u32 sw_rate = READ_ONCE(hws->sw_fps[ch]); incremented elsewhere
	 *   if (sw_rate > THRESHOLD) {
	 *       u32 fps = pick_fps_from_rate(sw_rate);
	 *       hws_write_if_diff(hws, HWS_REG_OUT_FRAME_RATE(ch), fps);
	 *       WRITE_ONCE(hws->sw_fps[ch], 0);
	 *   }
	 */
	(void)hws;
	(void)ch;
}

static void hws_video_apply_mode_change(struct hws_pcie_dev *pdx,
					unsigned int ch, u16 w, u16 h,
					bool interlaced)
{
	struct hws_video *v = &pdx->video[ch];
	unsigned long flags;
	u32 new_size;
	bool reenable = false;

	if (!pdx || !pdx->bar0_base)
		return;
	if (ch >= pdx->max_channels)
		return;
	if (!w || !h || w > MAX_VIDEO_HW_W ||
	    (!interlaced && h > MAX_VIDEO_HW_H) ||
	    (interlaced && (h * 2) > MAX_VIDEO_HW_H))
		return;

	WRITE_ONCE(v->stop_requested, true);
	WRITE_ONCE(v->cap_active, false);
	/* Publish software stop first so the hardirq/BH see the stop before
	 * we touch MMIO or the lists. Pairs with READ_ONCE() checks in
	 * hws_bh_video() and hws_arm_next(). Required to prevent the BH/ISR
	 * from completing/arming buffers while we are changing modes.
	 */
	smp_wmb();

	hws_enable_video_capture(pdx, ch, false);
	readl(pdx->bar0_base + HWS_REG_INT_STATUS);

	tasklet_kill(&v->bh_tasklet);

	spin_lock_irqsave(&v->irq_lock, flags);
	if (v->active) {
		vb2_buffer_done(&v->active->vb.vb2_buf, VB2_BUF_STATE_ERROR);
		v->active = NULL;
	}
	while (!list_empty(&v->capture_queue)) {
		struct hwsvideo_buffer *b = list_first_entry(&v->capture_queue,
							     struct
							     hwsvideo_buffer,
							     list);
		list_del_init(&b->list);
		vb2_buffer_done(&b->vb.vb2_buf, VB2_BUF_STATE_ERROR);
	}
	spin_unlock_irqrestore(&v->irq_lock, flags);

	/* Update software pixel state */
	v->pix.width = w;
	v->pix.height = h;
	v->pix.interlaced = interlaced;

	new_size = hws_calc_sizeimage(v, w, h, interlaced);

	/* If buffers are smaller than new requirement, signal src-change & error the queue */
	if (vb2_is_busy(&v->buffer_queue) && new_size > v->alloc_sizeimage) {
		struct v4l2_event ev = {
			.type = V4L2_EVENT_SOURCE_CHANGE,
		};
		ev.u.src_change.changes = V4L2_EVENT_SRC_CH_RESOLUTION;

		v4l2_event_queue(v->video_device, &ev);
		vb2_queue_error(&v->buffer_queue);
		return;
	}

	/* Program HW with new resolution */
	hws_write_if_diff(pdx, HWS_REG_OUT_RES(ch), (h << 16) | w);

	/* Legacy half-buffer programming */
	writel(v->pix.half_size / 16,
	       pdx->bar0_base + CVBS_IN_BUF_BASE2 + ch * PCIE_BARADDROFSIZE);
	(void)readl(pdx->bar0_base + CVBS_IN_BUF_BASE2 +
		    ch * PCIE_BARADDROFSIZE);

	/* Reset per-channel toggles/counters */
	WRITE_ONCE(v->last_buf_half_toggle, 0);
	v->sequence_number = 0;

	/* Re-prime first VB2 buffer if present */
	spin_lock_irqsave(&v->irq_lock, flags);
	if (!list_empty(&v->capture_queue)) {
		struct hwsvideo_buffer *buf;
		dma_addr_t dma;

		buf = list_first_entry(&v->capture_queue,
				       struct hwsvideo_buffer, list);
		v->active = buf;
		list_del_init(&v->active->list);
		dma = vb2_dma_contig_plane_dma_addr(&buf->vb.vb2_buf, 0);
		hws_program_dma_for_addr(pdx, ch, dma);
		iowrite32(lower_32_bits(dma),
			  pdx->bar0_base + HWS_REG_DMA_ADDR(ch));
		reenable = true;
	}
	spin_unlock_irqrestore(&v->irq_lock, flags);

	if (!reenable)
		return;

	WRITE_ONCE(v->stop_requested, false);
	WRITE_ONCE(v->cap_active, true);
	/* Publish stop_requested/cap_active before HW disable; pairs with
	 * BH/ISR reads in hws_bh_video/hws_arm_next.
	 */
	smp_wmb();
	wmb(); /* ensure DMA window/address writes visible before enable */
	hws_enable_video_capture(pdx, ch, true);
	readl(pdx->bar0_base + HWS_REG_INT_STATUS);
}

static void update_live_resolution(struct hws_pcie_dev *pdx, unsigned int ch)
{
	u32 reg = readl(pdx->bar0_base + HWS_REG_IN_RES(ch));
	u16 res_w = reg & 0xFFFF;
	u16 res_h = (reg >> 16) & 0xFFFF;
	bool interlace = READ_ONCE(pdx->video[ch].pix.interlaced);

	bool within_hw = (res_w <= MAX_VIDEO_HW_W) &&
	    ((!interlace && res_h <= MAX_VIDEO_HW_H) ||
	     (interlace && (res_h * 2) <= MAX_VIDEO_HW_H));

	if (!within_hw)
		return;

	if (res_w != pdx->video[ch].pix.width ||
	    res_h != pdx->video[ch].pix.height) {
		hws_video_apply_mode_change(pdx, ch, res_w, res_h, interlace);
	}
}

static int hws_open(struct file *file)
{
	int ret;
	struct hws_video *vid;

	/* Create V4L2 file handle so events & priorities work */
	ret = v4l2_fh_open(file);
	if (ret)
		return ret;

	vid = video_drvdata(file);

	/* Hard-fail additional opens while a capture is active */
	if (!v4l2_fh_is_singular_file(file) && vb2_is_busy(&vid->buffer_queue)) {
		v4l2_fh_release(file);
		return -EBUSY;
	}

	return 0;
}

static int hws_release(struct file *file)
{
	return vb2_fop_release(file);
}

static const struct v4l2_file_operations hws_fops = {
	.owner = THIS_MODULE,
	.open = hws_open,
	.release = hws_release,
	.poll = vb2_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = vb2_fop_mmap,
};

static int hws_subscribe_event(struct v4l2_fh *fh,
			       const struct v4l2_event_subscription *sub)
{
	switch (sub->type) {
	case V4L2_EVENT_SOURCE_CHANGE:
		return v4l2_src_change_event_subscribe(fh, sub);
	case V4L2_EVENT_CTRL:
		return v4l2_ctrl_subscribe_event(fh, sub);
	default:
		return -EINVAL;
	}
}

static const struct v4l2_ioctl_ops hws_ioctl_fops = {
	/* Core caps/info */
	.vidioc_querycap = hws_vidioc_querycap,

	/* Pixel format: still needed to report YUYV etc. */
	.vidioc_enum_fmt_vid_cap = hws_vidioc_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap = hws_vidioc_g_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap = hws_vidioc_s_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap = hws_vidioc_try_fmt_vid_cap,

	/* Buffer queueing / streaming */
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,

	/* Inputs */
	.vidioc_enum_input = hws_vidioc_enum_input,
	.vidioc_g_input = hws_vidioc_g_input,
	.vidioc_s_input = hws_vidioc_s_input,

	/* DV timings (HDMI/DVI/VESA modes) */
	.vidioc_query_dv_timings = hws_vidioc_query_dv_timings,
	.vidioc_enum_dv_timings = hws_vidioc_enum_dv_timings,
	.vidioc_g_dv_timings = hws_vidioc_g_dv_timings,
	.vidioc_s_dv_timings = hws_vidioc_s_dv_timings,
	.vidioc_dv_timings_cap = hws_vidioc_dv_timings_cap,

	.vidioc_log_status = vidioc_log_status,
	.vidioc_subscribe_event = hws_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
	.vidioc_g_parm = hws_vidioc_g_parm,
	.vidioc_s_parm = hws_vidioc_s_parm,
};

static u32 hws_calc_sizeimage(struct hws_video *v, u16 w, u16 h,
			      bool interlaced)
{
	/* example for packed 16bpp (YUYV); replace with your real math/align */
	u32 lines = h;		/* full frame lines for sizeimage */
	u32 bytesperline = ALIGN(w * 2, 64);

	/* publish into pix, since we now carry these in-state */
	v->pix.bytesperline = bytesperline;
	v->pix.sizeimage = bytesperline * lines;
	v->pix.half_size = v->pix.sizeimage / 2;	/* if HW uses halves */
	v->pix.field = interlaced ? V4L2_FIELD_INTERLACED : V4L2_FIELD_NONE;

	return v->pix.sizeimage;
}

static int hws_queue_setup(struct vb2_queue *q, unsigned int *num_buffers,
			   unsigned int *nplanes, unsigned int sizes[],
			   struct device *alloc_devs[])
{
	struct hws_video *vid = q->drv_priv;
	struct hws_pcie_dev *hws = vid->parent;
	size_t need_min = vid->pix.sizeimage;
	size_t need_alloc = PAGE_ALIGN(vid->pix.sizeimage);
	unsigned int have;
	unsigned int room;

	if (!need_min) {
		vid->pix.bytesperline = ALIGN(vid->pix.width * 2, 64);
		vid->pix.sizeimage = vid->pix.bytesperline * vid->pix.height;
		need_min = vid->pix.sizeimage;
		need_alloc = PAGE_ALIGN(vid->pix.sizeimage);
	}

	if (*nplanes) {
		if (*nplanes != 1)
			return -EINVAL;
		if (!sizes[0])
			sizes[0] = need_min;	/* publish minimal, not page-aligned */
		if (sizes[0] < need_min)
			return -EINVAL;
		vid->alloc_sizeimage = need_alloc;	/* keep internal aligned size */
	} else {
		*nplanes = 1;
		sizes[0] = need_min;	/* report minimal requirement */
		vid->alloc_sizeimage = need_alloc;
	}

	if (alloc_devs)
		alloc_devs[0] = &hws->pdev->dev;	/* vb2-dma-contig device */

	/* Make sure we have a reasonable minimum queue depth. */
	if (*num_buffers < 1)
		*num_buffers = 1;

	have = vb2_get_num_buffers(q);	/* instead of q->num_buffers */
	room = (have < HWS_MAX_BUFS) ? (HWS_MAX_BUFS - have) : 0;

	if (*num_buffers > room)
		*num_buffers = room;
	if (*num_buffers == 0) {
		dev_dbg(&hws->pdev->dev,
			"queue_setup: reject, no room (have=%u, max=%u)\n", have,
			HWS_MAX_BUFS);
		return -ENOBUFS;	/* or -ENOMEM; either is fine for CREATE_BUFS clamp */
	}
	return 0;
}

static int hws_buffer_prepare(struct vb2_buffer *vb)
{
	struct hws_video *vid = vb->vb2_queue->drv_priv;
	struct hws_pcie_dev *hws = vid->parent;
	size_t need = vid->pix.sizeimage;
	dma_addr_t dma_addr;

	if (vb2_plane_size(vb, 0) < need)
		return -EINVAL;

	/* Validate DMA address alignment */
	dma_addr = vb2_dma_contig_plane_dma_addr(vb, 0);
	if (dma_addr & 0x3F) {	/* 64-byte alignment required */
		dev_err(&hws->pdev->dev,
			"Buffer DMA address 0x%llx not 64-byte aligned\n",
			(unsigned long long)dma_addr);
		return -EINVAL;
	}

	vb2_set_plane_payload(vb, 0, need);
	return 0;
}

static void hws_buffer_queue(struct vb2_buffer *vb)
{
	struct hws_video *vid = vb->vb2_queue->drv_priv;
	struct hwsvideo_buffer *buf = to_hwsbuf(vb);
	struct vb2_buffer *vb2_buf = &buf->vb.vb2_buf;
	struct hws_pcie_dev *hws = vid->parent;
	unsigned long flags;
	bool ring_mode;

	dev_dbg(&hws->pdev->dev,
		"buffer_queue(ch=%u): vb=%p sizeimage=%u q_active=%d\n",
		vid->channel_index, vb, vid->pix.sizeimage,
		READ_ONCE(vid->cap_active));

	/* Initialize buffer slot */
	buf->slot = 0;
	ring_mode = hws_use_ring(vid);

	spin_lock_irqsave(&vid->irq_lock, flags);
	list_add_tail(&buf->list, &vid->capture_queue);
	vid->queued_count++;

	/* If streaming and no in-flight buffer, prime HW immediately */
	if (READ_ONCE(vid->cap_active) && !vid->active) {
		dev_dbg(&hws->pdev->dev,
			"buffer_queue(ch=%u): priming first vb=%p\n",
			vid->channel_index, vb2_buf);
		list_del_init(&buf->list);
		vid->queued_count--;
		vid->active = buf;

		if (ring_mode && vid->ring_cpu) {
			hws_program_dma_window(vid, vid->ring_dma);
		} else {
			dma_addr_t dma_addr;

			if (ring_mode && !vid->ring_cpu)
				dev_warn(&hws->pdev->dev,
					 "buffer_queue(ch=%u): ring buffer missing, using direct mode\n",
					 vid->channel_index);

			dma_addr = vb2_dma_contig_plane_dma_addr(vb2_buf, 0);
			hws_program_dma_for_addr(vid->parent,
						 vid->channel_index,
						 dma_addr);
			iowrite32(lower_32_bits(dma_addr),
				  hws->bar0_base +
				  HWS_REG_DMA_ADDR(vid->channel_index));
			ring_mode = false;
		}

		if (ring_mode)
			hws_set_dma_doorbell(hws, vid->channel_index,
					     vid->ring_dma,
					     "buffer_queue_ring");

		wmb(); /* ensure descriptors visible before enabling capture */
		hws_enable_video_capture(hws, vid->channel_index, true);
	}
	spin_unlock_irqrestore(&vid->irq_lock, flags);
}

static int hws_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct hws_video *v = q->drv_priv;
	struct hws_pcie_dev *hws = v->parent;
	struct hwsvideo_buffer *to_program = NULL;	/* local copy */
	struct vb2_buffer *prog_vb2 = NULL;
	unsigned long flags;
	int ret;
	bool ring_mode;

	dev_dbg(&hws->pdev->dev, "start_streaming: ch=%u count=%u\n",
		v->channel_index, count);

	ret = hws_check_card_status(hws);
	if (ret)
		return ret;
	(void)hws_update_active_interlace(hws, v->channel_index);

	ring_mode = hws_use_ring(v);

	mutex_lock(&v->state_lock);
	/* init per-stream state */
	WRITE_ONCE(v->stop_requested, false);
	WRITE_ONCE(v->cap_active, true);
	WRITE_ONCE(v->half_seen, false);
	WRITE_ONCE(v->last_buf_half_toggle, 0);
	mutex_unlock(&v->state_lock);

	/* Try to prime a buffer, but it's OK if none are queued yet */
	spin_lock_irqsave(&v->irq_lock, flags);
	if (!v->active && !list_empty(&v->capture_queue)) {
		to_program = list_first_entry(&v->capture_queue,
					      struct hwsvideo_buffer, list);
		list_del(&to_program->list);
		v->queued_count--;
		v->active = to_program;
		prog_vb2 = &to_program->vb.vb2_buf;
		dev_dbg(&hws->pdev->dev,
			"start_streaming: ch=%u took buffer %p\n",
			v->channel_index, to_program);
	}
	spin_unlock_irqrestore(&v->irq_lock, flags);

	/* Only program/enable HW if we actually have a buffer */
	if (to_program) {
		if (!prog_vb2)
			prog_vb2 = &to_program->vb.vb2_buf;

		if (ring_mode) {
			ret = hws_ring_setup(v);
			if (ret) {
				dev_warn(&hws->pdev->dev,
					 "start_streaming: ch=%u ring setup failed (%d), switching to direct mode\n",
					 v->channel_index, ret);
				ring_mode = false;
			}
		}

		if (ring_mode) {
			hws_program_dma_window(v, v->ring_dma);
		} else {
			dma_addr_t dma_addr;

			dma_addr = vb2_dma_contig_plane_dma_addr(prog_vb2, 0);
			hws_program_dma_for_addr(hws, v->channel_index, dma_addr);
			iowrite32(lower_32_bits(dma_addr),
				  hws->bar0_base +
				  HWS_REG_DMA_ADDR(v->channel_index));
			dev_dbg(&hws->pdev->dev,
				"start_streaming: ch=%u programmed buffer %p dma=0x%08x\n",
				v->channel_index, to_program,
				lower_32_bits(dma_addr));
			(void)readl(hws->bar0_base + HWS_REG_INT_STATUS);
		}

		if (ring_mode) {
			hws_set_dma_doorbell(hws, v->channel_index,
					     v->ring_dma,
					     "start_streaming_ring");
			dev_dbg(&hws->pdev->dev,
				"start_streaming: ch=%u ring mode active\n",
				v->channel_index);
		}

		wmb(); /* ensure descriptors visible before enabling capture */
		hws_enable_video_capture(hws, v->channel_index, true);
	} else {
		dev_dbg(&hws->pdev->dev,
			"start_streaming: ch=%u no buffer yet (will arm on QBUF)\n",
			v->channel_index);
	}

	return 0;
}

static inline bool list_node_unlinked(const struct list_head *n)
{
	return n->next == LIST_POISON1 || n->prev == LIST_POISON2;
}

static void hws_stop_streaming(struct vb2_queue *q)
{
	struct hws_video *v = q->drv_priv;
	unsigned long flags;
	struct hwsvideo_buffer *b, *tmp;
	LIST_HEAD(done);

	/* 1) Quiesce SW/HW first */
	mutex_lock(&v->state_lock);
	WRITE_ONCE(v->cap_active, false);
	WRITE_ONCE(v->stop_requested, true);
	mutex_unlock(&v->state_lock);

	hws_enable_video_capture(v->parent, v->channel_index, false);

	/* Release ring buffer if allocated */
	hws_ring_release(v);

	/* 2) Collect in-flight + queued under the IRQ lock */
	spin_lock_irqsave(&v->irq_lock, flags);

	if (v->active) {
		/*
		 * v->active may not be on any list (only referenced by v->active).
		 * Only move it if its list node is still linked somewhere.
		 */
		if (!list_node_unlinked(&v->active->list)) {
			/* Move directly to 'done' in one safe op */
			list_move_tail(&v->active->list, &done);
		} else {
			/* Not on a list: put list node into a known state for later reuse */
			INIT_LIST_HEAD(&v->active->list);
			/*
			 * We'll complete it below without relying on list pointers.
			 * To unify flow, push it via a temporary single-element list.
			 */
			list_add_tail(&v->active->list, &done);
		}
		v->active = NULL;
	}

	while (!list_empty(&v->capture_queue)) {
		b = list_first_entry(&v->capture_queue, struct hwsvideo_buffer,
				     list);
		/* Move (not del+add) to preserve invariants and avoid touching poisons */
		list_move_tail(&b->list, &done);
	}

	spin_unlock_irqrestore(&v->irq_lock, flags);

	/* 3) Complete outside the lock */
	list_for_each_entry_safe(b, tmp, &done, list) {
		/* Unlink from 'done' before completing */
		list_del_init(&b->list);
		vb2_buffer_done(&b->vb.vb2_buf, VB2_BUF_STATE_ERROR);
	}
}

static const struct vb2_ops hwspcie_video_qops = {
	.queue_setup = hws_queue_setup,
	.buf_prepare = hws_buffer_prepare,
	.buf_init = hws_buf_init,
	.buf_finish = hws_buf_finish,
	.buf_cleanup = hws_buf_cleanup,
	/* .buf_finish = hws_buffer_finish, */
	.buf_queue = hws_buffer_queue,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
	.start_streaming = hws_start_streaming,
	.stop_streaming = hws_stop_streaming,
};

int hws_video_register(struct hws_pcie_dev *dev)
{
	int i, ret;

	ret = v4l2_device_register(&dev->pdev->dev, &dev->v4l2_device);
	if (ret) {
		dev_err(&dev->pdev->dev, "v4l2_device_register failed: %d\n",
			ret);
		return ret;
	}

	for (i = 0; i < dev->cur_max_video_ch; i++) {
		struct hws_video *ch = &dev->video[i];
		struct video_device *vdev;
		struct vb2_queue *q;

		/* hws_video_init_channel() should have set:
		 * - ch->parent, ch->channel_index
		 * - locks (state_lock, irq_lock)
		 * - capture_queue (INIT_LIST_HEAD)
		 * - control_handler + controls
		 * - fmt_curr (width/height)
		 * Don’t reinitialize any of those here.
		 */

		vdev = video_device_alloc();
		if (!vdev) {
			dev_err(&dev->pdev->dev,
				"video_device_alloc ch%u failed\n", i);
			ret = -ENOMEM;
			goto err_unwind;
		}
		ch->video_device = vdev;

		/* Basic V4L2 node setup */
		snprintf(vdev->name, sizeof(vdev->name), "%s-hdmi%u",
			 KBUILD_MODNAME, i);
		vdev->v4l2_dev = &dev->v4l2_device;
		vdev->fops = &hws_fops;	/* your file_ops */
		vdev->ioctl_ops = &hws_ioctl_fops;	/* your ioctl_ops */
		vdev->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
		vdev->lock = &ch->state_lock;	/* serialize file ops */
		vdev->ctrl_handler = &ch->control_handler;
		vdev->vfl_dir = VFL_DIR_RX;
		vdev->release = video_device_release_empty;
		if (ch->control_handler.error)
			goto err_unwind;
		video_set_drvdata(vdev, ch);

		/* vb2 queue init (dma-contig) */
		q = &ch->buffer_queue;
		memset(q, 0, sizeof(*q));
		q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		q->io_modes = VB2_MMAP | VB2_DMABUF;
		q->drv_priv = ch;
		q->buf_struct_size = sizeof(struct hwsvideo_buffer);
		q->ops = &hwspcie_video_qops;	/* your vb2_ops */
		q->mem_ops = &vb2_dma_contig_memops;
		q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
		q->lock = &ch->qlock;
		q->max_num_buffers = HWS_MAX_BUFS;
		q->min_queued_buffers = 1;
		q->min_reqbufs_allocation = 1;
		q->dev = &dev->pdev->dev;

		ret = vb2_queue_init(q);
		vdev->queue = q;
		if (ret) {
			dev_err(&dev->pdev->dev,
				"vb2_queue_init ch%u failed: %d\n", i, ret);
			goto err_unwind;
		}

		/* Make controls live (no-op if none or already set up) */
		if (ch->control_handler.error) {
			ret = ch->control_handler.error;
			dev_err(&dev->pdev->dev,
				"ctrl handler ch%u error: %d\n", i, ret);
			goto err_unwind;
		}
		v4l2_ctrl_handler_setup(&ch->control_handler);
		ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
		if (ret) {
			dev_err(&dev->pdev->dev,
				"video_register_device ch%u failed: %d\n", i,
				ret);
			goto err_unwind;
		}
	}

	return 0;

err_unwind:
	for (i = i - 1; i >= 0; i--) {
		struct hws_video *ch = &dev->video[i];

		if (video_is_registered(ch->video_device))
			video_unregister_device(ch->video_device);

		if (ch->buffer_queue.ops)
			vb2_queue_release(&ch->buffer_queue);
		v4l2_ctrl_handler_free(&ch->control_handler);
		if (ch->video_device) {
			/* If not registered, we must free the alloc’d vdev ourselves */
			if (!video_is_registered(ch->video_device))
				video_device_release(ch->video_device);
			ch->video_device = NULL;
		}
	}
	v4l2_device_unregister(&dev->v4l2_device);
	return ret;
}

void hws_video_unregister(struct hws_pcie_dev *dev)
{
	int i;

	if (!dev)
		return;

	for (i = 0; i < dev->cur_max_video_ch; i++) {
		struct hws_video *ch = &dev->video[i];
		unsigned long flags;

		/* 1) Stop hardware capture for this channel (if running). */
		if (ch->cap_active)
			hws_enable_video_capture(dev, i, false);

		/* 2) Drain SW queue + complete in-flight buffer as ERROR. */
		spin_lock_irqsave(&ch->irq_lock, flags);

		if (ch->active) {
			vb2_buffer_done(&ch->active->vb.vb2_buf,
					VB2_BUF_STATE_ERROR);
			ch->active = NULL;
		}
		while (!list_empty(&ch->capture_queue)) {
			struct hwsvideo_buffer *b =
			    list_first_entry(&ch->capture_queue,
					     struct hwsvideo_buffer, list);
			list_del(&b->list);
			vb2_buffer_done(&b->vb.vb2_buf, VB2_BUF_STATE_ERROR);
		}

		spin_unlock_irqrestore(&ch->irq_lock, flags);

		/* 3) Release vb2 queue (safe to call once if it was inited). */
		if (ch->buffer_queue.ops)
			vb2_queue_release(&ch->buffer_queue);

		/* 4) Free V4L2 controls. */
		v4l2_ctrl_handler_free(&ch->control_handler);

		/* 5) Unregister the video node (if it was registered). */
		if (ch->video_device) {
			if (video_is_registered(ch->video_device))
				video_unregister_device(ch->video_device);
			else
				video_device_release(ch->video_device);
			ch->video_device = NULL;
		}
		/* 6) Reset lightweight state (optional). */
		ch->cap_active = false;
		ch->stop_requested = false;
		ch->last_buf_half_toggle = 0;
		ch->half_seen = false;
		ch->signal_loss_cnt = 0;
		INIT_LIST_HEAD(&ch->capture_queue);
	}
	v4l2_device_unregister(&dev->v4l2_device);
}

int hws_video_pm_suspend(struct hws_pcie_dev *hws)
{
	int i, ret = 0;

	for (i = 0; i < hws->cur_max_video_ch; i++) {
		struct hws_video *vid = &hws->video[i];
		struct vb2_queue *q = &vid->buffer_queue;

		if (!q || !q->ops)
			continue;
		if (vb2_is_streaming(q)) {
			/* Stop via vb2 (runs your .stop_streaming) */
			int r = vb2_streamoff(q, q->type);

			if (r && !ret)
				ret = r;
		}
	}
	return ret;
}

void hws_video_pm_resume(struct hws_pcie_dev *hws)
{
	/* Nothing mandatory to do here for vb2 — userspace will STREAMON again.
	 * If you track per-channel 'auto-restart' policy, re-arm it here.
	 */
}
