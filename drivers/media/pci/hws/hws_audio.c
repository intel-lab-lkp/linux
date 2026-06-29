// SPDX-License-Identifier: GPL-2.0-only
#include "hws_audio.h"

#include "hws.h"
#include "hws_reg.h"

#include <sound/core.h>
#include <sound/pcm_params.h>
#include <sound/control.h>
#include <sound/pcm.h>
#include <sound/rawmidi.h>
#include <sound/initval.h>
#include <linux/ktime.h>
#include <linux/preempt.h>
#include "hws_video.h"

static inline void hws_audio_ack_pending(struct hws_pcie_dev *hws,
					 unsigned int ch);
static void hws_audio_disable_capture_and_ack(struct hws_pcie_dev *hws,
					      unsigned int ch);
static void hws_audio_clear_pending(struct hws_audio *a);
static void hws_audio_deliver_work(struct work_struct *work);
static void hws_audio_drain_channel_work(struct hws_audio *a);
static void hws_audio_discard_stale_done(struct hws_pcie_dev *hws,
					 struct hws_audio *a,
					 unsigned int ch);

static void hws_audio_reset_ring_state(struct hws_audio *a)
{
	unsigned long flags;

	if (!a)
		return;

	spin_lock_irqsave(&a->ring_lock, flags);
	a->ring_size_byframes = 0;
	a->ring_wpos_byframes = 0;
	a->period_size_byframes = 0;
	a->period_used_byframes = 0;
	a->frame_bytes = 0;
	spin_unlock_irqrestore(&a->ring_lock, flags);
}

static void hws_audio_reset_counters(struct hws_audio *a)
{
	if (!a)
		return;

	WRITE_ONCE(a->last_period_toggle, 0xFF);
	WRITE_ONCE(a->irq_count, 0);
	WRITE_ONCE(a->delivered_count, 0);
	WRITE_ONCE(a->dropped_packets, 0);
}

static void hws_audio_reset_runtime_state(struct hws_audio *a)
{
	if (!a)
		return;

	hws_audio_clear_pending(a);
	hws_audio_reset_ring_state(a);
	hws_audio_reset_counters(a);
}

static bool hws_audio_publish_stopped(struct hws_audio *a)
{
	unsigned long flags;
	bool was_running;

	if (!a)
		return false;

	spin_lock_irqsave(&a->ring_lock, flags);
	was_running = READ_ONCE(a->stream_running) ||
		      READ_ONCE(a->cap_active);
	WRITE_ONCE(a->stream_running, false);
	WRITE_ONCE(a->cap_active, false);
	WRITE_ONCE(a->stop_requested, true);
	spin_unlock_irqrestore(&a->ring_lock, flags);
	/*
	 * IRQ handlers test these flags before touching scratch buffers or
	 * ALSA pointers. Publish the no-stream state before ACAP is disabled
	 * and before any teardown clears pcm_substream.
	 */
	smp_wmb();
	return was_running;
}

static void hws_audio_quiesce_capture(struct hws_pcie_dev *hws,
				      unsigned int ch, bool sync_irq)
{
	struct hws_audio *a;

	if (!hws || ch >= hws->cur_max_audio_ch)
		return;

	a = &hws->audio[ch];
	hws_audio_publish_stopped(a);

	hws_audio_disable_capture_and_ack(hws, ch);

	if (sync_irq && hws->irq >= 0 && !in_interrupt())
		synchronize_irq(hws->irq);

	if (!in_interrupt())
		hws_audio_drain_channel_work(a);

	hws_audio_reset_runtime_state(a);
}

#define HWS_AUDIO_PACKET_BYTES      MAX_DMA_AUDIO_PK_SIZE
#define HWS_AUDIO_PERIODS_MIN       4U
#define HWS_AUDIO_PERIODS_MAX       16U
#define HWS_AUDIO_PERIOD_BYTES_MAX  (HWS_AUDIO_PACKET_BYTES * 4U)
#define HWS_AUDIO_BUFFER_BYTES_MAX  (HWS_AUDIO_PACKET_BYTES * HWS_AUDIO_PERIODS_MAX)

/*
 * Audio DMA completes in fixed-size packets. The driver copies whole packets
 * into ALSA's ring, so expose packet-sized period and buffer granularity.
 */
static const struct snd_pcm_hardware audio_pcm_hardware = {
	.info = (SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_INTERLEAVED |
		 SNDRV_PCM_INFO_BLOCK_TRANSFER | SNDRV_PCM_INFO_RESUME |
		 SNDRV_PCM_INFO_MMAP_VALID),
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = SNDRV_PCM_RATE_48000,
	.rate_min = 48000,
	.rate_max = 48000,
	.channels_min = 2,
	.channels_max = 2,
	.buffer_bytes_max = HWS_AUDIO_BUFFER_BYTES_MAX,
	.period_bytes_min = HWS_AUDIO_PACKET_BYTES,
	.period_bytes_max = HWS_AUDIO_PERIOD_BYTES_MAX,
	.periods_min = HWS_AUDIO_PERIODS_MIN,
	.periods_max = HWS_AUDIO_PERIODS_MAX,
};

static bool hws_audio_select_buffer(struct hws_pcie_dev *hws, unsigned int ch,
				    void **cpu_base, dma_addr_t *dma_base,
				    size_t *size)
{
	struct hws_scratch_dma *scratch;

	if (!hws || ch >= hws->cur_max_audio_ch)
		return false;

	scratch = &hws->scratch_aud[ch];
	if (!scratch->cpu || !scratch->size)
		return false;

	if (cpu_base)
		*cpu_base = scratch->cpu;
	if (dma_base)
		*dma_base = scratch->dma;
	if (size)
		*size = scratch->size;
	return true;
}

static int hws_guard_audio_video_remap_page(struct hws_pcie_dev *hws,
					    unsigned int ch)
{
	struct hws_video *vid;
	dma_addr_t audio_dma;
	u32 audio_hi, audio_page;

	if (!hws || ch >= hws->cur_max_audio_ch)
		return -EINVAL;
	if (ch >= hws->cur_max_video_ch)
		return 0;

	vid = &hws->video[ch];
	if (!READ_ONCE(vid->cap_active) || !vid->window_valid)
		return 0;

	if (!hws_audio_select_buffer(hws, ch, NULL, &audio_dma, NULL))
		return -ENOMEM;

	audio_hi = upper_32_bits(audio_dma);
	audio_page = lower_32_bits(audio_dma) & PCI_E_BAR_ADD_MASK;
	if (audio_hi == vid->last_dma_hi && audio_page == vid->last_dma_page)
		return 0;

	dev_warn_ratelimited(&hws->pdev->dev,
			     "audio ch%u DMA page differs from active video remap slot; refusing shared-window conflict (audio=%pad video_hi=0x%08x video_page=0x%08x)\n",
			     ch, &audio_dma, vid->last_dma_hi,
			     vid->last_dma_page);
	return -EBUSY;
}

static void hws_audio_program_remap_slot(struct hws_pcie_dev *hws,
					 u32 table_off, u32 hi, u32 page_lo)
{
	writel_relaxed(hi, hws->bar0_base + PCI_ADDR_TABLE_BASE + table_off);
	writel_relaxed(page_lo, hws->bar0_base + PCI_ADDR_TABLE_BASE + table_off +
		       PCIE_BARADDROFSIZE);
}

static int hws_audio_seed_capture_buffer(struct hws_pcie_dev *hws, unsigned int ch)
{
	dma_addr_t dma;
	u32 lo, hi, pci_addr;
	u32 audio_table_off;

	if (!hws || ch >= hws->cur_max_audio_ch)
		return -EINVAL;

	if (!hws_audio_select_buffer(hws, ch, NULL, &dma, NULL))
		return -ENOMEM;

	lo = lower_32_bits(dma);
	hi = upper_32_bits(dma);
	pci_addr = lo & PCI_E_BAR_ADD_LOWMASK;
	lo &= PCI_E_BAR_ADD_MASK;
	audio_table_off = HWS_AUDIO_REMAP_SLOT_OFF(ch);
	hws_audio_program_remap_slot(hws, audio_table_off, hi, lo);
	writel_relaxed((ch + 1u) * PCIEBAR_AXI_BASE + pci_addr,
		       hws->bar0_base + HWS_REG_AUD_DMA_ADDR(ch));
	(void)readl(hws->bar0_base + HWS_REG_AUD_DMA_ADDR(ch));
	return 0;
}

void hws_audio_seed_channels(struct hws_pcie_dev *hws)
{
	unsigned int ch;

	if (!hws || !hws->bar0_base)
		return;

	for (ch = 0; ch < hws->cur_max_audio_ch; ch++) {
		int ret;

		if (!hws->scratch_aud[ch].cpu)
			continue;

		ret = hws_audio_seed_capture_buffer(hws, ch);
		if (ret)
			dev_warn(&hws->pdev->dev,
				 "audio seed ch%u failed ret=%d\n", ch, ret);
	}
}

static size_t hws_audio_packet_offset(const struct hws_audio *a, u8 cur_toggle)
{
	size_t packet = a->hw_packet_bytes;

	/*
	 * ABUF_TOGGLE reports the half the device is filling now, so the
	 * completed packet is the other half.
	 */
	return cur_toggle ? 0 : packet;
}

static void hws_audio_clear_pending(struct hws_audio *a)
{
	unsigned long flags;

	if (!a)
		return;

	spin_lock_irqsave(&a->pending_lock, flags);
	a->packet_pending = false;
	a->xrun_pending = false;
	a->pending_irq_ns = 0;
	spin_unlock_irqrestore(&a->pending_lock, flags);
}

static void hws_audio_report_xrun(struct hws_audio *a)
{
	struct hws_pcie_dev *hws;
	struct snd_pcm_substream *ss;
	unsigned long flags;
	unsigned int ch;

	if (!a)
		return;

	hws = a->parent;
	ch = a->channel_index;
	ss = READ_ONCE(a->pcm_substream);

	hws_audio_publish_stopped(a);
	hws_audio_disable_capture_and_ack(hws, ch);
	hws_audio_clear_pending(a);

	if (!ss)
		return;

	snd_pcm_stream_lock_irqsave(ss, flags);
	if (ss->runtime && READ_ONCE(a->pcm_substream) == ss)
		snd_pcm_stop(ss, SNDRV_PCM_STATE_XRUN);
	snd_pcm_stream_unlock_irqrestore(ss, flags);
}

static void hws_audio_drain_channel_work(struct hws_audio *a)
{
	if (!a)
		return;

	if (!in_interrupt())
		cancel_work_sync(&a->deliver_work);
	hws_audio_clear_pending(a);
}

static int hws_audio_acquire_scratch(struct hws_audio *a)
{
	struct hws_pcie_dev *hws;
	unsigned int ch;
	int ret;

	if (!a || !a->parent)
		return -EINVAL;

	mutex_lock(&a->scratch_state_lock);
	if (READ_ONCE(a->scratch_acquired)) {
		mutex_unlock(&a->scratch_state_lock);
		return 0;
	}

	hws = a->parent;
	ch = a->channel_index;
	ret = hws_alloc_channel_scratch(hws, ch);
	if (ret) {
		mutex_unlock(&a->scratch_state_lock);
		return ret;
	}

	WRITE_ONCE(a->scratch_acquired, true);
	mutex_unlock(&a->scratch_state_lock);
	return 0;
}

static void hws_audio_release_scratch(struct hws_audio *a)
{
	struct hws_pcie_dev *hws;
	unsigned int ch;

	if (!a)
		return;

	mutex_lock(&a->scratch_state_lock);
	if (!a->scratch_acquired) {
		mutex_unlock(&a->scratch_state_lock);
		return;
	}

	WRITE_ONCE(a->scratch_acquired, false);
	hws = a->parent;
	ch = a->channel_index;
	mutex_unlock(&a->scratch_state_lock);

	if (hws)
		hws_release_channel_scratch(hws, ch);
}

static bool hws_audio_deliver_packet(struct hws_audio *a, const void *src)
{
	struct snd_pcm_substream *ss;
	struct snd_pcm_runtime *rt;
	snd_pcm_uframes_t frames, ring_pos, ring_frames, period_frames;
	size_t frame_bytes, packet_bytes, ring_bytes, first;
	unsigned long flags;
	unsigned int elapsed = 0;
	bool delivered = false;
	char *dst;

	if (!READ_ONCE(a->stream_running) || !READ_ONCE(a->cap_active) ||
	    READ_ONCE(a->stop_requested))
		return false;

	ss = READ_ONCE(a->pcm_substream);
	if (!ss)
		return false;

	rt = ss->runtime;
	if (!rt || !rt->dma_area)
		return false;

	spin_lock_irqsave(&a->ring_lock, flags);
	if (!READ_ONCE(a->stream_running) || !READ_ONCE(a->cap_active) ||
	    READ_ONCE(a->stop_requested) ||
	    READ_ONCE(a->pcm_substream) != ss) {
		spin_unlock_irqrestore(&a->ring_lock, flags);
		return false;
	}

	frame_bytes = a->frame_bytes;
	packet_bytes = a->hw_packet_bytes;
	ring_frames = a->ring_size_byframes;
	period_frames = a->period_size_byframes;
	if (!frame_bytes || !packet_bytes || !ring_frames || !period_frames)
		goto out_unlock;
	if (packet_bytes % frame_bytes)
		goto out_unlock;

	frames = packet_bytes / frame_bytes;
	if (!frames)
		goto out_unlock;

	ring_pos = a->ring_wpos_byframes;
	ring_bytes = ring_frames * frame_bytes;
	dst = rt->dma_area + ring_pos * frame_bytes;
	first = min(packet_bytes, ring_bytes - ring_pos * frame_bytes);
	memcpy(dst, src, first);
	if (first < packet_bytes)
		memcpy(rt->dma_area, (const char *)src + first, packet_bytes - first);
	delivered = true;

	ring_pos += frames;
	if (ring_pos >= ring_frames)
		ring_pos %= ring_frames;
	a->ring_wpos_byframes = ring_pos;

	a->period_used_byframes += frames;
	while (a->period_used_byframes >= period_frames) {
		a->period_used_byframes -= period_frames;
		elapsed++;
	}
out_unlock:
	spin_unlock_irqrestore(&a->ring_lock, flags);

	if (!READ_ONCE(a->stream_running) || !READ_ONCE(a->cap_active) ||
	    READ_ONCE(a->stop_requested))
		return delivered;

	while (elapsed--)
		snd_pcm_period_elapsed(ss);
	return delivered;
}

static bool hws_audio_packet_stale(struct hws_audio *a, u64 irq_ns)
{
	u64 packet_ns;
	size_t frame_bytes;
	u32 rate;
	u64 frames;

	if (!a || !irq_ns)
		return false;

	frame_bytes = READ_ONCE(a->frame_bytes);
	rate = READ_ONCE(a->output_sample_rate);
	if (!frame_bytes || !rate || a->hw_packet_bytes % frame_bytes)
		return false;

	frames = a->hw_packet_bytes / frame_bytes;
	if (!frames)
		return false;

	packet_ns = div_u64(frames * NSEC_PER_SEC, rate);
	return ktime_get_mono_fast_ns() - irq_ns >= packet_ns;
}

static void hws_audio_deliver_one_packet(struct hws_audio *a, u8 cur_toggle)
{
	struct hws_pcie_dev *hws;
	unsigned int ch;
	void *cpu;
	size_t size;
	size_t offset;

	if (!a)
		return;

	hws = a->parent;
	ch = a->channel_index;
	if (!hws || ch >= hws->cur_max_audio_ch)
		return;

	if (!READ_ONCE(a->stream_running) || !READ_ONCE(a->cap_active) ||
	    READ_ONCE(a->stop_requested))
		return;

	if (!hws_audio_select_buffer(hws, ch, &cpu, NULL, &size))
		return;

	offset = hws_audio_packet_offset(a, cur_toggle);
	if (offset + a->hw_packet_bytes > size)
		return;

	dma_rmb();
	if (hws_audio_deliver_packet(a, (char *)cpu + offset))
		WRITE_ONCE(a->delivered_count,
			   READ_ONCE(a->delivered_count) + 1);
}

static void hws_audio_deliver_work(struct work_struct *work)
{
	struct hws_audio *a = container_of(work, struct hws_audio, deliver_work);
	unsigned long flags;
	u64 irq_ns;
	u8 toggle;

	for (;;) {
		spin_lock_irqsave(&a->pending_lock, flags);
		if (a->xrun_pending) {
			a->xrun_pending = false;
			a->packet_pending = false;
			a->pending_irq_ns = 0;
			spin_unlock_irqrestore(&a->pending_lock, flags);
			hws_audio_report_xrun(a);
			break;
		}
		if (!a->packet_pending) {
			spin_unlock_irqrestore(&a->pending_lock, flags);
			break;
		}
		toggle = a->pending_toggle;
		irq_ns = a->pending_irq_ns;
		a->packet_pending = false;
		a->pending_irq_ns = 0;
		spin_unlock_irqrestore(&a->pending_lock, flags);

		if (hws_audio_packet_stale(a, irq_ns)) {
			WRITE_ONCE(a->dropped_packets,
				   READ_ONCE(a->dropped_packets) + 1);
			hws_audio_report_xrun(a);
			break;
		}

		hws_audio_deliver_one_packet(a, toggle);
	}
}

void hws_audio_queue_interrupt(struct hws_pcie_dev *hws, unsigned int ch, u8 cur_toggle)
{
	struct workqueue_struct *wq;
	struct hws_audio *a;

	if (!hws || ch >= hws->cur_max_audio_ch)
		return;

	a = &hws->audio[ch];
	if (!READ_ONCE(a->stream_running) || !READ_ONCE(a->cap_active) ||
	    READ_ONCE(a->stop_requested))
		return;

	wq = READ_ONCE(hws->audio_wq);
	if (!wq) {
		WRITE_ONCE(a->dropped_packets,
			   READ_ONCE(a->dropped_packets) + 1);
		return;
	}

	WRITE_ONCE(a->last_period_toggle, cur_toggle);
	spin_lock(&a->pending_lock);
	if (a->packet_pending) {
		WRITE_ONCE(a->dropped_packets,
			   READ_ONCE(a->dropped_packets) + 1);
		a->xrun_pending = true;
	}
	a->pending_toggle = cur_toggle;
	a->pending_irq_ns = ktime_get_mono_fast_ns();
	a->packet_pending = true;
	WRITE_ONCE(a->irq_count, READ_ONCE(a->irq_count) + 1);
	spin_unlock(&a->pending_lock);

	queue_work(wq, &a->deliver_work);
}

int hws_audio_init_channel(struct hws_pcie_dev *pdev, int ch)
{
	struct hws_audio *aud;

	if (!pdev || ch < 0 || ch >= pdev->max_channels)
		return -EINVAL;

	aud = &pdev->audio[ch];
	memset(aud, 0, sizeof(*aud));     /* ok: no embedded locks yet */

	/* identity */
	aud->parent        = pdev;
	aud->channel_index = ch;
	spin_lock_init(&aud->ring_lock);
	spin_lock_init(&aud->pending_lock);
	mutex_init(&aud->scratch_state_lock);
	INIT_WORK(&aud->deliver_work, hws_audio_deliver_work);

	/* defaults */
	aud->output_sample_rate = 48000;
	aud->channel_count      = 2;
	aud->bits_per_sample    = 16;
	aud->hw_packet_bytes    = pdev->audio_pkt_size;

	/* ALSA linkage */
	WRITE_ONCE(aud->pcm_substream, NULL);

	/* stream state */
	WRITE_ONCE(aud->cap_active, false);
	WRITE_ONCE(aud->stream_running, false);
	WRITE_ONCE(aud->stop_requested, false);
	WRITE_ONCE(aud->scratch_acquired, false);

	hws_audio_reset_counters(aud);

	return 0;
}

void hws_audio_cleanup_channel(struct hws_pcie_dev *pdev, int ch, bool device_removal)
{
	struct hws_audio *aud;
	struct snd_pcm_substream *ss;

	if (!pdev || ch < 0 || ch >= pdev->cur_max_audio_ch)
		return;

	aud = &pdev->audio[ch];
	hws_audio_quiesce_capture(pdev, ch, true);

	/* If device is going away and stream was open, tell ALSA. */
	ss = READ_ONCE(aud->pcm_substream);
	if (device_removal && ss) {
		unsigned long flags;

		snd_pcm_stream_lock_irqsave(ss, flags);
		if (ss->runtime)
			snd_pcm_stop(ss, SNDRV_PCM_STATE_DISCONNECTED);
		snd_pcm_stream_unlock_irqrestore(ss, flags);
		WRITE_ONCE(aud->pcm_substream, NULL);
	}

	hws_audio_release_scratch(aud);
}

static inline bool hws_check_audio_capture(struct hws_pcie_dev *hws, unsigned int ch)
{
	u32 reg = readl(hws->bar0_base + HWS_REG_ACAP_ENABLE);

	return !!(reg & BIT(ch));
}

static int hws_audio_hw_ready(struct hws_pcie_dev *hws)
{
	u32 status;

	if (!hws || !hws->bar0_base)
		return -ENODEV;

	status = readl(hws->bar0_base + HWS_REG_SYS_STATUS);
	if (status == 0xFFFFFFFF) {
		hws->pci_lost = true;
		dev_err(&hws->pdev->dev, "PCIe device not responding\n");
		return -ENODEV;
	}

	if (!(status & BIT(0))) {
		dev_warn_ratelimited(&hws->pdev->dev,
				     "audio start refused while device is not ready (SYS_STATUS=0x%08x)\n",
				     status);
		return -EIO;
	}

	return 0;
}

static int hws_start_audio_capture(struct hws_pcie_dev *hws, unsigned int ch)
{
	struct hws_audio *a;
	int ret;

	if (!hws || ch >= hws->cur_max_audio_ch)
		return -EINVAL;
	a = &hws->audio[ch];

	/* Already running? Re-assert HW if needed. */
	if (READ_ONCE(a->stream_running)) {
		if (!hws_check_audio_capture(hws, ch)) {
			ret = hws_audio_hw_ready(hws);
			if (ret)
				return ret;
			ret = hws_guard_audio_video_remap_page(hws, ch);
			if (ret)
				return ret;
			ret = hws_audio_seed_capture_buffer(hws, ch);
			if (ret)
				return ret;
			WRITE_ONCE(a->cap_active, false);
			smp_wmb(); /* publish inactive before stale ADONE ack */
			hws_audio_discard_stale_done(hws, a, ch);
			WRITE_ONCE(a->cap_active, true);
			smp_wmb(); /* publish active before ACAP_ENABLE */
			hws_enable_audio_capture(hws, ch, true);
		}
		dev_dbg(&hws->pdev->dev, "audio ch%u already running (re-enabled)\n", ch);
		return 0;
	}

	if (!READ_ONCE(a->scratch_acquired))
		return -ENOMEM;

	ret = hws_audio_hw_ready(hws);
	if (ret)
		return ret;

	ret = hws_guard_audio_video_remap_page(hws, ch);
	if (ret)
		return ret;

	ret = hws_audio_seed_capture_buffer(hws, ch);
	if (ret)
		return ret;

	hws_audio_discard_stale_done(hws, a, ch);
	hws_audio_reset_counters(a);

	/*
	 * ADONE can fire as soon as capture is enabled. Discard any completion
	 * latched before this start, then publish the stream state before
	 * ACAP_ENABLE so the IRQ path accepts the first fresh packet.
	 */
	WRITE_ONCE(a->stop_requested, false);
	WRITE_ONCE(a->stream_running, true);
	WRITE_ONCE(a->cap_active, true);
	smp_wmb(); /* publish start state before ACAP_ENABLE */

	/* Kick HW */
	hws_enable_audio_capture(hws, ch, true);
	return 0;
}

static inline void hws_audio_ack_pending(struct hws_pcie_dev *hws, unsigned int ch)
{
	u32 abit = HWS_INT_ADONE_BIT(ch);
	u32 st;

	if (!hws || !hws->bar0_base || ch >= hws->cur_max_audio_ch)
		return;

	st = readl(hws->bar0_base + HWS_REG_INT_STATUS);

	if (st & abit) {
		writel(abit, hws->bar0_base + HWS_REG_INT_ACK);
		/* flush posted write */
		readl(hws->bar0_base + HWS_REG_INT_STATUS);
	}
}

static void hws_audio_discard_stale_done(struct hws_pcie_dev *hws,
					 struct hws_audio *a,
					 unsigned int ch)
{
	hws_audio_clear_pending(a);
	hws_audio_ack_pending(hws, ch);
}

static void hws_audio_disable_capture_and_ack(struct hws_pcie_dev *hws,
					      unsigned int ch)
{
	if (!hws || !hws->bar0_base || ch >= hws->cur_max_audio_ch)
		return;

	hws_enable_audio_capture(hws, ch, false);
	readl(hws->bar0_base + HWS_REG_INT_STATUS);
	hws_audio_ack_pending(hws, ch);
}

static inline void hws_audio_ack_all(struct hws_pcie_dev *hws)
{
	u32 mask = 0;

	if (!hws || !hws->bar0_base)
		return;

	for (unsigned int ch = 0; ch < hws->cur_max_audio_ch; ch++)
		mask |= HWS_INT_ADONE_BIT(ch);
	if (mask) {
		writel(mask, hws->bar0_base + HWS_REG_INT_ACK);
		readl(hws->bar0_base + HWS_REG_INT_STATUS);
	}
}

static void hws_stop_audio_capture(struct hws_pcie_dev *hws, unsigned int ch)
{
	struct hws_audio *a;

	if (!hws || ch >= hws->cur_max_audio_ch)
		return;

	a = &hws->audio[ch];
	if (!READ_ONCE(a->stream_running) && !READ_ONCE(a->cap_active))
		return;

	hws_audio_publish_stopped(a);
	hws_audio_disable_capture_and_ack(hws, ch);
	hws_audio_clear_pending(a);
	dev_dbg(&hws->pdev->dev, "audio capture stopped on ch %u\n", ch);
}

void hws_enable_audio_capture(struct hws_pcie_dev *hws,
			      unsigned int ch, bool enable)
{
	u32 reg, mask = BIT(ch);

	if (!hws || ch >= hws->cur_max_audio_ch || hws->pci_lost)
		return;

	reg = readl(hws->bar0_base + HWS_REG_ACAP_ENABLE);
	if (enable)
		reg |= mask;
	else
		reg &= ~mask;

	writel(reg, hws->bar0_base + HWS_REG_ACAP_ENABLE);

	dev_dbg(&hws->pdev->dev, "audio capture %s ch%u, reg=0x%08x\n",
		enable ? "enabled" : "disabled", ch, reg);
}

static snd_pcm_uframes_t hws_pcie_audio_pointer(struct snd_pcm_substream *substream)
{
	struct hws_audio *a = snd_pcm_substream_chip(substream);
	snd_pcm_uframes_t pos;
	unsigned long flags;

	spin_lock_irqsave(&a->ring_lock, flags);
	pos = a->ring_wpos_byframes;
	spin_unlock_irqrestore(&a->ring_lock, flags);
	return pos;
}

static int hws_pcie_audio_open(struct snd_pcm_substream *substream)
{
	struct hws_audio *a = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *rt = substream->runtime;
	int ret;

	rt->hw = audio_pcm_hardware;

	ret = snd_pcm_hw_constraint_integer(rt, SNDRV_PCM_HW_PARAM_PERIODS);
	if (ret < 0)
		return ret;
	ret = snd_pcm_hw_constraint_step(rt, 0, SNDRV_PCM_HW_PARAM_PERIOD_BYTES,
					 HWS_AUDIO_PACKET_BYTES);
	if (ret < 0)
		return ret;
	ret = snd_pcm_hw_constraint_step(rt, 0, SNDRV_PCM_HW_PARAM_BUFFER_BYTES,
					 HWS_AUDIO_PACKET_BYTES);
	if (ret < 0)
		return ret;

	WRITE_ONCE(a->pcm_substream, substream);
	return 0;
}

static int hws_pcie_audio_close(struct snd_pcm_substream *substream)
{
	struct hws_audio *a = snd_pcm_substream_chip(substream);

	hws_stop_audio_capture(a->parent, a->channel_index);
	hws_audio_drain_channel_work(a);
	hws_audio_reset_runtime_state(a);
	hws_audio_release_scratch(a);
	WRITE_ONCE(a->pcm_substream, NULL);
	return 0;
}

static int hws_pcie_audio_hw_params(struct snd_pcm_substream *substream,
				    struct snd_pcm_hw_params *hw_params)
{
	struct hws_audio *a = snd_pcm_substream_chip(substream);
	struct hws_pcie_dev *hws = a->parent;
	int pages_changed;
	int ret;

	if (!hws)
		return -ENODEV;

	ret = hws_check_card_status(hws);
	if (ret)
		return ret;

	pages_changed = snd_pcm_lib_malloc_pages(substream,
						 params_buffer_bytes(hw_params));
	if (pages_changed < 0)
		return pages_changed;

	ret = hws_audio_acquire_scratch(a);
	if (ret) {
		snd_pcm_lib_free_pages(substream);
		return ret;
	}

	ret = hws_guard_audio_video_remap_page(hws, a->channel_index);
	if (ret) {
		hws_audio_release_scratch(a);
		snd_pcm_lib_free_pages(substream);
		return ret;
	}

	return pages_changed;
}

static int hws_pcie_audio_hw_free(struct snd_pcm_substream *substream)
{
	struct hws_audio *a = snd_pcm_substream_chip(substream);
	int ret;

	hws_stop_audio_capture(a->parent, a->channel_index);
	hws_audio_drain_channel_work(a);
	hws_audio_reset_runtime_state(a);
	hws_audio_release_scratch(a);
	ret = snd_pcm_lib_free_pages(substream);
	return ret;
}

static int hws_pcie_audio_prepare(struct snd_pcm_substream *substream)
{
	struct hws_audio *a = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *rt = substream->runtime;
	unsigned long flags;
	size_t frame_bytes;

	frame_bytes = snd_pcm_format_physical_width(rt->format) / 8;
	frame_bytes *= rt->channels;
	if (!frame_bytes || a->hw_packet_bytes % frame_bytes)
		return -EINVAL;

	spin_lock_irqsave(&a->ring_lock, flags);
	a->ring_size_byframes = rt->buffer_size;
	a->ring_wpos_byframes = 0;
	a->period_size_byframes = rt->period_size;
	a->period_used_byframes = 0;
	a->frame_bytes = frame_bytes;
	spin_unlock_irqrestore(&a->ring_lock, flags);

	hws_audio_reset_counters(a);
	hws_audio_clear_pending(a);
	return 0;
}

static int hws_pcie_audio_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct hws_audio *a = snd_pcm_substream_chip(substream);
	struct hws_pcie_dev *hws = a->parent;
	unsigned int ch = a->channel_index;

	dev_dbg(&hws->pdev->dev, "audio trigger %d on ch %u\n", cmd, ch);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		return hws_start_audio_capture(hws, ch);
	case SNDRV_PCM_TRIGGER_STOP:
		hws_stop_audio_capture(hws, ch);
		return 0;
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		return hws_start_audio_capture(hws, ch);
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		hws_stop_audio_capture(hws, ch);
		return 0;
	default:
		return -EINVAL;
	}
}

static const struct snd_pcm_ops hws_pcie_pcm_ops = {
	.open      = hws_pcie_audio_open,
	.close     = hws_pcie_audio_close,
	.ioctl     = snd_pcm_lib_ioctl,
	.hw_params = hws_pcie_audio_hw_params,
	.hw_free   = hws_pcie_audio_hw_free,
	.prepare   = hws_pcie_audio_prepare,
	.trigger   = hws_pcie_audio_trigger,
	.pointer   = hws_pcie_audio_pointer,
};

int hws_audio_register(struct hws_pcie_dev *hws)
{
	struct snd_card *card = NULL;
	struct snd_pcm  *pcm  = NULL;
	char card_id[16];
	char card_name[64];
	int i, ret;

	if (!hws)
		return -EINVAL;
	if (!hws->cur_max_audio_ch)
		return 0;

	/* ---- Create a single ALSA card for this PCI function ---- */
	snprintf(card_id, sizeof(card_id), "hws%u", hws->port_id);     /* <=16 chars */
	snprintf(card_name, sizeof(card_name), "HWS HDMI Audio %u", hws->port_id);

	ret = snd_card_new(&hws->pdev->dev, -1 /* auto index */,
			   card_id, THIS_MODULE, 0, &card);
	if (ret < 0) {
		dev_err(&hws->pdev->dev, "snd_card_new failed: %d\n", ret);
		return ret;
	}

	snd_card_set_dev(card, &hws->pdev->dev);
	strscpy(card->driver,   KBUILD_MODNAME, sizeof(card->driver));
	strscpy(card->shortname, card_name,      sizeof(card->shortname));
	strscpy(card->longname,  card->shortname, sizeof(card->longname));

	/* ---- Create one PCM capture device per HDMI input ---- */
	for (i = 0; i < hws->cur_max_audio_ch; i++) {
		char pcm_name[32];

		snprintf(pcm_name, sizeof(pcm_name), "HDMI In %d", i);

		/* device number = i, so userspace sees hw:X,i */
		ret = snd_pcm_new(card, pcm_name, i,
				  0 /* playback */, 1 /* capture */, &pcm);
		if (ret < 0) {
			dev_err(&hws->pdev->dev, "snd_pcm_new(%d) failed: %d\n", i, ret);
			goto error_card;
		}

		pcm->private_data = &hws->audio[i];
		strscpy(pcm->name, pcm_name, sizeof(pcm->name));
		snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &hws_pcie_pcm_ops);

		/*
		 * snd_pcm_lib_malloc_pages() requires a valid DMA buffer type.
		 * Keep allocation dynamic at HW_PARAMS time, but advertise the
		 * maximum buffer size up front for modern ALSA.
		 */
		ret = snd_pcm_set_managed_buffer_all(pcm,
						     SNDRV_DMA_TYPE_DEV,
						     &hws->pdev->dev,
						     0,
						     audio_pcm_hardware.buffer_bytes_max);
		if (ret < 0) {
			dev_err(&hws->pdev->dev,
				"snd_pcm_set_managed_buffer_all(%d) failed: %d\n",
				i, ret);
			goto error_card;
		}
	}

	/* Register the card once all PCMs are created */
	ret = snd_card_register(card);
	if (ret < 0) {
		dev_err(&hws->pdev->dev, "snd_card_register failed: %d\n", ret);
		goto error_card;
	}

	/* Store the single card handle (optional: also mirror to each channel if you like) */
	hws->snd_card = card;
	dev_info(&hws->pdev->dev, "audio registration complete (%d HDMI inputs)\n",
		 hws->cur_max_audio_ch);
	return 0;

error_card:
	/* Frees all PCMs created on it as well */
	snd_card_free(card);
	return ret;
}

void hws_audio_unregister(struct hws_pcie_dev *hws)
{
	if (!hws)
		return;

	/* Prevent new opens and mark existing streams disconnected */
	if (hws->snd_card)
		snd_card_disconnect(hws->snd_card);

	for (unsigned int i = 0; i < hws->cur_max_audio_ch; i++) {
		struct hws_audio *a = &hws->audio[i];

		hws_audio_publish_stopped(a);
		hws_enable_audio_capture(hws, i, false);
	}

	/* Flush ACAP disables before waiting for any running IRQ handler. */
	if (hws->bar0_base)
		readl(hws->bar0_base + HWS_REG_INT_STATUS);
	if (hws->irq >= 0 && !in_interrupt())
		synchronize_irq(hws->irq);

	hws_audio_drain_work(hws);
	hws_audio_ack_all(hws);

	for (unsigned int i = 0; i < hws->cur_max_audio_ch; i++) {
		struct hws_audio *a = &hws->audio[i];
		struct snd_pcm_substream *ss = READ_ONCE(a->pcm_substream);

		if (ss) {
			unsigned long flags;

			snd_pcm_stream_lock_irqsave(ss, flags);
			if (ss->runtime)
				snd_pcm_stop(ss, SNDRV_PCM_STATE_DISCONNECTED);
			snd_pcm_stream_unlock_irqrestore(ss, flags);
		}

		WRITE_ONCE(a->pcm_substream, NULL);
		hws_audio_reset_runtime_state(a);
		hws_audio_release_scratch(a);
	}

	if (hws->snd_card) {
		snd_card_free_when_closed(hws->snd_card);
		hws->snd_card = NULL;
	}

	dev_info(&hws->pdev->dev, "audio unregistered (%u channels)\n",
		 hws->cur_max_audio_ch);
}

int hws_audio_pm_suspend_all(struct hws_pcie_dev *hws)
{
	struct snd_pcm *seen[ARRAY_SIZE(hws->audio)];
	int seen_cnt = 0;
	int i, j, ret = 0;

	if (!hws || !hws->snd_card)
		return 0;

	/* Iterate audio channels and suspend each unique PCM device */
	for (i = 0; i < hws->cur_max_audio_ch && i < ARRAY_SIZE(hws->audio); i++) {
		struct hws_audio *a = &hws->audio[i];
		struct snd_pcm_substream *ss = READ_ONCE(a->pcm_substream);
		struct snd_pcm *pcm;
		bool already = false;

		if (!ss)
			continue;

		pcm = ss->pcm;
		if (!pcm)
			continue;

		/* De-duplicate in case multiple channels share a PCM */
		for (j = 0; j < seen_cnt; j++) {
			if (seen[j] == pcm) {
				already = true;
				break;
			}
		}
		if (already)
			continue;

		if (seen_cnt < ARRAY_SIZE(seen))
			seen[seen_cnt++] = pcm;

		if (!ret) {
			int r = snd_pcm_suspend_all(pcm);

			if (r)
				ret = r;  /* remember first error, keep going */
		}

		if (seen_cnt == ARRAY_SIZE(seen))
			break; /* defensive: shouldn't happen with sane config */
	}

	return ret;
}

void hws_audio_pm_resume(struct hws_pcie_dev *hws)
{
	unsigned int ch;

	if (!hws || !hws->bar0_base)
		return;

	for (ch = 0; ch < hws->cur_max_audio_ch && ch < MAX_VID_CHANNELS; ch++) {
		struct hws_audio *a = &hws->audio[ch];

		WRITE_ONCE(a->stream_running, false);
		WRITE_ONCE(a->cap_active, false);
		WRITE_ONCE(a->stop_requested, true);
		hws_audio_reset_counters(a);
		hws_audio_clear_pending(a);
	}
	hws_audio_seed_channels(hws);
	hws_audio_ack_all(hws);
}

void hws_audio_drain_work(struct hws_pcie_dev *hws)
{
	unsigned int ch;

	if (!hws)
		return;

	for (ch = 0; ch < hws->cur_max_audio_ch && ch < MAX_VID_CHANNELS; ch++)
		hws_audio_drain_channel_work(&hws->audio[ch]);
}
