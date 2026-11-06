// SPDX-License-Identifier: GPL-2.0
/*
 * Frame streaming. The pixel data is sent as one bulk transfer per frame on
 * endpoint 1 OUT, followed by a zero-length packet that ends the frame, after
 * switching the AV control interface (#0) to altsetting 1. The hardware
 * exposes endpoint #1 on more than one interface, which the USB specification
 * only allows across alternate settings.
 *
 * The current frame lives in a shadow buffer, already in the device's pixel
 * format. Damage is converted into the shadow and nowhere else, and every
 * outgoing URB buffer is brought up to date from it just before it is sent,
 * with the cursor blended in on the way out.
 *
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2020, Artem Mygaiev
 */

#include <linux/iosys-map.h>
#include <linux/scatterlist.h>
#include <linux/vmalloc.h>

#include <drm/drm_cache.h>
#include <drm/drm_rect.h>

#include "fl2000.h"

/*
 * FL2000_SB_MIN URBs are kept in flight; the rest are spares the send path
 * draws from. Damage does not depend on this pool - it lands in the shadow
 * frame - so an empty pool only delays a send, it can never lose an update.
 */
#define FL2000_SB_MIN 3
#define FL2000_SB_NUM (FL2000_SB_MIN * 2)

#define FL2000_URB_TIMEOUT 100

/*
 * How many shadow generations of damage history are kept. A buffer further
 * behind than this is refreshed with a full copy.
 */
#define FL2000_DAMAGE_LOG 16

/* Must cover FL2000_MAX_WIDTH from fl2000_drm.c */
#define FL2000_MAX_LINE_PIXELS 4096

struct fl2000_stream_buf {
	struct list_head list;
	/*
	 * Generation of the desktop content and of the cursor this buffer was
	 * built from. Both matching means the buffer can be resent as it is.
	 */
	u32 seq;
	u32 cursor_seq;
	/* Where the cursor was blended in, so it can be lifted again */
	struct drm_rect cursor_rect;
	struct sg_table sgt;
	struct page **pages;
	unsigned int nr_pages;
	void *vaddr;
};

struct fl2000_stream {
	struct usb_device *usb_dev;
	/* Each buffer journey: render (free) -> wait (in flight) -> render */
	struct list_head render_list;
	struct list_head wait_list;
	spinlock_t list_lock; /* Protects the lists, taken from URB completion */
	size_t buf_size;
	u32 bytes_pix;
	unsigned int width;
	unsigned int height;
	/*
	 * The authoritative copy of the current frame, in the device's pixel
	 * format. Damage lands here and nowhere else, and every outgoing URB
	 * buffer is a copy of it.
	 */
	void *shadow;
	struct mutex shadow_lock; /* Serialises shadow and cursor against the send path */
	u32 shadow_seq;
	/*
	 * What changed at each shadow generation. A buffer that is only a few
	 * generations behind is caught up by replaying these rectangles
	 * instead of copying the whole frame.
	 */
	struct drm_rect damage_log[FL2000_DAMAGE_LOG];
	/*
	 * Bumped by cursor changes only, so moving the pointer never
	 * invalidates the desktop content a buffer already holds.
	 */
	u32 cursor_seq;
	/*
	 * The buffer that carries the most recent content. Only the send path
	 * writes a buffer, and never one that is in flight, so it can be
	 * copied from without a lock.
	 */
	struct fl2000_stream_buf *newest_sb;
	/* Cursor sprite in ARGB8888, blended on the way out */
	u32 *cursor_argb;
	unsigned int cursor_w;
	unsigned int cursor_h;
	int cursor_x;
	int cursor_y;
	struct work_struct work;
	struct workqueue_struct *work_queue;
	/* URBs the work may still submit: limits the frames in flight */
	atomic_t credits;
	wait_queue_head_t credits_wq;
	bool enabled;
	struct usb_anchor anchor;
	/* Throughput accounting, see fl2000_stream_report() */
	atomic_t frames;
	atomic_t rendered;
	atomic_t stalled;
	atomic_t conv_us_total;
	atomic_t conv_us_max;
	atomic_t damage_px_total;
	ktime_t last_report;
	/* Cached staging buffer for one scanline of XRGB8888 source pixels */
	u32 *line_buf;
};

/*
 * Periodic throughput line:
 *
 *  sent     - URBs completed, new frames and resends alike: the link rate.
 *  rendered - frames the compositor actually handed over: what the eye sees.
 *  stalled  - sends that could not use the shadow because no buffer was free
 *             or a conversion held it. No update is lost, the frame that goes
 *             out is one generation older.
 *  conv     - time converting pixels, and the average damaged area it covered.
 */
#define FL2000_REPORT_INTERVAL_MS 5000

static void fl2000_stream_report(struct fl2000_stream *stream)
{
	ktime_t now = ktime_get();
	s64 elapsed_ms = ktime_ms_delta(now, stream->last_report);
	unsigned int frames, rendered, stalled;
	unsigned int conv_us, conv_max, damage_px;

	if (elapsed_ms < FL2000_REPORT_INTERVAL_MS)
		return;

	stream->last_report = now;
	frames = atomic_xchg(&stream->frames, 0);
	rendered = atomic_xchg(&stream->rendered, 0);
	stalled = atomic_xchg(&stream->stalled, 0);
	conv_us = atomic_xchg(&stream->conv_us_total, 0);
	conv_max = atomic_xchg(&stream->conv_us_max, 0);
	damage_px = atomic_xchg(&stream->damage_px_total, 0);

	/* div64_u64(): the MB/s divisor does not fit in 32 bits */
	dev_dbg(&stream->usb_dev->dev,
		"Stream: %llu sent/s, %llu rendered/s, %llu MB/s, %u stalled, conv %u us (max %u) over %u px\n",
		div64_u64((u64)frames * 1000, elapsed_ms),
		div64_u64((u64)rendered * 1000, elapsed_ms),
		div64_u64((u64)frames * stream->buf_size * 1000, (u64)elapsed_ms * 1024 * 1024),
		stalled, rendered ? conv_us / rendered : 0, conv_max,
		rendered ? damage_px / rendered : 0);
}

/* Called by the primary plane update with the cost of one damage update */
void fl2000_stream_note_render(struct fl2000_stream *stream, unsigned int conv_us,
			       unsigned int damage_px)
{
	atomic_inc(&stream->rendered);
	atomic_add(conv_us, &stream->conv_us_total);
	atomic_add(damage_px, &stream->damage_px_total);
	if (conv_us > (unsigned int)atomic_read(&stream->conv_us_max))
		atomic_set(&stream->conv_us_max, conv_us);
}

static void fl2000_free_sb(struct fl2000_stream_buf *sb)
{
	vunmap(sb->vaddr);

	sg_free_table(&sb->sgt);

	for (int i = 0; i < sb->nr_pages && sb->pages[i]; i++)
		__free_page(sb->pages[i]);

	kfree(sb->pages);

	kfree(sb);
}

static struct fl2000_stream_buf *fl2000_alloc_sb(unsigned int size)
{
	int ret;
	struct fl2000_stream_buf *sb;
	unsigned int nr_pages = PAGE_ALIGN(size) >> PAGE_SHIFT;

	sb = kzalloc_obj(*sb);
	if (!sb)
		return NULL;

	sb->nr_pages = nr_pages;

	sb->pages = kzalloc_objs(*sb->pages, nr_pages);
	if (!sb->pages)
		goto error;

	for (int i = 0; i < nr_pages; i++) {
		sb->pages[i] = alloc_page(GFP_KERNEL);
		if (!sb->pages[i])
			goto error;
	}

	ret = sg_alloc_table_from_pages(&sb->sgt, sb->pages, nr_pages, 0, size, GFP_KERNEL);
	if (ret != 0)
		goto error;

	sb->vaddr = vmap(sb->pages, nr_pages, VM_MAP, PAGE_KERNEL);
	if (!sb->vaddr)
		goto error;

	INIT_LIST_HEAD(&sb->list);
	memset(sb->vaddr, 0, nr_pages << PAGE_SHIFT);

	return sb;

error:
	fl2000_free_sb(sb);

	return NULL;
}

static void fl2000_stream_put_buffers(struct fl2000_stream *stream)
{
	struct fl2000_stream_buf *cur_sb;
	struct fl2000_stream_buf *temp_sb;

	list_for_each_entry_safe(cur_sb, temp_sb, &stream->render_list, list) {
		list_del(&cur_sb->list);
		fl2000_free_sb(cur_sb);
	}

	/* These are gone; the send path must not copy from one of them */
	stream->newest_sb = NULL;
}

static int fl2000_stream_get_buffers(struct fl2000_stream *stream, unsigned int size)
{
	int ret;
	struct fl2000_stream_buf *cur_sb;

	if (WARN_ON(!list_empty(&stream->render_list)))
		return -EBUSY;

	for (int i = 0; i < FL2000_SB_NUM; i++) {
		cur_sb = fl2000_alloc_sb(size);
		if (!cur_sb) {
			ret = -ENOMEM;
			goto error;
		}

		list_add(&cur_sb->list, &stream->render_list);
	}

	return 0;

error:
	fl2000_stream_put_buffers(stream);
	return ret;
}

static void fl2000_stream_release(struct device *dev, void *res)
{
	struct fl2000_stream *stream = res;

	fl2000_stream_disable(stream);
	destroy_workqueue(stream->work_queue);
	fl2000_stream_put_buffers(stream);
	vfree(stream->shadow);
	kfree(stream->cursor_argb);
	kfree(stream->line_buf);
	mutex_destroy(&stream->shadow_lock);
}

static void fl2000_stream_data_completion(struct urb *urb)
{
	struct fl2000_stream_buf *cur_sb = urb->transfer_buffer;
	struct usb_device *usb_dev = urb->dev;
	struct fl2000_stream *stream = urb->context;
	unsigned long flags;

	if (stream) {
		spin_lock_irqsave(&stream->list_lock, flags);
		list_move_tail(&cur_sb->list, &stream->render_list);
		spin_unlock_irqrestore(&stream->list_lock, flags);

		atomic_inc(&stream->frames);

		/* Return the credit and kick the transmit work */
		atomic_inc(&stream->credits);
		wake_up(&stream->credits_wq);

		fl2000_urb_status(usb_dev, urb->status);
	}

	usb_free_urb(urb);
}

/* The end-of-frame packet carries no buffer: only its errors matter */
static void fl2000_stream_eof_completion(struct urb *urb)
{
	fl2000_urb_status(urb->dev, urb->status);

	usb_free_urb(urb);
}

/*
 * Anchor and submit one URB. Its completion frees it; one that could not be
 * submitted is freed here.
 */
static int fl2000_stream_submit(struct fl2000_stream *stream, struct urb *urb)
{
	int ret;

	usb_anchor_urb(urb, &stream->anchor);
	ret = fl2000_submit_urb(urb);
	if (ret) {
		/*
		 * A URB that was never submitted stays on the anchor. Freeing
		 * it there leaves a dangling entry that
		 * usb_kill_anchored_urbs() would spin on forever, which hangs
		 * the machine when the adapter is unplugged while streaming.
		 */
		usb_unanchor_urb(urb);
		usb_free_urb(urb);
	}

	return ret;
}

/*
 * Frame layout. The FL2000 expects the two 32-bit halves of every 64-bit word
 * of the frame swapped, and the frame is one flat stream: a line does not start
 * on a word boundary unless its length is a multiple of 8 bytes (1366 pixels at
 * 3 bytes per pixel is 4098). Every writer below therefore applies the swizzle
 * ('^ 4' on bytes for 24bpp, '^ 2' on 16-bit words for 16bpp) to the offset
 * from the start of the FRAME, which is why they take the frame base and the
 * offset of the line in it, never a pointer to the line.
 *
 * Cursor compositing. The sprite is blended into each outgoing frame rather
 * than into the shadow, so moving the pointer costs one 64x64 blend and never
 * re-reads the source framebuffer.
 */
static inline u8 fl2000_blend_channel(u8 dst, u8 src, u8 alpha)
{
	return (u8)((src * alpha + dst * (255 - alpha) + 127) / 255);
}

/* 'line' is the offset of the line in the frame, in 16-bit words */
static void fl2000_blend_cursor_rgb565_line(u16 *frame, size_t line, const u32 *cbuf, u32 x_start,
					    u32 x_end)
{
	for (unsigned int x = x_start; x < x_end; x++) {
		size_t idx = (line + x) ^ 2;
		u32 pix = *cbuf++;
		u8 alpha = (u8)(pix >> 24);
		u16 dst;
		u8 r, g, b;

		if (!alpha)
			continue;

		/* Expand the destination to 8 bits per channel, blend, pack again */
		dst = frame[idx];
		r = (u8)(((dst & 0xF800) >> 8) | ((dst & 0xF800) >> 13));
		g = (u8)(((dst & 0x07E0) >> 3) | ((dst & 0x07E0) >> 9));
		b = (u8)(((dst & 0x001F) << 3) | ((dst & 0x001F) >> 2));

		r = fl2000_blend_channel(r, (u8)(pix >> 16), alpha);
		g = fl2000_blend_channel(g, (u8)(pix >> 8), alpha);
		b = fl2000_blend_channel(b, (u8)pix, alpha);

		frame[idx] = (u16)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
	}
}

/* 'line' is the offset of the line in the frame, in bytes */
static void fl2000_blend_cursor_rgb888_line(u8 *frame, size_t line, const u32 *cbuf, u32 x_start,
					    u32 x_end)
{
	size_t xx = line + (size_t)x_start * 3;

	for (unsigned int x = x_start; x < x_end; x++) {
		u32 pix = *cbuf++;
		u8 alpha = (u8)(pix >> 24);

		if (!alpha) {
			xx += 3;
			continue;
		}

		/* Read and write through the swizzle */
		frame[xx ^ 4] = fl2000_blend_channel(frame[xx ^ 4], (u8)pix, alpha);
		xx++;
		frame[xx ^ 4] = fl2000_blend_channel(frame[xx ^ 4], (u8)(pix >> 8), alpha);
		xx++;
		frame[xx ^ 4] = fl2000_blend_channel(frame[xx ^ 4], (u8)(pix >> 16), alpha);
		xx++;
	}
}

/*
 * Blend the cursor into 'dst' and report the area it covered in 'rect', so the
 * next rebuild knows exactly what to restore.
 */
static void fl2000_blend_cursor(struct fl2000_stream *stream, void *dst, struct drm_rect *rect)
{
	u32 dst_line_len = stream->width * stream->bytes_pix;
	int cy_start, cy_end, cx_start, cx_end;

	*rect = (struct drm_rect){ 0, 0, 0, 0 };

	if (!stream->cursor_w || !stream->cursor_h || !stream->width)
		return;

	/* The cursor may hang off any edge of the frame */
	cx_start = max(stream->cursor_x, 0);
	cy_start = max(stream->cursor_y, 0);
	cx_end = min(stream->cursor_x + (int)stream->cursor_w, (int)stream->width);
	cy_end = min(stream->cursor_y + (int)stream->cursor_h, (int)stream->height);
	if (cx_start >= cx_end || cy_start >= cy_end)
		return;

	*rect = (struct drm_rect){ cx_start, cy_start, cx_end, cy_end };

	for (int y = cy_start; y < cy_end; y++) {
		const u32 *cline = stream->cursor_argb +
				   (size_t)(y - stream->cursor_y) * stream->cursor_w +
				   (cx_start - stream->cursor_x);

		if (stream->bytes_pix == 2)
			fl2000_blend_cursor_rgb565_line(dst, (size_t)y * stream->width, cline,
							cx_start, cx_end);
		else if (stream->bytes_pix == 3)
			fl2000_blend_cursor_rgb888_line(dst, (size_t)y * dst_line_len, cline,
							cx_start, cx_end);
	}
}

/*
 * Copy one rectangle of the shadow into an outgoing buffer. The pixels of a
 * line sit in 64-bit words whose halves are swapped, so a pixel's bytes can lie
 * up to 4 bytes outside the plain byte range of the pixel: copy whole words.
 */
static void fl2000_copy_rect(struct fl2000_stream *stream, void *dst, const struct drm_rect *r)
{
	u32 line_len = stream->width * stream->bytes_pix;
	size_t off, span;
	int x1, x2, y1, y2;

	/*
	 * A rectangle can outlive the geometry it was recorded under: a mode
	 * change that keeps the buffer size keeps the buffers too.
	 */
	x1 = clamp(r->x1, 0, (int)stream->width);
	x2 = clamp(r->x2, 0, (int)stream->width);
	y1 = clamp(r->y1, 0, (int)stream->height);
	y2 = clamp(r->y2, 0, (int)stream->height);
	if (x1 >= x2 || y1 >= y2)
		return;

	off = (size_t)x1 * stream->bytes_pix;
	span = (size_t)(x2 - x1) * stream->bytes_pix;

	for (int y = y1; y < y2; y++) {
		size_t start = round_down((size_t)y * line_len + off, 8);
		size_t end = min_t(size_t, round_up((size_t)y * line_len + off + span, 8),
				   stream->buf_size);

		memcpy(dst + start, stream->shadow + start, end - start);
	}
}

/*
 * Fill one URB buffer with what should be on screen now: the shadow frame plus
 * the cursor. A buffer that is only a few generations behind replays the
 * damage rectangles since, rather than copying the whole frame, and a buffer
 * whose desktop content is current only has the cursor lifted and re-blended.
 */
static void fl2000_stream_build(struct fl2000_stream *stream, struct fl2000_stream_buf *sb)
{
	/*
	 * Never block here. fl2000_stream_compress() holds the mutex for as
	 * long as one conversion takes, and waiting for it stops URB
	 * submission for that whole time: the device runs out of data and the
	 * monitor blinks. Resend instead, but never a frame older than the
	 * newest one already sent, or the picture would step back in time.
	 */
	if (!mutex_trylock(&stream->shadow_lock)) {
		struct fl2000_stream_buf *newest = stream->newest_sb;

		atomic_inc(&stream->stalled);

		if (newest && newest != sb &&
		    (sb->seq != newest->seq || sb->cursor_seq != newest->cursor_seq)) {
			memcpy(sb->vaddr, newest->vaddr, stream->buf_size);
			sb->seq = newest->seq;
			sb->cursor_seq = newest->cursor_seq;
			sb->cursor_rect = newest->cursor_rect;
		}

		return;
	}

	if (!stream->shadow) {
		mutex_unlock(&stream->shadow_lock);
		return;
	}

	if (sb->seq != stream->shadow_seq) {
		u32 behind = stream->shadow_seq - sb->seq;

		if (!sb->seq || behind >= FL2000_DAMAGE_LOG) {
			memcpy(sb->vaddr, stream->shadow, stream->buf_size);
		} else {
			/*
			 * Replay the rectangles one by one rather than their
			 * bounding box: damage in two opposite corners would
			 * otherwise cost a full-frame copy.
			 */
			for (u32 seq = sb->seq + 1; seq != stream->shadow_seq + 1; seq++)
				fl2000_copy_rect(stream, sb->vaddr,
						 &stream->damage_log[seq % FL2000_DAMAGE_LOG]);
		}

		sb->seq = stream->shadow_seq;
		/* The copy may have overwritten part of the blended cursor */
		sb->cursor_seq = 0;
	}

	if (sb->cursor_seq != stream->cursor_seq) {
		/* Lift the old cursor by restoring the desktop underneath it */
		fl2000_copy_rect(stream, sb->vaddr, &sb->cursor_rect);
		fl2000_blend_cursor(stream, sb->vaddr, &sb->cursor_rect);
		sb->cursor_seq = stream->cursor_seq;
	}

	stream->newest_sb = sb;
	mutex_unlock(&stream->shadow_lock);
}

static void fl2000_stream_work(struct work_struct *work)
{
	int ret;
	struct fl2000_stream *stream = container_of(work, struct fl2000_stream, work);
	struct usb_device *usb_dev = stream->usb_dev;
	struct fl2000_stream_buf *cur_sb;
	struct urb *data_urb, *eof_urb;

	while (READ_ONCE(stream->enabled)) {
		/* Interruptible sleep: an idle stream may wait here indefinitely */
		ret = wait_event_interruptible(stream->credits_wq, !READ_ONCE(stream->enabled) ||
					       atomic_add_unless(&stream->credits, -1, 0));
		if (ret) {
			dev_err(&usb_dev->dev, "Work interrupt error %d\n", ret);
			WRITE_ONCE(stream->enabled, false);
			return;
		}
		if (!READ_ONCE(stream->enabled))
			break;

		fl2000_stream_report(stream);

		spin_lock_irq(&stream->list_lock);
		if (list_empty(&stream->render_list)) {
			/*
			 * No buffer available - should not happen. Give the
			 * credit back: only completions replenish them, so one
			 * taken without submitting a URB is lost for good.
			 */
			spin_unlock_irq(&stream->list_lock);
			atomic_inc(&stream->stalled);
			atomic_inc(&stream->credits);
			usleep_range(1000, 2000);
			continue;
		}
		cur_sb = list_first_entry(&stream->render_list, struct fl2000_stream_buf, list);
		list_del_init(&cur_sb->list);
		spin_unlock_irq(&stream->list_lock);

		/*
		 * Building the frame copies up to several MB and must run with
		 * interrupts enabled, so cur_sb is owned (off all lists) for
		 * the duration. Holding list_lock (which disables IRQs) across
		 * it starves the whole system.
		 */
		fl2000_stream_build(stream, cur_sb);

		spin_lock_irq(&stream->list_lock);
		list_add_tail(&cur_sb->list, &stream->wait_list);
		spin_unlock_irq(&stream->list_lock);

		data_urb = usb_alloc_urb(0, GFP_KERNEL);
		eof_urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!data_urb || !eof_urb) {
			usb_free_urb(data_urb);
			usb_free_urb(eof_urb);
			dev_err(&usb_dev->dev, "Data URB allocation error\n");
			WRITE_ONCE(stream->enabled, false);
			return;
		}

		/*
		 * Endpoint 1 bulk out. We store pointer to current stream buffer structure in
		 * transfer_buffer field of URB which is unused due to SGT.
		 */
		usb_fill_bulk_urb(data_urb, usb_dev, usb_sndbulkpipe(usb_dev, 1), cur_sb,
				  (int)stream->buf_size, fl2000_stream_data_completion, stream);
		data_urb->interval = 0;
		data_urb->sg = cur_sb->sgt.sgl;
		data_urb->num_sgs = cur_sb->sgt.nents;

		/*
		 * The device ends a frame at a zero-length packet
		 * (FL2000_VGA_CTRL_ACLK_USE_ZERO_PKT_LEN), so send one of its
		 * own after every frame, as the vendor driver does.
		 * URB_ZERO_PACKET is not enough: it only adds the packet when
		 * the frame is a whole number of packets. Any other frame
		 * (1366x768 at 3 bytes per pixel is 3073.5 packets of 1024
		 * bytes) ends in a short packet, the device never sees the end
		 * of the frame, and the screen stays black while the device
		 * drains data at one pixel per clock, blanking included.
		 */
		usb_fill_bulk_urb(eof_urb, usb_dev, usb_sndbulkpipe(usb_dev, 1), NULL, 0,
				  fl2000_stream_eof_completion, stream);

		ret = fl2000_stream_submit(stream, data_urb);
		if (ret)
			usb_free_urb(eof_urb);
		else
			ret = fl2000_stream_submit(stream, eof_urb);
		if (ret) {
			dev_err(&usb_dev->dev, "Data URB error %d\n", ret);
			WRITE_ONCE(stream->enabled, false);
		}
	}
}

/*
 * The converters write through the frame swizzle described above. 'frame' is
 * the start of the destination frame, 'line' the offset of the destination line
 * in it (in bytes for 24bpp, in 16-bit words for 16bpp) and the x range is
 * absolute. 'sbuf' points at the first pixel of the span in the staging line.
 */
static void fl2000_xrgb8888_to_rgb888_line(u8 *frame, size_t line, const u32 *sbuf, u32 x_start,
					   u32 x_end)
{
	size_t xx = line + (size_t)x_start * 3;

	for (unsigned int x = x_start; x < x_end; x++) {
		u32 pix = *sbuf++;

		frame[xx++ ^ 4] = (pix & 0x000000FF) >> 0;
		frame[xx++ ^ 4] = (pix & 0x0000FF00) >> 8;
		frame[xx++ ^ 4] = (pix & 0x00FF0000) >> 16;
	}
}

static void fl2000_xrgb8888_to_rgb565_line(u16 *frame, size_t line, const u32 *sbuf, u32 x_start,
					   u32 x_end)
{
	for (unsigned int x = x_start; x < x_end; x++) {
		u32 pix = *sbuf++;

		frame[(line + x) ^ 2] = ((pix & 0x00F80000) >> 8) | ((pix & 0x0000FC00) >> 5) |
					((pix & 0x000000F8) >> 3);
	}
}

/*
 * The 10-bit formats keep the colour in bits 29:0 (bits 31:30 are unused or
 * alpha, which the primary plane ignores). Drop the two low bits of each
 * channel, in place, so the converters above only deal with XRGB8888.
 */
static void fl2000_xrgb2101010_to_xrgb8888_line(u32 *buf, u32 x_start, u32 x_end)
{
	for (unsigned int x = x_start; x < x_end; x++) {
		u32 pix = buf[x - x_start];

		buf[x - x_start] = (((pix >> 22) & 0xFF) << 16) | (((pix >> 12) & 0xFF) << 8) |
				   ((pix >> 2) & 0xFF);
	}
}

/*
 * Pull one span of source pixels into the cached line buffer.
 *
 * A framebuffer imported from the primary GPU is mapped write-combining,
 * whether it sits in VRAM (I/O memory) or in system memory pinned for us,
 * and a plain memcpy reads it at ~100 MB/s: every access is a partial
 * cache-line transaction. drm_memcpy_from_wc() uses streaming loads to fetch
 * whole lines, about ten times faster. On our own cached shmem buffers a
 * non-temporal load is slower than a plain memcpy, so choose per source.
 *
 * Streaming loads move whole 16 bytes and fall back to the slow plain read for
 * any other length, and a line of 1366 pixels is 5464 bytes (78 ms instead of
 * 8 ms per frame). From a 16-byte aligned address the length can be rounded
 * up without leaving the buffer, whose size is whole pages; the pixels past
 * the span land in the line buffer but are not converted.
 */
static void fl2000_read_span(struct fl2000_stream *stream, const struct iosys_map *src_map,
			     bool wc, size_t offset, size_t len)
{
	struct iosys_map dst = IOSYS_MAP_INIT_VADDR(stream->line_buf);
	struct iosys_map src = *src_map;
	uintptr_t addr;

	if (!wc) {
		iosys_map_memcpy_from(stream->line_buf, src_map, offset, len);
		return;
	}

	iosys_map_incr(&src, offset);
	addr = src.is_iomem ? (uintptr_t)src.vaddr_iomem : (uintptr_t)src.vaddr;
	if (IS_ALIGNED(addr, 16))
		len = ALIGN(len, 16);
	drm_memcpy_from_wc(&dst, &src, len);
}

/*
 * Convert one damaged rectangle of the source framebuffer into the shadow
 * frame. The shadow is the only thing written here, so it is always the
 * complete current frame: an update can never be lost because no URB buffer
 * happened to be free.
 */
void fl2000_stream_compress(struct fl2000_stream *stream, const struct iosys_map *src_map,
			    bool imported, u32 format, unsigned int width, unsigned int height,
			    unsigned int pitch, const struct drm_rect *clip)
{
	bool wc = imported || src_map->is_iomem;
	u32 dst_line_len;
	u32 x_start, x_end, y_start, y_end;
	size_t span;

	x_start = min_t(u32, max_t(int, clip->x1, 0), width);
	x_end = min_t(u32, max_t(int, clip->x2, 0), width);
	y_start = min_t(u32, max_t(int, clip->y1, 0), height);
	y_end = min_t(u32, max_t(int, clip->y2, 0), height);
	if (x_start >= x_end || y_start >= y_end)
		return;

	guard(mutex)(&stream->shadow_lock);

	if (!stream->shadow || width != stream->width || height != stream->height)
		return;

	/*
	 * Streaming loads need 16-byte alignment, which is 4 source pixels.
	 * Widening the span converts a few pixels twice, which is free next to
	 * losing the fast path.
	 */
	x_start &= ~3U;
	x_end = min(ALIGN(x_end, 4), width);

	dst_line_len = width * stream->bytes_pix;
	span = (x_end - x_start) * sizeof(u32);

	for (unsigned int y = y_start; y < y_end; y++) {
		/*
		 * Pull the span into a cached line buffer in one bulk transfer
		 * and convert from there: converting straight from an uncached
		 * source issues one read per pixel.
		 */
		fl2000_read_span(stream, src_map, wc,
				 (size_t)y * pitch + (size_t)x_start * sizeof(u32), span);

		if (format != DRM_FORMAT_XRGB8888)
			fl2000_xrgb2101010_to_xrgb8888_line(stream->line_buf, x_start, x_end);

		switch (stream->bytes_pix) {
		case 2:
			fl2000_xrgb8888_to_rgb565_line(stream->shadow, (size_t)y * width,
						       stream->line_buf, x_start, x_end);
			break;
		case 3:
			fl2000_xrgb8888_to_rgb888_line(stream->shadow, (size_t)y * dst_line_len,
						       stream->line_buf, x_start, x_end);
			break;
		default:
			/* Rejected by fl2000_get_bytes_pix() */
			break;
		}
	}

	stream->shadow_seq++;
	stream->damage_log[stream->shadow_seq % FL2000_DAMAGE_LOG] =
		(struct drm_rect){ x_start, y_start, x_end, y_end };
}

/*
 * Hand the cursor sprite and its position to the send path. 'argb' may be NULL
 * to hide the cursor.
 */
void fl2000_stream_set_cursor(struct fl2000_stream *stream, const u32 *argb, unsigned int w,
			      unsigned int h, int x, int y)
{
	if (w > FL2000_CURSOR_MAX || h > FL2000_CURSOR_MAX)
		return;

	guard(mutex)(&stream->shadow_lock);

	/* Nothing visible before or after: do not rebuild every outgoing frame */
	if (!argb && !stream->cursor_w)
		return;

	if (argb) {
		memcpy(stream->cursor_argb, argb, (size_t)w * h * sizeof(u32));
		stream->cursor_w = w;
		stream->cursor_h = h;
		stream->cursor_x = x;
		stream->cursor_y = y;
	} else {
		stream->cursor_w = 0;
		stream->cursor_h = 0;
	}

	stream->cursor_seq++;
}

/* Move the cursor without touching its image, the common case by far */
void fl2000_stream_move_cursor(struct fl2000_stream *stream, int x, int y)
{
	guard(mutex)(&stream->shadow_lock);

	if (!stream->cursor_w || (stream->cursor_x == x && stream->cursor_y == y))
		return;

	stream->cursor_x = x;
	stream->cursor_y = y;
	stream->cursor_seq++;
}

int fl2000_stream_mode_set(struct fl2000_stream *stream, unsigned int width,
			   unsigned int height, u32 bytes_pix)
{
	int ret;
	unsigned int size;
	void *shadow;

	/* Round buffer size up to multiple of 8 to meet HW expectations */
	size = ALIGN(width * height * bytes_pix, 8);

	guard(mutex)(&stream->shadow_lock);

	stream->bytes_pix = bytes_pix;
	stream->width = width;
	stream->height = height;

	/* If there are buffers with same size - keep them */
	if (stream->buf_size == size)
		return 0;

	/* Destroy wrong size buffers if they exist */
	if (!list_empty(&stream->render_list))
		fl2000_stream_put_buffers(stream);

	/* Allocate new buffers */
	ret = fl2000_stream_get_buffers(stream, size);
	if (ret) {
		fl2000_stream_put_buffers(stream);
		stream->buf_size = 0;
		return ret;
	}

	/* One full frame in the device format; only the CPU ever reads it */
	shadow = vzalloc(size);
	if (!shadow) {
		fl2000_stream_put_buffers(stream);
		stream->buf_size = 0;
		return -ENOMEM;
	}
	vfree(stream->shadow);
	stream->shadow = shadow;
	/*
	 * Both generations move on from zero: a buffer carrying zero has never
	 * been built and always takes the full-copy path.
	 */
	stream->shadow_seq++;
	stream->cursor_seq++;

	stream->buf_size = size;

	return 0;
}

int fl2000_stream_enable(struct fl2000_stream *stream)
{
	if (WARN_ON(list_empty(&stream->render_list)))
		return -ENOMEM;

	stream->last_report = ktime_get();
	atomic_set(&stream->frames, 0);
	atomic_set(&stream->rendered, 0);
	atomic_set(&stream->stalled, 0);
	atomic_set(&stream->conv_us_total, 0);
	atomic_set(&stream->conv_us_max, 0);
	atomic_set(&stream->damage_px_total, 0);

	/* Start the transmit work with the minimum number of buffers in flight */
	atomic_set(&stream->credits, FL2000_SB_MIN);
	WRITE_ONCE(stream->enabled, true);
	queue_work(stream->work_queue, &stream->work);

	return 0;
}

void fl2000_stream_disable(struct fl2000_stream *stream)
{
	struct fl2000_stream_buf *cur_sb;

	/* Wake the work up even when no URB is in flight to complete */
	WRITE_ONCE(stream->enabled, false);
	wake_up(&stream->credits_wq);

	drain_workqueue(stream->work_queue);

	if (!usb_wait_anchor_empty_timeout(&stream->anchor, 1000))
		usb_kill_anchored_urbs(&stream->anchor);

	spin_lock_irq(&stream->list_lock);
	while (!list_empty(&stream->wait_list)) {
		cur_sb = list_first_entry(&stream->wait_list, struct fl2000_stream_buf, list);
		list_move_tail(&cur_sb->list, &stream->render_list);
	}
	spin_unlock_irq(&stream->list_lock);
}

/**
 * fl2000_stream_create() - streaming processing context creation
 * @usb_dev: FL2000 USB device
 *
 * Called on DRM master bind. It shall not initiate any USB transfers: buffers
 * and URBs are allocated later, once the mode (and thus the frame size) is
 * known.
 *
 * Return: stream context, or an ERR_PTR() on failure
 */
struct fl2000_stream *fl2000_stream_create(struct usb_device *usb_dev)
{
	int ret;
	struct fl2000_stream *stream;

	/* Altsetting 1 on interface 0 */
	ret = usb_set_interface(usb_dev, FL2000_USBIF_AVCONTROL, 1);
	if (ret) {
		dev_err(&usb_dev->dev, "Cannot set streaming interface for bulk transfers\n");
		return ERR_PTR(ret);
	}

	stream = devres_alloc(&fl2000_stream_release, sizeof(*stream), GFP_KERNEL);
	if (!stream)
		return ERR_PTR(-ENOMEM);

	INIT_WORK(&stream->work, &fl2000_stream_work);
	INIT_LIST_HEAD(&stream->render_list);
	INIT_LIST_HEAD(&stream->wait_list);
	spin_lock_init(&stream->list_lock);
	mutex_init(&stream->shadow_lock);
	init_usb_anchor(&stream->anchor);
	atomic_set(&stream->credits, 0);
	init_waitqueue_head(&stream->credits_wq);
	stream->usb_dev = usb_dev;

	/* One scanline of source pixels, for the widest mode we advertise */
	stream->line_buf = kmalloc_objs(*stream->line_buf, FL2000_MAX_LINE_PIXELS);
	/* The largest cursor, allocated once: the atomic update must not fail */
	stream->cursor_argb = kmalloc_objs(*stream->cursor_argb,
					   FL2000_CURSOR_MAX * FL2000_CURSOR_MAX);
	stream->work_queue = alloc_ordered_workqueue("fl2000_stream", 0);
	if (!stream->line_buf || !stream->cursor_argb || !stream->work_queue) {
		if (stream->work_queue)
			destroy_workqueue(stream->work_queue);
		kfree(stream->cursor_argb);
		kfree(stream->line_buf);
		mutex_destroy(&stream->shadow_lock);
		devres_free(stream);
		return ERR_PTR(-ENOMEM);
	}

	devres_add(&usb_dev->dev, stream);

	return stream;
}

void fl2000_stream_destroy(struct usb_device *usb_dev)
{
	devres_release(&usb_dev->dev, fl2000_stream_release, NULL, NULL);
}
