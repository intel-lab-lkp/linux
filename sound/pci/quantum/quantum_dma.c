// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2026 Nicholas Johnson */
#include <linux/module.h>

#include "quantum.h"

#define QUANTUM_FPGA_PAGE_SIZE 4096U
#define QUANTUM_PT_WORDS       (QUANTUM_FPGA_PAGE_SIZE / sizeof(__le64))
#define QUANTUM_DEFAULT_CPU_LATENCY_US 2

/*
 * The hardware's real-time service interval is quantum/rate, held roughly
 * constant across rates by quantum_hw_quantum() -- 32 frames at 48kHz is
 * exactly as tight (~667us) as 128 frames at 192kHz. Compare against that
 * interval directly, cross-multiplied to stay in integer arithmetic,
 * rather than hardcoding the one rate this used to only ever apply to.
 */
static bool quantum_needs_low_latency(struct quantum_chip *chip)
{
	return (u64)chip->dma_quantum_frames * 48000 <=
	       (u64)QUANTUM_BASE_HW_QUANTUM * chip->current_sample_rate;
}

static int quantum_effective_cpu_latency_us(struct quantum_chip *chip)
{
	if (chip->cpu_latency_us < 0)
		return chip->cpu_latency_us;

	if (chip->cpu_latency_auto_low_latency &&
	    quantum_needs_low_latency(chip))
		return 0;

	return chip->cpu_latency_us;
}

static void quantum_cpu_latency_qos_apply(struct quantum_chip *chip)
{
	int latency_us = quantum_effective_cpu_latency_us(chip);

	if (latency_us < 0) {
		if (cpu_latency_qos_request_active(&chip->cpu_latency_qos))
			cpu_latency_qos_remove_request(&chip->cpu_latency_qos);
		return;
	}

	if (cpu_latency_qos_request_active(&chip->cpu_latency_qos))
		cpu_latency_qos_update_request(&chip->cpu_latency_qos,
					       latency_us);
	else
		cpu_latency_qos_add_request(&chip->cpu_latency_qos,
					    latency_us);
}

static void quantum_cpu_latency_qos_enable(struct quantum_chip *chip)
{
	quantum_cpu_latency_qos_apply(chip);
}

static void quantum_cpu_latency_qos_disable(struct quantum_chip *chip)
{
	if (!cpu_latency_qos_request_active(&chip->cpu_latency_qos))
		return;

	cpu_latency_qos_remove_request(&chip->cpu_latency_qos);
}

static ssize_t cpu_latency_us_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct snd_card *card = dev_get_drvdata(dev);
	struct quantum_chip *chip;

	if (!card)
		return -ENODEV;

	chip = card->private_data;

	return sysfs_emit(buf, "%d\n", chip->cpu_latency_us);
}

static ssize_t cpu_latency_us_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct snd_card *card = dev_get_drvdata(dev);
	struct quantum_chip *chip;
	int value;
	int err;

	if (!card)
		return -ENODEV;

	err = kstrtoint(buf, 0, &value);
	if (err)
		return err;

	chip = card->private_data;
	mutex_lock(&chip->dma_mutex);
	chip->cpu_latency_us = value;
	if (READ_ONCE(chip->stream_active))
		quantum_cpu_latency_qos_apply(chip);
	mutex_unlock(&chip->dma_mutex);

	return count;
}
static DEVICE_ATTR_RW(cpu_latency_us);

static ssize_t cpu_latency_auto_low_latency_show(struct device *dev,
						 struct device_attribute *attr,
						 char *buf)
{
	struct snd_card *card = dev_get_drvdata(dev);
	struct quantum_chip *chip;

	if (!card)
		return -ENODEV;

	chip = card->private_data;

	return sysfs_emit(buf, "%u\n", chip->cpu_latency_auto_low_latency);
}

static ssize_t cpu_latency_auto_low_latency_store(struct device *dev,
						  struct device_attribute *attr,
						  const char *buf, size_t count)
{
	struct snd_card *card = dev_get_drvdata(dev);
	struct quantum_chip *chip;
	bool value;
	int err;

	if (!card)
		return -ENODEV;

	err = kstrtobool(buf, &value);
	if (err)
		return err;

	chip = card->private_data;
	mutex_lock(&chip->dma_mutex);
	chip->cpu_latency_auto_low_latency = value;
	if (READ_ONCE(chip->stream_active))
		quantum_cpu_latency_qos_apply(chip);
	mutex_unlock(&chip->dma_mutex);

	return count;
}
static DEVICE_ATTR_RW(cpu_latency_auto_low_latency);

static ssize_t cpu_latency_effective_us_show(struct device *dev,
					     struct device_attribute *attr,
					     char *buf)
{
	struct snd_card *card = dev_get_drvdata(dev);
	struct quantum_chip *chip;
	int value;

	if (!card)
		return -ENODEV;

	chip = card->private_data;
	mutex_lock(&chip->dma_mutex);
	value = quantum_effective_cpu_latency_us(chip);
	mutex_unlock(&chip->dma_mutex);

	return sysfs_emit(buf, "%d\n", value);
}
static DEVICE_ATTR_RO(cpu_latency_effective_us);

static ssize_t cpu_latency_state_show(struct device *dev,
				      struct device_attribute *attr,
				      char *buf)
{
	struct snd_card *card = dev_get_drvdata(dev);
	struct quantum_chip *chip;
	int effective;
	ssize_t count;

	if (!card)
		return -ENODEV;

	chip = card->private_data;
	mutex_lock(&chip->dma_mutex);
	effective = quantum_effective_cpu_latency_us(chip);
	count = sysfs_emit(buf,
			   "configured_us=%d effective_us=%d auto_low_latency=%u active=%u rate=%u quantum=%u period=%u buffer=%u\n",
			   chip->cpu_latency_us, effective,
			   chip->cpu_latency_auto_low_latency,
			   READ_ONCE(chip->stream_active) ? 1 : 0,
			   chip->current_sample_rate, chip->dma_quantum_frames,
			   chip->period_frames, chip->buffer_frames);
	mutex_unlock(&chip->dma_mutex);

	return count;
}
static DEVICE_ATTR_RO(cpu_latency_state);

int quantum_cpu_latency_sysfs_create(struct quantum_chip *chip)
{
	int err;

	chip->cpu_latency_us = QUANTUM_DEFAULT_CPU_LATENCY_US;
	chip->cpu_latency_auto_low_latency = true;

	err = device_create_file(&chip->pci->dev, &dev_attr_cpu_latency_us);
	if (err)
		return err;

	err = device_create_file(&chip->pci->dev,
				 &dev_attr_cpu_latency_auto_low_latency);
	if (err)
		goto err_remove_latency;

	err = device_create_file(&chip->pci->dev,
				 &dev_attr_cpu_latency_effective_us);
	if (err)
		goto err_remove_auto;

	err = device_create_file(&chip->pci->dev, &dev_attr_cpu_latency_state);
	if (err)
		goto err_remove_effective;

	return 0;

err_remove_effective:
	device_remove_file(&chip->pci->dev, &dev_attr_cpu_latency_effective_us);
err_remove_auto:
	device_remove_file(&chip->pci->dev,
			   &dev_attr_cpu_latency_auto_low_latency);
err_remove_latency:
	device_remove_file(&chip->pci->dev, &dev_attr_cpu_latency_us);
	return err;
}

void quantum_cpu_latency_sysfs_remove(struct quantum_chip *chip)
{
	/*
	 * Must run exactly once, synchronously from remove(): these files
	 * are attached to chip->pci->dev, which the PCI core deletes shortly
	 * after remove() returns, racing a later device_remove_file() call
	 * from the deferred private_free() path.
	 */
	if (test_and_set_bit(0, &chip->sysfs_removed))
		return;

	device_remove_file(&chip->pci->dev, &dev_attr_cpu_latency_state);
	device_remove_file(&chip->pci->dev, &dev_attr_cpu_latency_effective_us);
	device_remove_file(&chip->pci->dev,
			   &dev_attr_cpu_latency_auto_low_latency);
	device_remove_file(&chip->pci->dev, &dev_attr_cpu_latency_us);
}

/* Build a chained page table */

static int quantum_build_page_table(struct quantum_chip *chip,
				    struct quantum_page_table *pt,
				    const struct quantum_stream *stream,
				    struct snd_pcm_substream *substream,
				    size_t audio_size,
				    u32 addr_per_segment)
{
	u32 pt_stride = PAGE_SIZE;
	u32 num_pages = DIV_ROUND_UP(audio_size, QUANTUM_FPGA_PAGE_SIZE);
	u32 fw_pages_per_host_page = PAGE_SIZE / QUANTUM_FPGA_PAGE_SIZE;
	u32 num_segments;
	u32 seg, i;

	if (!num_pages || PAGE_SIZE < QUANTUM_FPGA_PAGE_SIZE ||
	    PAGE_SIZE % QUANTUM_FPGA_PAGE_SIZE)
		return -EINVAL;
	if (substream) {
		if (!substream->runtime || !substream->runtime->dma_area ||
		    substream->runtime->dma_bytes < audio_size)
			return -EINVAL;
	} else if (!stream->pages ||
		   stream->num_pages * fw_pages_per_host_page < num_pages) {
		return -EINVAL;
	}
	/* One word after the leaf entries is reserved for the next-record link. */
	if (!addr_per_segment || addr_per_segment >= QUANTUM_PT_WORDS)
		return -EINVAL;

	num_segments = (num_pages + addr_per_segment - 1) / addr_per_segment;

	pt->num_entries = num_pages;
	pt->num_segments = num_segments;
	pt->addr_per_segment = addr_per_segment;
	pt->size = num_segments * pt_stride;
	pt->pages = kcalloc(num_segments, sizeof(*pt->pages), GFP_KERNEL);
	if (!pt->pages)
		return -ENOMEM;

	for (seg = 0; seg < num_segments; seg++) {
		pt->pages[seg].cpu_addr = dma_alloc_coherent(&chip->pci->dev,
							     PAGE_SIZE,
							     &pt->pages[seg].dma_handle,
							     GFP_KERNEL);
		if (!pt->pages[seg].cpu_addr)
			goto err_free_pages;
		memset(pt->pages[seg].cpu_addr, 0, PAGE_SIZE);
	}
	pt->cpu_addr = pt->pages[0].cpu_addr;
	pt->dma_handle = pt->pages[0].dma_handle;

	for (seg = 0; seg < num_segments; seg++) {
		__le64 *entries = pt->pages[seg].cpu_addr;
		u32 first_idx = seg * addr_per_segment;
		u32 last_idx = min(first_idx + addr_per_segment, num_pages);
		u32 count = last_idx - first_idx;

		for (i = 0; i < count; i++) {
			u32 fw_page = first_idx + i;
			u32 host_page = fw_page / fw_pages_per_host_page;
			u32 offset = (fw_page % fw_pages_per_host_page) *
				     QUANTUM_FPGA_PAGE_SIZE;
			u64 addr;

			if (substream)
				addr = snd_pcm_sgbuf_get_addr(substream,
							      fw_page * QUANTUM_FPGA_PAGE_SIZE);
			else
				addr = stream->pages[host_page].dma_handle + offset;
			if (!addr) {
				seg = num_segments;
				goto err_free_pages;
			}
			addr |= 1ULL;
			entries[i] = cpu_to_le64(addr);
		}

		if (seg + 1 < num_segments) {
			u64 chain = pt->pages[seg + 1].dma_handle | 1ULL;

			entries[addr_per_segment] = cpu_to_le64(chain);
		}
	}

	return 0;

err_free_pages:
	while (seg--)
		dma_free_coherent(&chip->pci->dev, PAGE_SIZE,
				  pt->pages[seg].cpu_addr,
				  pt->pages[seg].dma_handle);
	kfree(pt->pages);
	memset(pt, 0, sizeof(*pt));
	return -ENOMEM;
}

static int quantum_alloc_stream_pages(struct quantum_chip *chip,
				      struct quantum_stream *stream,
				      size_t bytes)
{
	u32 i;

	if (!bytes)
		return -EINVAL;

	stream->num_pages = DIV_ROUND_UP(bytes, PAGE_SIZE);
	stream->pages = kcalloc(stream->num_pages, sizeof(*stream->pages),
				GFP_KERNEL);
	if (!stream->pages)
		return -ENOMEM;

	for (i = 0; i < stream->num_pages; i++) {
		stream->pages[i].cpu_addr = dma_alloc_coherent(&chip->pci->dev,
							       PAGE_SIZE,
							       &stream->pages[i].dma_handle,
				GFP_KERNEL);
		if (!stream->pages[i].cpu_addr)
			goto err_free;
		memset(stream->pages[i].cpu_addr, 0, PAGE_SIZE);
	}

	stream->buffer_bytes = bytes;
	stream->valid = true;
	stream->owns_pages = true;
	return 0;

err_free:
	while (i--)
		dma_free_coherent(&chip->pci->dev, PAGE_SIZE,
				  stream->pages[i].cpu_addr,
				  stream->pages[i].dma_handle);
	kfree(stream->pages);
	stream->pages = NULL;
	stream->num_pages = 0;
	stream->buffer_bytes = 0;
	stream->valid = false;
	stream->owns_pages = false;
	return -ENOMEM;
}

static void quantum_free_stream_pages(struct quantum_chip *chip,
				      struct quantum_stream *stream)
{
	u32 i;
	bool gone = pci_dev_is_disconnected(chip->pci);

	if (!stream->owns_pages)
		goto reset;

	/* Leak instead of touching a device already torn down by the PCI core. */
	if (!gone) {
		for (i = 0; i < stream->num_pages; i++)
			dma_free_coherent(&chip->pci->dev, PAGE_SIZE,
					  stream->pages[i].cpu_addr,
					  stream->pages[i].dma_handle);
	}
	kfree(stream->pages);

reset:
	stream->pages = NULL;
	stream->num_pages = 0;
	stream->buffer_bytes = 0;
	stream->valid = false;
	stream->owns_pages = false;
}

static void quantum_free_page_table(struct quantum_chip *chip,
				    struct quantum_page_table *pt)
{
	u32 i;

	/* See the comment in quantum_free_stream_pages() above. */
	if (!pci_dev_is_disconnected(chip->pci)) {
		for (i = 0; i < pt->num_segments; i++)
			dma_free_coherent(&chip->pci->dev, PAGE_SIZE,
					  pt->pages[i].cpu_addr,
					  pt->pages[i].dma_handle);
	}
	kfree(pt->pages);
	memset(pt, 0, sizeof(*pt));
}

static void quantum_reset_dma_descriptors(struct quantum_chip *chip)
{
	chip->playback.pt = &chip->playback.pt_storage;
	chip->capture.pt = &chip->capture.pt_storage;
	memset(&chip->playback.pt_storage, 0, sizeof(chip->playback.pt_storage));
	memset(&chip->capture.pt_storage, 0, sizeof(chip->capture.pt_storage));
}

/* Publish a page table to hardware */

static void quantum_write_page_table(struct quantum_chip *chip,
				     struct quantum_page_table *pt,
				     bool is_playback)
{
	u32 addr_low, addr_high;
	u32 block_count, block_size;

	addr_low = lower_32_bits(pt->dma_handle);
	addr_high = upper_32_bits(pt->dma_handle);
	block_count = chip->dma_ring_frames;
	block_size = chip->dma_quantum_frames;
	/* Ensure page-table contents are visible before publishing roots. */
	dma_wmb();

	if (is_playback) {
		quantum_write32(chip, QUANTUM_REG_PLAY_BLOCK_COUNT, block_count);
		quantum_write32(chip, QUANTUM_REG_PLAY_BLOCK_SIZE, block_size);
		quantum_write32(chip, QUANTUM_REG_PLAY_PAGE_TABLE_ADDR_HIGH, addr_high);
		quantum_write32(chip, QUANTUM_REG_PLAY_PAGE_TABLE_ADDR_LOW, addr_low);
	} else {
		quantum_write32(chip, QUANTUM_REG_REC_BLOCK_COUNT, block_count);
		quantum_write32(chip, QUANTUM_REG_REC_BLOCK_SIZE, block_size);
		quantum_write32(chip, QUANTUM_REG_REC_PAGE_TABLE_ADDR_HIGH, addr_high);
		quantum_write32(chip, QUANTUM_REG_REC_PAGE_TABLE_ADDR_LOW, addr_low);
	}
}

static void quantum_clear_page_table_registers(struct quantum_chip *chip)
{
	quantum_write32(chip, QUANTUM_REG_PLAY_BLOCK_COUNT, 0);
	quantum_write32(chip, QUANTUM_REG_PLAY_BLOCK_SIZE, 0);
	quantum_write32(chip, QUANTUM_REG_PLAY_PAGE_TABLE_ADDR_HIGH, 0);
	quantum_write32(chip, QUANTUM_REG_PLAY_PAGE_TABLE_ADDR_LOW, 0);
	quantum_write32(chip, QUANTUM_REG_REC_BLOCK_COUNT, 0);
	quantum_write32(chip, QUANTUM_REG_REC_BLOCK_SIZE, 0);
	quantum_write32(chip, QUANTUM_REG_REC_PAGE_TABLE_ADDR_HIGH, 0);
	quantum_write32(chip, QUANTUM_REG_REC_PAGE_TABLE_ADDR_LOW, 0);

	/* Flush posted register clears before freeing page-table storage. */
	quantum_read32(chip, QUANTUM_REG_REC_PAGE_TABLE_ADDR_LOW);
}

/* Wait for the hardware to fetch the page tables */

static int quantum_wait_page_table_ready(struct quantum_chip *chip)
{
	int timeout = 100;
	u32 status;

	while (timeout-- > 0) {
		status = quantum_read32(chip, QUANTUM_REG_PAGE_TABLE_STATUS);
		if ((status & 0xFF00) && (status & 0xFF))
			return 0;

		usleep_range(10, 20);
	}

	dev_err(&chip->pci->dev,
		"page table activation timed out (status=0x%08x)\n", status);
	return -ETIMEDOUT;
}

/* Allocate audio DMA resources */

int quantum_allocate_dma_resources(struct quantum_chip *chip,
				   unsigned int sample_rate,
				   unsigned int period_frames,
				   unsigned int buffer_frames,
				   struct snd_pcm_substream *playback_substream,
				   struct snd_pcm_substream *capture_substream)
{
	u32 channel_cfg;
	u32 playback_channels, capture_channels;
	size_t playback_bytes, capture_bytes;
	u32 hw_quantum;
	u32 addr_per_seg;
	int err;

	if (READ_ONCE(chip->resources_released) ||
	    quantum_device_unavailable(chip))
		return -ENODEV;

	chip->playback.pt = &chip->playback.pt_storage;
	chip->capture.pt = &chip->capture.pt_storage;

	if (READ_ONCE(chip->dma_resources_allocated))
		quantum_free_dma_resources(chip);

	channel_cfg = quantum_read32(chip, QUANTUM_REG_CHANNEL_COUNTS);
	capture_channels = channel_cfg & 0xff;
	playback_channels = (channel_cfg >> 8) & 0xff;
	if (!capture_channels || !playback_channels) {
		dev_err(&chip->pci->dev,
			"Invalid DMA channel register: 0x%08x\n", channel_cfg);
		return -EIO;
	}
	hw_quantum = quantum_hw_quantum(sample_rate, period_frames);
	if (!period_frames || !hw_quantum ||
	    buffer_frames < hw_quantum ||
	    buffer_frames % hw_quantum)
		return -EINVAL;
	chip->dma_quantum_frames = hw_quantum;
	chip->dma_ring_frames = buffer_frames;
	playback_bytes = (size_t)chip->dma_ring_frames * sizeof(u32) *
			 playback_channels;
	capture_bytes = (size_t)chip->dma_ring_frames * sizeof(u32) *
			capture_channels;
	chip->playback.channels = playback_channels;
	chip->capture.channels = capture_channels;

	chip->current_sample_rate = sample_rate;

	chip->playback.buffer_bytes = playback_bytes;
	chip->playback.valid = true;
	chip->playback.owns_pages = false;
	if (!playback_substream) {
		err = quantum_alloc_stream_pages(chip, &chip->playback,
						 playback_bytes);
		if (err)
			return err;
	}

	chip->capture.buffer_bytes = capture_bytes;
	chip->capture.valid = true;
	chip->capture.owns_pages = false;
	if (!capture_substream) {
		err = quantum_alloc_stream_pages(chip, &chip->capture,
						 capture_bytes);
		if (err)
			goto err_free;
	}

	addr_per_seg = chip->addr_per_segment_play;
	if (!addr_per_seg || addr_per_seg >= QUANTUM_PT_WORDS) {
		err = -EINVAL;
		dev_err(&chip->pci->dev, "Invalid playback PT leaf count: %u\n",
			addr_per_seg);
		goto err_free;
	}

	err = quantum_build_page_table(chip, &chip->playback.pt_storage,
				       &chip->playback,
				       playback_substream,
				       playback_bytes, addr_per_seg);
	if (err) {
		dev_err(&chip->pci->dev, "Playback page table build failed: %d\n", err);
		goto err_free;
	}

	addr_per_seg = chip->addr_per_segment_rec;
	err = quantum_build_page_table(chip, &chip->capture.pt_storage,
				       &chip->capture,
				       capture_substream,
				       capture_bytes, addr_per_seg);
	if (err) {
		dev_err(&chip->pci->dev, "Capture page table build failed: %d\n", err);
		goto err_free_playback_pt;
	}

	chip->playback.pt = &chip->playback.pt_storage;
	chip->capture.pt = &chip->capture.pt_storage;

	quantum_write_page_table(chip, chip->playback.pt, true);
	quantum_write_page_table(chip, chip->capture.pt, false);

	err = quantum_wait_page_table_ready(chip);
	if (err) {
		quantum_clear_page_table_registers(chip);
		goto err_free_capture_pt;
	}
	WRITE_ONCE(chip->dma_resources_allocated, true);
	chip->period_frames = period_frames;
	chip->buffer_frames = buffer_frames;

	dev_dbg(&chip->pci->dev,
		"DMA configured: rate=%u ring=%u quantum=%u play_ch=%u cap_ch=%u play_pt=%u/%u cap_pt=%u/%u\n",
		sample_rate, chip->dma_ring_frames, chip->dma_quantum_frames,
		playback_channels, capture_channels,
		chip->playback.pt_storage.num_entries,
		chip->playback.pt_storage.num_segments,
		chip->capture.pt_storage.num_entries,
		chip->capture.pt_storage.num_segments);
	return 0;

err_free_capture_pt:
	if (chip->capture.pt_storage.cpu_addr)
		quantum_free_page_table(chip, &chip->capture.pt_storage);
err_free_playback_pt:
	if (chip->playback.pt_storage.cpu_addr)
		quantum_free_page_table(chip, &chip->playback.pt_storage);
err_free:
	quantum_reset_dma_descriptors(chip);
	quantum_free_stream_pages(chip, &chip->capture);
	quantum_free_stream_pages(chip, &chip->playback);
	return err;
}

/* Free audio DMA resources */

void quantum_free_dma_resources(struct quantum_chip *chip)
{
	if (!READ_ONCE(chip->dma_resources_allocated))
		return;

	/*
	 * Normal hw_free stops the engine and invalidates its page-table roots.
	 * Orderly PCI removal does that explicitly before setting device_gone;
	 * surprise removal cannot access BARs, so this becomes memory-only cleanup.
	 */
	if (!test_bit(0, &chip->device_gone)) {
		quantum_stop_dma(chip);
		quantum_clear_page_table_registers(chip);
	} else {
		WRITE_ONCE(chip->stream_active, false);
	}

	if (chip->playback.pt && chip->playback.pt->cpu_addr)
		quantum_free_page_table(chip, chip->playback.pt);

	if (chip->capture.pt && chip->capture.pt->cpu_addr)
		quantum_free_page_table(chip, chip->capture.pt);

	quantum_reset_dma_descriptors(chip);
	quantum_free_stream_pages(chip, &chip->playback);
	quantum_free_stream_pages(chip, &chip->capture);
	quantum_cpu_latency_qos_disable(chip);

	chip->playback.valid = false;
	chip->capture.valid = false;
	WRITE_ONCE(chip->dma_resources_allocated, false);
	chip->current_sample_rate = 0;
	dev_dbg(&chip->pci->dev, "audio DMA resources released\n");
}

/* Start audio DMA */

int quantum_start_dma(struct quantum_chip *chip)
{
	if (test_bit(0, &chip->device_gone))
		return -ENODEV;

	if (!READ_ONCE(chip->dma_resources_allocated)) {
		dev_err(&chip->pci->dev, "cannot start without DMA resources\n");
		return -EINVAL;
	}

	/*
	 * DMA_START=3 resets the hardware stream position to ring position
	 * zero and cycle zero. Do not seed last_dma_pos from the stale value
	 * left in DMA_POSITION while the engine is stopped.
	 */
	chip->last_dma_pos = 0;
	chip->dma_cycle_count = 0;
	WRITE_ONCE(chip->dma_position_valid, true);
	chip->dma_elapsed_frames = 0;
	chip->dma_pending_frames = 0;
	chip->playback_attach_pending = false;
	chip->capture_attach_pending = false;
	quantum_cpu_latency_qos_enable(chip);

	quantum_write32(chip, QUANTUM_REG_INTERRUPT_STATUS,
			QUANTUM_INT_DMA);

	quantum_write32_mask(chip, QUANTUM_REG_DMA_INT_ENABLE,
			     QUANTUM_INT_DMA | QUANTUM_INT_CMD_RX,
			     QUANTUM_INT_DMA | QUANTUM_INT_CMD_RX);

	/*
	 * Set stream_active before starting the engine so an immediate
	 * interrupt cannot be discarded by the worker.
	 */
	/* Pair with IRQ/thread readers before they consume stream state. */
	smp_store_release(&chip->stream_active, true);

	quantum_write32(chip, QUANTUM_REG_DMA_START, 3);
	dev_dbg(&chip->pci->dev,
		"audio DMA started: rate=%u ring=%u quantum=%u\n",
		chip->current_sample_rate, chip->dma_ring_frames,
		chip->dma_quantum_frames);
	return 0;
}

/* Stop audio DMA */

static void __quantum_stop_dma(struct quantum_chip *chip, bool force)
{
	u32 irq_enable;
	u32 status = 0;
	int retry;

	if (quantum_device_unavailable(chip) ||
	    READ_ONCE(chip->resources_released)) {
		WRITE_ONCE(chip->stream_active, false);
		quantum_cpu_latency_qos_disable(chip);
		return;
	}

	if (!force && !READ_ONCE(chip->stream_active)) {
		quantum_cpu_latency_qos_disable(chip);
		return;
	}

	irq_enable = quantum_read32(chip, QUANTUM_REG_DMA_INT_ENABLE);
	quantum_write32(chip, QUANTUM_REG_DMA_INT_ENABLE,
			irq_enable & ~QUANTUM_INT_DMA);
	quantum_write32(chip, QUANTUM_REG_DMA_START, 0);
	for (retry = 0; retry < 100; retry++) {
		status = quantum_read32(chip, QUANTUM_REG_DMA_STOP_STATUS);
		if (!(status & 3))
			break;

		usleep_range(10, 20);
	}
	if (retry == 100)
		dev_warn(&chip->pci->dev,
			 "audio DMA did not become idle (status=0x%08x)\n",
			 status);

	WRITE_ONCE(chip->stream_active, false);
	quantum_cpu_latency_qos_disable(chip);
	dev_dbg(&chip->pci->dev, "audio DMA stopped\n");
}

void quantum_stop_dma(struct quantum_chip *chip)
{
	__quantum_stop_dma(chip, false);
}

void quantum_shutdown_audio_dma(struct quantum_chip *chip)
{
	if (quantum_device_unavailable(chip))
		return;

	__quantum_stop_dma(chip, true);
	quantum_clear_page_table_registers(chip);
	dev_dbg(&chip->pci->dev, "audio DMA registers cleared\n");
}

/* Update the audio DMA position */

void quantum_update_dma_position(struct quantum_chip *chip)
{
	u32 raw, pos, cycle, cycle_delta, delta, max_delta;
	s64 signed_delta;

	/* Pair with stream_active publication before reading stream state. */
	if (!smp_load_acquire(&chip->stream_active) ||
	    !READ_ONCE(chip->dma_resources_allocated))
		return;

	raw = quantum_read32(chip, QUANTUM_REG_DMA_POSITION);
	pos = raw & 0x000fffff;
	cycle = raw >> 20;
	if (!chip->dma_ring_frames || pos >= chip->dma_ring_frames) {
		dev_warn_ratelimited(&chip->pci->dev,
				     "invalid DMA position: raw=0x%08x ring=%u\n",
			raw, chip->dma_ring_frames);
		chip->dma_elapsed_frames = 0;
		return;
	}
	if (!READ_ONCE(chip->dma_position_valid)) {
		chip->last_dma_pos = pos;
		chip->dma_cycle_count = cycle;
		chip->dma_elapsed_frames = 0;
		WRITE_ONCE(chip->dma_position_valid, true);
		return;
	}
	cycle_delta = (cycle - chip->dma_cycle_count) & 0xfff;
	signed_delta = (s64)pos - chip->last_dma_pos +
		       (s64)cycle_delta * chip->dma_ring_frames;
	if (signed_delta < 0) {
		dev_warn_ratelimited(&chip->pci->dev,
				     "DMA position moved backwards: raw=0x%08x last=%u cycle_delta=%u\n",
			raw, chip->last_dma_pos, cycle_delta);
		delta = chip->dma_quantum_frames;
	} else {
		delta = signed_delta;
	}
	max_delta = chip->dma_ring_frames;
	if (delta > max_delta) {
		dev_warn_ratelimited(&chip->pci->dev,
				     "implausible DMA delta: raw=0x%08x delta=%u; resyncing\n",
			raw, delta);
		delta = chip->dma_quantum_frames;
	}
	chip->last_dma_pos = pos;
	chip->dma_cycle_count = cycle;
	chip->dma_elapsed_frames = delta;
}

void quantum_transfer_audio(struct quantum_chip *chip)
{
	u32 raw_elapsed = chip->dma_elapsed_frames;
	u32 quantum = chip->dma_quantum_frames;
	u32 processed;

	if (!raw_elapsed || !quantum)
		return;

	chip->dma_pending_frames += raw_elapsed;
	processed = (chip->dma_pending_frames / quantum) * quantum;
	chip->dma_pending_frames -= processed;
	if (READ_ONCE(chip->capture_substream))
		/* Ensure captured frames are visible before ALSA wakeups. */
		dma_rmb();

	chip->dma_elapsed_frames = processed;
}
