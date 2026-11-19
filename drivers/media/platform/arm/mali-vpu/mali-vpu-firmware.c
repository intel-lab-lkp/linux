// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 ARM Limited.
 */

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/sizes.h>
#include <linux/stddef.h>
#include <linux/string.h>

#include "mali-vpu-firmware.h"
#include "mali-vpu-firmware-cache.h"
#include "mali-vpu-formats.h"
#include "mali-vpu-mmu.h"
#include "mali-vpu-protocol-def-v3.h"

#define MALI_VPU_FW_MAX_CORES 8
#define MALI_VPU_RPC_VA_GRANULE SZ_16M

int mali_vpu_fw_get_region(enum mali_vpu_fw_region region,
			   mali_vpu_mmu_va *begin, mali_vpu_mmu_va *end)
{
	switch (region) {
	case MALI_VPU_FW_REGION_MSG_HOST:
		*begin = MVE_COMM_MSG_INQ_ADDR;
		*end = MVE_COMM_MSG_INQ_ADDR + MALI_VPU_PAGE_SIZE;
		break;
	case MALI_VPU_FW_REGION_MSG_MVE:
		*begin = MVE_COMM_MSG_OUTQ_ADDR;
		*end = MVE_COMM_MSG_OUTQ_ADDR + MALI_VPU_PAGE_SIZE;
		break;
	case MALI_VPU_FW_REGION_BUF_IN_HOST:
		*begin = MVE_COMM_BUF_INQ_ADDR;
		*end = MVE_COMM_BUF_INQ_ADDR + MALI_VPU_PAGE_SIZE;
		break;
	case MALI_VPU_FW_REGION_BUF_IN_MVE:
		*begin = MVE_COMM_BUF_INRQ_ADDR;
		*end = MVE_COMM_BUF_INRQ_ADDR + MALI_VPU_PAGE_SIZE;
		break;
	case MALI_VPU_FW_REGION_BUF_OUT_HOST:
		*begin = MVE_COMM_BUF_OUTQ_ADDR;
		*end = MVE_COMM_BUF_OUTQ_ADDR + MALI_VPU_PAGE_SIZE;
		break;
	case MALI_VPU_FW_REGION_BUF_OUT_MVE:
		*begin = MVE_COMM_BUF_OUTRQ_ADDR;
		*end = MVE_COMM_BUF_OUTRQ_ADDR + MALI_VPU_PAGE_SIZE;
		break;
	case MALI_VPU_FW_REGION_RPC:
		*begin = MVE_COMM_RPC_ADDR;
		*end = MVE_COMM_RPC_ADDR + MALI_VPU_PAGE_SIZE;
		break;
	case MALI_VPU_FW_REGION_CORE_0:
		*begin = MVE_MEM_REGION_FW_INSTANCE0_ADDR_BEGIN;
		*end = MVE_MEM_REGION_FW_INSTANCE0_ADDR_END;
		break;
	case MALI_VPU_FW_REGION_CORE_1:
		*begin = MVE_MEM_REGION_FW_INSTANCE1_ADDR_BEGIN;
		*end = MVE_MEM_REGION_FW_INSTANCE1_ADDR_END;
		break;
	case MALI_VPU_FW_REGION_CORE_2:
		*begin = MVE_MEM_REGION_FW_INSTANCE2_ADDR_BEGIN;
		*end = MVE_MEM_REGION_FW_INSTANCE2_ADDR_END;
		break;
	case MALI_VPU_FW_REGION_CORE_3:
		*begin = MVE_MEM_REGION_FW_INSTANCE3_ADDR_BEGIN;
		*end = MVE_MEM_REGION_FW_INSTANCE3_ADDR_END;
		break;
	case MALI_VPU_FW_REGION_CORE_4:
		*begin = MVE_MEM_REGION_FW_INSTANCE4_ADDR_BEGIN;
		*end = MVE_MEM_REGION_FW_INSTANCE4_ADDR_END;
		break;
	case MALI_VPU_FW_REGION_CORE_5:
		*begin = MVE_MEM_REGION_FW_INSTANCE5_ADDR_BEGIN;
		*end = MVE_MEM_REGION_FW_INSTANCE5_ADDR_END;
		break;
	case MALI_VPU_FW_REGION_CORE_6:
		*begin = MVE_MEM_REGION_FW_INSTANCE6_ADDR_BEGIN;
		*end = MVE_MEM_REGION_FW_INSTANCE6_ADDR_END;
		break;
	case MALI_VPU_FW_REGION_CORE_7:
		*begin = MVE_MEM_REGION_FW_INSTANCE7_ADDR_BEGIN;
		*end = MVE_MEM_REGION_FW_INSTANCE7_ADDR_END;
		break;
	case MALI_VPU_FW_REGION_PROTECTED:
		*begin = MVE_MEM_REGION_PROTECTED_ADDR_BEGIN;
		*end = MVE_MEM_REGION_PROTECTED_ADDR_END;
		break;
	case MALI_VPU_FW_REGION_FRAMEBUF:
		*begin = MVE_MEM_REGION_FRAMEBUF_ADDR_BEGIN;
		*end = MVE_MEM_REGION_FRAMEBUF_ADDR_END;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int mali_vpu_fw_map_region(struct mali_vpu_fw *fw, struct mali_vpu_fw_proto_page *proto_page,
				  enum mali_vpu_fw_region region)
{
	mali_vpu_mmu_va begin, end;
	dma_addr_t dma;
	int ret;

	/* Get virtual address where the message queue is to be mapped. */
	ret = mali_vpu_fw_get_region(region, &begin, &end);
	if (ret)
		return ret;

	/* Allocate backing memory and retain the original page. */
	dma = mali_vpu_mmu_alloc_page(fw->dev, &proto_page->linux_page, DMA_BIDIRECTIONAL);
	if (!proto_page->linux_page)
		return -ENOMEM;

	/* Memory map region. */
	ret = mali_vpu_mmu_map_dma(fw->mmu, begin, dma, MALI_VPU_PAGE_SIZE,
				   MALI_VPU_ATTR_SHARED_RW, MALI_VPU_ACCESS_READ_WRITE);
	if (ret) {
		mali_vpu_mmu_free_page(fw->dev, dma, proto_page->linux_page, DMA_BIDIRECTIONAL);
		proto_page->linux_page = NULL;
		return ret;
	}

	proto_page->cpu = page_address(proto_page->linux_page);
	proto_page->dma = dma;
	proto_page->va = begin;

	return 0;
}

static void mali_vpu_fw_unmap_region(struct mali_vpu_fw *fw,
				     struct mali_vpu_fw_proto_page *proto_page)
{
	if (!fw || !proto_page || !proto_page->linux_page)
		return;

	mali_vpu_mmu_unmap_va(fw->mmu, proto_page->va, MALI_VPU_PAGE_SIZE);
	mali_vpu_mmu_free_page(fw->dev, proto_page->dma, proto_page->linux_page,
			       DMA_BIDIRECTIONAL);
	memset(proto_page, 0, sizeof(*proto_page));
}

static void mali_vpu_fw_unmap_protocol(struct mali_vpu_fw *fw)
{
	if (!fw)
		return;

	mali_vpu_fw_unmap_region(fw, &fw->rpc);
	mali_vpu_fw_unmap_region(fw, &fw->buf_out_mve);
	mali_vpu_fw_unmap_region(fw, &fw->buf_out_host);
	mali_vpu_fw_unmap_region(fw, &fw->buf_in_mve);
	mali_vpu_fw_unmap_region(fw, &fw->buf_in_host);
	mali_vpu_fw_unmap_region(fw, &fw->msg_mve);
	mali_vpu_fw_unmap_region(fw, &fw->msg_host);
}

static int mali_vpu_fw_map_protocol(struct mali_vpu_fw *fw)
{
	int ret = 0;

	if (!fw)
		return -EINVAL;

	ret = mali_vpu_fw_map_region(fw, &fw->msg_host, MALI_VPU_FW_REGION_MSG_HOST);
	if (ret)
		return ret;

	ret = mali_vpu_fw_map_region(fw, &fw->msg_mve, MALI_VPU_FW_REGION_MSG_MVE);
	if (ret)
		goto err_unmap_protocol;

	ret = mali_vpu_fw_map_region(fw, &fw->buf_in_host, MALI_VPU_FW_REGION_BUF_IN_HOST);
	if (ret)
		goto err_unmap_protocol;

	ret = mali_vpu_fw_map_region(fw, &fw->buf_in_mve, MALI_VPU_FW_REGION_BUF_IN_MVE);
	if (ret)
		goto err_unmap_protocol;

	ret = mali_vpu_fw_map_region(fw, &fw->buf_out_host, MALI_VPU_FW_REGION_BUF_OUT_HOST);
	if (ret)
		goto err_unmap_protocol;

	ret = mali_vpu_fw_map_region(fw, &fw->buf_out_mve, MALI_VPU_FW_REGION_BUF_OUT_MVE);
	if (ret)
		goto err_unmap_protocol;

	ret = mali_vpu_fw_map_region(fw, &fw->rpc, MALI_VPU_FW_REGION_RPC);
	if (ret)
		goto err_unmap_protocol;

	return 0;

err_unmap_protocol:
	mali_vpu_fw_unmap_protocol(fw);

	return ret;
}

static int mali_vpu_fw_map_one_core(struct mali_vpu_fw *fw,
				    const struct mali_vpu_fw_bin *fw_bin,
				    u32 core)
{
	/* Map pages for the text and BSS segments for one core. */
	enum mali_vpu_fw_region region = MALI_VPU_FW_REGION_CORE_0 + core;
	mali_vpu_mmu_va begin, end, va;
	const struct mali_vpu_fw_header *hdr = fw_bin->header;
	u32 bss_count = core * fw_bin->bss_page_count;
	u32 shared_bss_count = 0;
	u32 master_rw_start, master_rw_size, bss_bitmap_size, i;
	bool is_shared;
	int ret;

	/*
	 * Get the base address where the pages for this core should be
	 * mapped.
	 */
	ret = mali_vpu_fw_get_region(region, &begin, &end);
	if (ret)
		return ret;

	/* Map text segment. */
	ret = mali_vpu_mmu_map_pages(fw->mmu, begin + MALI_VPU_FW_TEXT_BASE_ADDR,
				     fw->text_pages, MALI_VPU_ATTR_PRIVATE,
				     MALI_VPU_ACCESS_EXECUTABLE);
	if (ret)
		return ret;

	bss_bitmap_size = le32_to_cpu(hdr->bss_bitmap_size);
	master_rw_start = le32_to_cpu(hdr->master_rw_start_address);
	master_rw_size = le32_to_cpu(hdr->master_rw_size);

	/* Map bss shared and private pages. */
	va = le32_to_cpu(hdr->bss_start_address);
	for (i = 0; i < bss_bitmap_size; i++) {
		dma_addr_t dma;

		is_shared = (va >= master_rw_start &&
			     va < (master_rw_start + master_rw_size));

		if (is_shared) {
			dma = fw->shared_bss_pages->pages[shared_bss_count++].dma;
		} else if (mali_vpu_test_bit_le32(i, hdr->bss_bitmap)) {
			dma = fw->bss_pages->pages[bss_count++].dma;
		} else {
			va += MALI_VPU_PAGE_SIZE;
			continue;
		}

		ret = mali_vpu_mmu_map_dma(fw->mmu, begin + va, dma, MALI_VPU_PAGE_SIZE,
					   MALI_VPU_ATTR_PRIVATE, MALI_VPU_ACCESS_READ_WRITE);
		if (ret)
			return ret;

		va += MALI_VPU_PAGE_SIZE;
	}

	return 0;
}

static void mali_vpu_fw_unmap_core_regions(struct mali_vpu_fw *fw)
{
	enum mali_vpu_fw_region region;
	mali_vpu_mmu_va begin, end;
	u32 core;
	int ret;

	if (!fw)
		return;

	for (core = 0; core < fw->ncores; core++) {
		region = MALI_VPU_FW_REGION_CORE_0 + core;
		ret = mali_vpu_fw_get_region(region, &begin, &end);
		if (!ret)
			mali_vpu_mmu_unmap_va(fw->mmu, begin, end - begin);
	}

	if (!IS_ERR_OR_NULL(fw->text_pages)) {
		mali_vpu_mmu_free_pages(fw->text_pages);
		fw->text_pages = NULL;
	}

	if (!IS_ERR_OR_NULL(fw->bss_pages)) {
		mali_vpu_mmu_free_pages(fw->bss_pages);
		fw->bss_pages = NULL;
	}

	if (!IS_ERR_OR_NULL(fw->shared_bss_pages)) {
		mali_vpu_mmu_free_pages(fw->shared_bss_pages);
		fw->shared_bss_pages = NULL;
	}
}

static int mali_vpu_fw_map_core_regions(struct mali_vpu_fw *fw,
					const struct mali_vpu_fw_bin *fw_bin)
{
	const struct mali_vpu_fw_header *hdr;
	int ret = 0;
	u32 core;

	if (!fw || !fw_bin || !fw_bin->header || !fw_bin->fw)
		return -EINVAL;

	hdr = fw_bin->header;

	fw->text_pages = mali_vpu_mmu_alloc_pages(fw->dev, fw_bin->text_page_count, 0);
	if (IS_ERR(fw->text_pages)) {
		ret = PTR_ERR(fw->text_pages);
		goto err_unmap;
	}

	if (fw_bin->bss_page_count) {
		fw->bss_pages = mali_vpu_mmu_alloc_pages(fw->dev,
							 fw_bin->bss_page_count * fw->ncores, 0);
		if (IS_ERR(fw->bss_pages)) {
			ret = PTR_ERR(fw->bss_pages);
			goto err_unmap;
		}
	}

	if (fw_bin->shared_bss_page_count) {
		fw->shared_bss_pages = mali_vpu_mmu_alloc_pages(fw->dev,
								fw_bin->shared_bss_page_count, 0);
		if (IS_ERR(fw->shared_bss_pages)) {
			ret = PTR_ERR(fw->shared_bss_pages);
			goto err_unmap;
		}
	}

	for (core = 0; core < fw->ncores; core++) {
		ret = mali_vpu_fw_map_one_core(fw, fw_bin, core);
		if (ret)
			goto err_unmap;
	}

	ret = mali_vpu_mmu_write(fw->text_pages, fw_bin->fw->data,
				 le32_to_cpu(hdr->text_length));
	if (ret)
		goto err_unmap;

	return 0;

err_unmap:
	mali_vpu_fw_unmap_core_regions(fw);
	return ret;
}

static void mali_vpu_fw_clear_buffers(struct mali_vpu_fw *fw)
{
	struct mali_vpu_buffer *vpu_buf;
	unsigned long id;

	xa_for_each(&fw->inflight_buffers, id, vpu_buf) {
		xa_erase(&fw->inflight_buffers, id);
		vpu_buf->fw_buffer_id = 0;
		vpu_buf->attr = MALI_VPU_BUF_ATTR_NONE;
	}

	xa_destroy(&fw->inflight_buffers);
}

int mali_vpu_fw_construct(struct mali_vpu_fw *fw, const struct mali_vpu_fw_bin *fw_bin,
			  struct device *dev, struct mali_vpu_mmu *mmu, u32 ncores)
{
	int ret;

	if (!fw || !fw_bin || !dev || !mmu)
		return -EINVAL;

	if (ncores == 0 || ncores > MALI_VPU_FW_MAX_CORES)
		return -EINVAL;

	memset(fw, 0, sizeof(*fw));
	fw->dev = dev;
	fw->mmu = mmu;
	fw->ncores = ncores;
	xa_init_flags(&fw->inflight_buffers, XA_FLAGS_ALLOC1);
	fw->next_buffer_id = 1;
	hash_init(fw->rpc_mem);

	/* Initialise firmware core and protocol regions */
	ret = mali_vpu_fw_map_core_regions(fw, fw_bin);
	if (ret) {
		dev_err(fw->dev, "failed to map firmware core regions: %d\n", ret);
		return ret;
	}

	ret = mali_vpu_fw_map_protocol(fw);
	if (ret) {
		dev_err(fw->dev, "failed to map firmware protocol regions: %d\n", ret);
		goto err_unmap_core;
	}

	return 0;

err_unmap_core:
	mali_vpu_fw_unmap_core_regions(fw);
	return ret;
}

void mali_vpu_fw_cleanup(struct mali_vpu_fw *fw)
{
	struct mali_vpu_mmu_pages *pages;
	struct hlist_node *tmp;
	unsigned int bkt;

	if (!fw)
		return;

	mali_vpu_fw_clear_buffers(fw);

	mali_vpu_fw_unmap_protocol(fw);
	mali_vpu_fw_unmap_core_regions(fw);

	hash_for_each_safe(fw->rpc_mem, bkt, tmp, pages, node) {
		hash_del(&pages->node);
		mali_vpu_mmu_free_pages(pages);
	}

	memset(fw, 0, sizeof(*fw));
}

/*
 * Read arbitrary byte count from circular u32 queue payload area.
 * Return new read position in the queue.
 */
static u16 mali_vpu_fw_read32n(u32 *src, u16 offset, u32 *dst, size_t size)
{
	u32 v;

	for (; size >= sizeof(u32); size -= sizeof(u32)) {
		*dst++ = READ_ONCE(src[offset]);
		offset = (offset + 1) % MVE_COMM_QUEUE_SIZE_IN_WORDS;
	}

	if (size) {
		v = READ_ONCE(src[offset]);
		memcpy(dst, &v, size);
		offset = (offset + 1) % MVE_COMM_QUEUE_SIZE_IN_WORDS;
	}

	return offset;
}

static int mali_vpu_fw_read_msg(struct mali_vpu_fw *fw,
				struct mali_vpu_fw_proto_page *host_page,
				struct mali_vpu_fw_proto_page *mve_page,
				u16 *code, void *payload, size_t *size)
{
	struct mve_comm_area_host *host = host_page->cpu;
	struct mve_comm_area_mve *mve = mve_page->cpu;
	struct mve_msg_header hdr;
	u16 rpos, out_wpos;
	ssize_t capacity;

	if (!host || !mve)
		return -EINVAL;

	dma_sync_single_for_cpu(fw->dev, mve_page->dma, MALI_VPU_PAGE_SIZE,
				DMA_FROM_DEVICE);

	rpos = READ_ONCE(host->out_rpos);
	out_wpos = READ_ONCE(mve->out_wpos);

	if (rpos >= MVE_COMM_QUEUE_SIZE_IN_WORDS ||
	    out_wpos >= MVE_COMM_QUEUE_SIZE_IN_WORDS)
		return -EPROTO;

	/* Calculate how much data that is available in the buffer. */
	capacity = out_wpos - rpos;
	if (capacity < 0)
		capacity += MVE_COMM_QUEUE_SIZE_IN_WORDS;

	if (capacity <= 0)
		return 0;

	/* Firmware publishes out_wpos after writing the message. */
	dma_rmb();

	rpos = mali_vpu_fw_read32n(mve->out_data, rpos, (u32 *)&hdr, sizeof(hdr));

	/* Make sure there is enough space for both header and message. */
	capacity -= DIV_ROUND_UP(sizeof(hdr) + hdr.size, sizeof(u32));
	if (capacity < 0) {
		dev_err(fw->dev,
			"fw msg larger than capacity code=%u size=%u, wpos=%u, rpos=%u\n",
			hdr.code, hdr.size, out_wpos, rpos);
		return -EPROTO;
	}

	if (hdr.size > *size)
		return -EMSGSIZE;

	rpos = mali_vpu_fw_read32n(mve->out_data, rpos,
				   (u32 *)payload, hdr.size);

	/*
	 * Finish reading the message before publishing updated consumed queue
	 * positions to the firmware.
	 */
	dma_mb();
	WRITE_ONCE(host->out_rpos, rpos);

	dma_sync_single_for_device(fw->dev,
				   host_page->dma + offsetof(struct mve_comm_area_host, out_rpos),
				   sizeof(host->out_rpos), DMA_TO_DEVICE);

	*code = hdr.code;
	*size = hdr.size;

	return 1;
}

/*
 * Write arbitrary byte count to circular u32 queue payload area.
 * Return new write position in the queue.
 */
static u16 mali_vpu_fw_write32n(u32 *dst, u16 offset, const u32 *src, size_t size)
{
	u32 v;

	for (; size >= sizeof(u32); size -= sizeof(u32)) {
		WRITE_ONCE(dst[offset], *src++);
		offset = (offset + 1) % MVE_COMM_QUEUE_SIZE_IN_WORDS;
	}

	if (size) {
		v = 0;
		memcpy(&v, src, size);
		WRITE_ONCE(dst[offset], v);
		offset = (offset + 1) % MVE_COMM_QUEUE_SIZE_IN_WORDS;
	}

	return offset;
}

static int mali_vpu_fw_write_msg(struct mali_vpu_fw *fw,
				 struct mali_vpu_fw_proto_page *host_page,
				 struct mali_vpu_fw_proto_page *mve_page,
				 u16 code, const void *payload, size_t size)
{
	struct mve_comm_area_host *host = host_page->cpu;
	struct mve_comm_area_mve *mve = mve_page->cpu;
	struct mve_msg_header hdr = { .code = code, .size = size};
	u16 wpos, in_rpos;
	int capacity;

	if (!host || !mve)
		return -EINVAL;

	dma_sync_single_for_cpu(fw->dev,
				mve_page->dma + offsetof(struct mve_comm_area_mve, in_rpos),
				sizeof(mve->in_rpos), DMA_FROM_DEVICE);

	wpos = READ_ONCE(host->in_wpos);
	in_rpos = READ_ONCE(mve->in_rpos);

	if (wpos >= MVE_COMM_QUEUE_SIZE_IN_WORDS || in_rpos >= MVE_COMM_QUEUE_SIZE_IN_WORDS)
		return -EPROTO;

	/* Calculate how much space that is available in the buffer. */
	capacity = in_rpos - wpos;
	if (capacity <= 0)
		capacity += MVE_COMM_QUEUE_SIZE_IN_WORDS;

	/* Make sure there is enough space for both header and message. */
	capacity -= DIV_ROUND_UP(sizeof(hdr) + size, sizeof(u32));
	if (capacity <= 0)
		return -ENOMEM;

	wpos = mali_vpu_fw_write32n(host->in_data, wpos, (const u32 *)&hdr, sizeof(hdr));
	wpos = mali_vpu_fw_write32n(host->in_data, wpos, (const u32 *)payload, size);

	/*
	 * Publish the producer position only after the complete message has
	 * been written.
	 */
	dma_wmb();
	WRITE_ONCE(host->in_wpos, wpos);

	dma_sync_single_for_device(fw->dev, host_page->dma, sizeof(*host), DMA_TO_DEVICE);

	return 0;
}

static size_t mali_vpu_get_capacity_bytes(int rpos, int wpos)
{
	ssize_t capacity;

	capacity = wpos - rpos;
	if (capacity < 0)
		capacity += MVE_COMM_QUEUE_SIZE_IN_WORDS;

	return capacity * sizeof(u32);
}

static void mali_vpu_fw_print_debug(struct mali_vpu_fw *fw)
{
	struct mve_comm_area_host *host = fw->msg_host.cpu;
	struct mve_comm_area_mve *mve = fw->msg_mve.cpu;
	struct mve_msg_header hdr;
	u16 rpos, wpos;
	size_t capacity;

	dma_sync_single_for_cpu(fw->dev, fw->msg_mve.dma, MALI_VPU_PAGE_SIZE,
				DMA_FROM_DEVICE);

	dev_dbg(fw->dev,
		"dump message q. host={out_rpos=%u, in_wpos=%u}, mve={out_wpos=%u, in_rpos=%u}\n",
		host->out_rpos, host->in_wpos, mve->out_wpos, mve->in_rpos);

	rpos = READ_ONCE(host->out_rpos);
	wpos = READ_ONCE(mve->out_wpos);

	if (rpos >= MVE_COMM_QUEUE_SIZE_IN_WORDS ||
	    wpos >= MVE_COMM_QUEUE_SIZE_IN_WORDS)
		return;

	/* Firmware publishes out_wpos after writing the message. */
	dma_rmb();

	while ((capacity = mali_vpu_get_capacity_bytes(rpos, wpos)) >= sizeof(hdr)) {
		unsigned int pos;

		pos = mali_vpu_fw_read32n(mve->out_data, rpos, (u32 *)&hdr, sizeof(hdr));

		dev_dbg(fw->dev,
			"queue={rpos=%u, wpos=%u, capacity=%zu}, msg={code=%u, size=%u}\n",
			rpos, wpos, capacity, hdr.code, hdr.size);

		capacity = mali_vpu_get_capacity_bytes(pos, wpos);
		if (hdr.size > capacity) {
			dev_dbg(fw->dev,
				"size is larger than capacity. capacity=%zu, size=%u.\n",
				capacity, hdr.size);
			return;
		}

		rpos = (pos + DIV_ROUND_UP(hdr.size, sizeof(u32))) % MVE_COMM_QUEUE_SIZE_IN_WORDS;
	}
}

int mali_vpu_fw_send_job(struct mali_vpu_fw *fw, u16 cores, u16 frames)
{
	struct mve_request_job job = {
		.cores = cores,
		.frames = frames,
		.flags = 0,
	};

	return mali_vpu_fw_write_msg(fw, &fw->msg_host, &fw->msg_mve,
				     MVE_REQUEST_CODE_JOB, &job, sizeof(job));
}

int mali_vpu_fw_send_ping(struct mali_vpu_fw *fw)
{
	return mali_vpu_fw_write_msg(fw, &fw->msg_host, &fw->msg_mve,
				     MVE_REQUEST_CODE_PING, NULL, 0);
}

int mali_vpu_fw_send_state_change(struct mali_vpu_fw *fw, enum mali_vpu_fw_state state)
{
	u16 code;
	int ret;

	switch (state) {
	case MALI_VPU_FW_STATE_STOPPED:
		code = MVE_REQUEST_CODE_STOP;
		break;
	case MALI_VPU_FW_STATE_RUNNING:
		code = MVE_REQUEST_CODE_GO;
		break;
	default:
		return -EINVAL;
	}

	ret = mali_vpu_fw_write_msg(fw, &fw->msg_host, &fw->msg_mve, code, NULL, 0);
	if (!ret)
		fw->pending_response_count++;

	return ret;
}

static int mali_vpu_to_mve_nalu_format(s64 nalu_format, u32 *mve_nalu_format)
{
	switch (nalu_format) {
	case MALI_VPU_NALU_FORMAT_START_CODES:
		*mve_nalu_format = MVE_OPT_NALU_FORMAT_START_CODES;
		break;
	case MALI_VPU_NALU_FORMAT_ONE_NALU_PER_BUFFER:
		*mve_nalu_format = MVE_OPT_NALU_FORMAT_ONE_NALU_PER_BUFFER;
		break;
	case MALI_VPU_NALU_FORMAT_ONE_BYTE_LENGTH_FIELD:
		*mve_nalu_format = MVE_OPT_NALU_FORMAT_ONE_BYTE_LENGTH_FIELD;
		break;
	case MALI_VPU_NALU_FORMAT_TWO_BYTE_LENGTH_FIELD:
		*mve_nalu_format = MVE_OPT_NALU_FORMAT_TWO_BYTE_LENGTH_FIELD;
		break;
	case MALI_VPU_NALU_FORMAT_FOUR_BYTE_LENGTH_FIELD:
		*mve_nalu_format = MVE_OPT_NALU_FORMAT_FOUR_BYTE_LENGTH_FIELD;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int mali_vpu_fw_send_option(struct mali_vpu_fw *fw,
				   struct mve_request_set_option *mve_opt,
				   size_t size)
{
	int ret;

	ret = mali_vpu_fw_write_msg(fw, &fw->msg_host, &fw->msg_mve,
				    MVE_REQUEST_CODE_SET_OPTION, mve_opt,
				    offsetof(typeof(*mve_opt), data) + size);
	if (!ret)
		fw->pending_response_count++;

	return ret;
}

int mali_vpu_fw_send_set_option(struct mali_vpu_fw *fw,
				struct mali_vpu_fw_set_option *option)
{
	int ret;

	switch (option->code) {
	case MALI_VPU_FW_SET_NALU_FORMAT: {
		struct mve_request_set_option mve_opt;

		mve_opt.index = MVE_SET_OPT_INDEX_NALU_FORMAT;

		ret = mali_vpu_to_mve_nalu_format(option->nalu_format,
						  &mve_opt.data.arg);
		if (!ret)
			ret = mali_vpu_fw_send_option(fw, &mve_opt,
						      sizeof(mve_opt.data.arg));

		break;
	}
	case MALI_VPU_FW_SET_STREAM_ESCAPING: {
		struct mve_request_set_option mve_opt;

		mve_opt.index = MVE_SET_OPT_INDEX_STREAM_ESCAPING;
		mve_opt.data.arg = option->stream_escaping ? 1 : 0;

		ret = mali_vpu_fw_send_option(fw, &mve_opt,
					      sizeof(mve_opt.data.arg));
		break;
	}
	case MALI_VPU_FW_SET_HOST_PROTOCOL_PRINTS: {
		struct mve_request_set_option mve_opt;

		mve_opt.index = MVE_SET_OPT_INDEX_HOST_PROTOCOL_PRINTS;
		mve_opt.data.arg = option->host_protocol_prints ? 1 : 0;

		ret = mali_vpu_fw_send_option(fw, &mve_opt,
					      sizeof(mve_opt.data.arg));
		break;
	}
	case MALI_VPU_FW_SET_IGNORE_STREAM_HEADERS: {
		struct mve_request_set_option mve_opt;

		mve_opt.index = MVE_SET_OPT_INDEX_IGNORE_STREAM_HEADERS;
		mve_opt.data.arg = option->ignore_stream_headers ? 1 : 0;

		ret = mali_vpu_fw_send_option(fw, &mve_opt,
					      sizeof(mve_opt.data.arg));
		break;
	}
	case MALI_VPU_FW_SET_FRAME_REORDERING: {
		struct mve_request_set_option mve_opt;

		mve_opt.index = MVE_SET_OPT_INDEX_FRAME_REORDERING;
		mve_opt.data.arg = option->frame_reordering ? 1 : 0;

		ret = mali_vpu_fw_send_option(fw, &mve_opt,
					      sizeof(mve_opt.data.arg));
		break;
	}
	case MALI_VPU_FW_SET_INTBUF_SIZE: {
		struct mve_request_set_option mve_opt;

		mve_opt.index = MVE_SET_OPT_INDEX_INTBUF_SIZE;
		mve_opt.data.arg = option->intbuf_size;

		ret = mali_vpu_fw_send_option(fw, &mve_opt,
					      sizeof(mve_opt.data.arg));
		break;
	}
	default:
		ret = -EINVAL;
	}

	return ret;
}

int mali_vpu_fw_send_flush_src(struct mali_vpu_fw *fw)
{
	int ret;

	ret = mali_vpu_fw_write_msg(fw, &fw->msg_host, &fw->msg_mve, MVE_REQUEST_CODE_INPUT_FLUSH,
				    NULL, 0);
	if (!ret)
		fw->pending_response_count++;

	return ret;
}

int mali_vpu_fw_send_flush_dst(struct mali_vpu_fw *fw)
{
	int ret;

	ret = mali_vpu_fw_write_msg(fw, &fw->msg_host, &fw->msg_mve, MVE_REQUEST_CODE_OUTPUT_FLUSH,
				    NULL, 0);
	if (!ret)
		fw->pending_response_count++;

	return ret;
}

int mali_vpu_fw_send_eos(struct mali_vpu_fw *fw)
{
	struct mve_buffer_bitstream bs_buffer = {
		.host_handle        = 0,
		.bitstream_buf_addr = MVE_MEM_REGION_PROTECTED_ADDR_BEGIN,
		.bitstream_flags    = MVE_BUFFER_BITSTREAM_FLAG_EOS,
	};

	return mali_vpu_fw_write_msg(fw, &fw->buf_in_host, &fw->buf_in_mve,
				     MVE_BUFFER_CODE_BITSTREAM, &bs_buffer,
				     sizeof(bs_buffer));
}

static struct mali_vpu_buffer *
mali_vpu_fw_resolve_returned_buffer(struct mali_vpu_fw *fw,
				    u64 host_handle,
				    enum v4l2_buf_type expected_type)
{
	struct mali_vpu_buffer *vpu_buf;
	u32 id;

	if (!host_handle || host_handle > UINT_MAX)
		return ERR_PTR(-EPROTO);

	id = host_handle;

	vpu_buf = xa_load(&fw->inflight_buffers, id);
	if (!vpu_buf)
		return ERR_PTR(-EPROTO);

	if (vpu_buf->fw_buffer_id != id ||
	    vpu_buf->type != expected_type ||
	    vpu_buf->attr != MALI_VPU_BUF_ATTR_QUEUED)
		return ERR_PTR(-EPROTO);

	if (xa_erase(&fw->inflight_buffers, id) != vpu_buf)
		return ERR_PTR(-EPROTO);

	vpu_buf->fw_buffer_id = 0;
	vpu_buf->attr = MALI_VPU_BUF_ATTR_NONE;

	return vpu_buf;
}

static int mali_vpu_convert_buffer_frame(struct mali_vpu_fw *fw,
					 struct mali_vpu_fw_msg *msg,
					 u32 buf_type,
					 struct mve_buffer_frame *mve_frame_buf)
{
	struct mali_vpu_buffer *vpu_buf;
	unsigned int i;

	/* NULL host handle with the EOS flag denotes a firmware-generated EOS marker. */
	if (!mve_frame_buf->host_handle &&
	    mve_frame_buf->frame_flags & MVE_BUFFER_FRAME_FLAG_EOS) {
		msg->code = MALI_VPU_FW_CODE_EOS;
		return 0;
	}

	vpu_buf = mali_vpu_fw_resolve_returned_buffer(fw,
						      mve_frame_buf->host_handle,
						      buf_type);
	if (IS_ERR(vpu_buf))
		return PTR_ERR(vpu_buf);

	msg->code = MALI_VPU_FW_CODE_BUFFER;
	msg->data.vpu_buf = vpu_buf;

	if (buf_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
		vpu_buf->width = mve_frame_buf->visible_frame_width;
		vpu_buf->height = mve_frame_buf->visible_frame_height;

		if (vpu_buf->width == 0 || vpu_buf->height == 0) {
			for (i = 0; i < vpu_buf->nplanes; i++)
				(void)mali_vpu_buffer_set_plane_payload(vpu_buf, i, 0, 0);
		}
	}

	vpu_buf->user_data = mve_frame_buf->user_data_tag;
	vpu_buf->flags = 0;

	if (mve_frame_buf->frame_flags & MVE_BUFFER_FRAME_FLAG_EOS)
		vpu_buf->flags |=  MALI_VPU_BUFFER_EOS;

	if (mve_frame_buf->frame_flags & MVE_BUFFER_FRAME_FLAG_REJECTED)
		vpu_buf->flags |=  MALI_VPU_BUFFER_REJECTED;

	if (mve_frame_buf->frame_flags & MVE_BUFFER_FRAME_FLAG_CORRUPT)
		vpu_buf->flags |=  MALI_VPU_BUFFER_CORRUPT;

	return 0;
}

static int
mali_vpu_convert_buffer_bitstream(struct mali_vpu_fw *fw,
				  struct mali_vpu_fw_msg *msg,
				  u32 buf_type,
				  struct mve_buffer_bitstream *mve_stream_buf)
{
	struct mali_vpu_buffer *vpu_buf;

	/* NULL host handle with the EOS flag denotes a firmware-generated EOS marker. */
	if (!mve_stream_buf->host_handle &&
	    mve_stream_buf->bitstream_flags & MVE_BUFFER_BITSTREAM_FLAG_EOS) {
		msg->code = MALI_VPU_FW_CODE_EOS;
		return 0;
	}

	vpu_buf = mali_vpu_fw_resolve_returned_buffer(fw,
						      mve_stream_buf->host_handle,
						      buf_type);
	if (IS_ERR(vpu_buf))
		return PTR_ERR(vpu_buf);

	msg->code = MALI_VPU_FW_CODE_BUFFER;
	msg->data.vpu_buf = vpu_buf;

	vpu_buf->user_data = mve_stream_buf->user_data_tag;
	vpu_buf->flags = 0;

	if (mve_stream_buf->bitstream_flags & MVE_BUFFER_BITSTREAM_FLAG_EOS)
		vpu_buf->flags |= MALI_VPU_BUFFER_EOS;

	return 0;
}

static int mali_vpu_convert_buffer_param(struct mali_vpu_fw_msg *msg,
					 struct mve_buffer_param *p,
					 size_t size)
{
	if (size < sizeof(p->type))
		return -EPROTO;

	switch (p->type) {
	case MVE_BUFFER_PARAM_TYPE_COLOUR_DESCRIPTION:
		/* The colour-description payload is currently ignored. */
		msg->code = MALI_VPU_FW_CODE_COLOR_DESC;
		break;
	case MVE_BUFFER_PARAM_TYPE_DPB_HELD_FRAMES:
		if (size < offsetofend(struct mve_buffer_param, data.arg))
			return -EPROTO;

		msg->code = MALI_VPU_FW_CODE_DPB_HELD_FRAMES;
		msg->data.dpb_held_frames = p->data.arg;
		break;
	default:
		msg->code = MALI_VPU_FW_CODE_UNKNOWN;
		break;
	}

	return 1;
}

static int mali_vpu_fw_get_buffer(struct mali_vpu_fw *fw,
				  struct mali_vpu_fw_proto_page *buf_host_page,
				  struct mali_vpu_fw_proto_page *buf_mve_page,
				  struct mali_vpu_fw_msg *msg, u32 buf_type)
{
	u16 code;
	union {
		struct mve_buffer_frame frame;
		struct mve_buffer_bitstream bitstream;
	} fw_msg;
	size_t size = sizeof(fw_msg);
	int ret;

	ret = mali_vpu_fw_read_msg(fw, buf_host_page, buf_mve_page, &code,
				   &fw_msg, &size);
	if (ret <= 0)
		return ret;

	switch (code) {
	case MVE_BUFFER_CODE_FRAME:
		if (size != sizeof(fw_msg.frame) ||
		    (!fw_msg.frame.host_handle &&
		     !(fw_msg.frame.frame_flags & MVE_BUFFER_FRAME_FLAG_EOS)))
			return -EPROTO;

		ret = mali_vpu_convert_buffer_frame(fw, msg, buf_type, &fw_msg.frame);
		if (ret)
			return ret;
		break;
	case MVE_BUFFER_CODE_BITSTREAM:
		if (size != sizeof(fw_msg.bitstream) ||
		    (!fw_msg.bitstream.host_handle &&
		     !(fw_msg.bitstream.bitstream_flags & MVE_BUFFER_BITSTREAM_FLAG_EOS)))
			return -EPROTO;

		ret = mali_vpu_convert_buffer_bitstream(fw, msg, buf_type,
							&fw_msg.bitstream);
		if (ret)
			return ret;
		break;
	default:
		dev_dbg(fw->dev, "unknown buf code. code: %u.\n", code);
		break;
	}

	return 1;
}

int mali_vpu_fw_parse_msg(struct mali_vpu_fw *fw, struct mali_vpu_fw_msg *msg)
{
	u16 code;
	union {
		struct mve_response_set_option_confirm set_option_confirm;
		struct mve_response_set_option_fail set_option_fail;
		struct mve_response_state_change state_change;
		struct mve_response_frame_alloc_parameters alloc_params;
		struct mve_response_sequence_parameters seq_params;
		struct mve_buffer_param buffer_param;
		struct mve_response_event event;
		struct mve_response_error error;
	} fw_msg = {};
	size_t size = sizeof(fw_msg);
	struct mali_vpu_fw_proto_page *buf_host_page;
	struct mali_vpu_fw_proto_page *buf_mve_page;
	int ret;

	if (!fw || !msg)
		return -EINVAL;

	ret = mali_vpu_fw_read_msg(fw, &fw->msg_host, &fw->msg_mve, &code,
				   &fw_msg, &size);
	if (ret <= 0)
		return ret; /* 0 = no msg, <0 = error */

	memset(msg, 0, sizeof(*msg));

	switch (code) {
	case MVE_RESPONSE_CODE_SWITCHED_IN:
		msg->code = MALI_VPU_FW_CODE_SWITCH_IN;
		break;
	case MVE_RESPONSE_CODE_SWITCHED_OUT:
		msg->code = MALI_VPU_FW_CODE_SWITCH_OUT;
		break;
	case MVE_RESPONSE_CODE_JOB_DEQUEUED:
		msg->code = MALI_VPU_FW_CODE_JOB;
		break;
	case MVE_RESPONSE_CODE_PONG:
		msg->code = MALI_VPU_FW_CODE_PONG;
		break;
	case MVE_RESPONSE_CODE_IDLE:
		msg->code = MALI_VPU_FW_CODE_IDLE;
		break;
	case MVE_RESPONSE_CODE_STATE_CHANGE: {
		if (size != sizeof(fw_msg.state_change))
			return -EPROTO;

		msg->code = MALI_VPU_FW_CODE_STATE_CHANGE;

		switch (fw_msg.state_change.new_state) {
		case MVE_STATE_STOPPED:
			msg->data.state = MALI_VPU_FW_STATE_STOPPED;
			break;
		case MVE_STATE_RUNNING:
			msg->data.state = MALI_VPU_FW_STATE_RUNNING;
			break;
		default:
			return -EPROTO;
		}

		if (fw->pending_response_count == 0)
			return -EPROTO;

		fw->pending_response_count--;
		break;
	}
	case MVE_RESPONSE_CODE_SET_OPTION_CONFIRM:
		if (size != sizeof(fw_msg.set_option_confirm))
			return -EPROTO;

		msg->code = MALI_VPU_FW_CODE_SET_OPTION;

		if (fw->pending_response_count == 0)
			return -EPROTO;

		fw->pending_response_count--;
		break;
	case MVE_RESPONSE_CODE_SET_OPTION_FAIL: {
		if (size != sizeof(fw_msg.set_option_fail))
			return -EPROTO;

		dev_err(fw->dev,
			"firmware set option failed. index=%u, msg=%.*s.\n",
			fw_msg.set_option_fail.index,
			(int)sizeof(fw_msg.set_option_fail.message),
			fw_msg.set_option_fail.message);

		msg->code = MALI_VPU_FW_CODE_SET_OPTION_FAIL;

		if (fw->pending_response_count == 0)
			return -EPROTO;

		fw->pending_response_count--;
		break;
	}
	case MVE_RESPONSE_CODE_INPUT:
		buf_host_page = &fw->buf_in_host;
		buf_mve_page = &fw->buf_in_mve;

		ret = mali_vpu_fw_get_buffer(fw, buf_host_page, buf_mve_page, msg,
					     V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
		break;
	case MVE_RESPONSE_CODE_OUTPUT:
		buf_host_page = &fw->buf_out_host;
		buf_mve_page = &fw->buf_out_mve;

		ret = mali_vpu_fw_get_buffer(fw, buf_host_page, buf_mve_page, msg,
					     V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
		break;
	case MVE_RESPONSE_CODE_FRAME_ALLOC_PARAM:
		if (size != sizeof(fw_msg.alloc_params))
			return -EPROTO;

		msg->code = MALI_VPU_FW_CODE_ALLOC_PARAM;
		msg->data.alloc_params.width = fw_msg.alloc_params.planar_alloc_frame_width;
		msg->data.alloc_params.height = fw_msg.alloc_params.planar_alloc_frame_height;
		break;
	case MVE_RESPONSE_CODE_SEQUENCE_PARAMETERS:
		if (size != sizeof(fw_msg.seq_params))
			return -EPROTO;

		msg->code = MALI_VPU_FW_CODE_SEQUENCE_PARAMS;
		msg->data.seq_params.planar.buffers_min = fw_msg.seq_params.num_buffers_planar;
		msg->data.seq_params.bitdepth.luma = fw_msg.seq_params.bitdepth_luma;
		msg->data.seq_params.bitdepth.chroma = fw_msg.seq_params.bitdepth_chroma;
		msg->data.seq_params.interlaced = fw_msg.seq_params.interlace;
		break;
	case MVE_RESPONSE_CODE_INPUT_FLUSHED:
		msg->code = MALI_VPU_FW_CODE_INPUT_FLUSH;

		if (fw->pending_response_count == 0)
			return -EPROTO;

		fw->pending_response_count--;
		break;
	case MVE_RESPONSE_CODE_OUTPUT_FLUSHED:
		msg->code = MALI_VPU_FW_CODE_OUTPUT_FLUSH;

		if (fw->pending_response_count == 0)
			return -EPROTO;

		fw->pending_response_count--;
		break;
	case MVE_BUFFER_CODE_PARAM:
		ret =  mali_vpu_convert_buffer_param(msg, &fw_msg.buffer_param, size);
		break;
	case MVE_RESPONSE_CODE_EVENT: {
		size_t message_offset = offsetof(struct mve_response_event, event_data.message);
		size_t message_size;

		msg->code = MALI_VPU_FW_CODE_EVENT;

		if (size < sizeof(fw_msg.event.event_code))
			return -EPROTO;

		switch (fw_msg.event.event_code) {
		case MVE_EVENT_ERROR_STREAM_CORRUPT:
			if (size <= message_offset)
				return -EPROTO;

			message_size = min(size - message_offset,
					   sizeof(fw_msg.event.event_data.message));

			dev_warn_ratelimited(fw->dev,
					     "firmware reports corrupt stream: %.*s\n",
					     (int)message_size,
					     fw_msg.event.event_data.message);

			break;
		case MVE_EVENT_ERROR_STREAM_NOT_SUPPORTED:
			if (size <= message_offset)
				return -EPROTO;

			message_size = min(size - message_offset,
					   sizeof(fw_msg.event.event_data.message));
			dev_warn_ratelimited(fw->dev,
					     "firmware reports unsupported stream: %.*s\n",
					     (int)message_size,
					     fw_msg.event.event_data.message);
			msg->code = MALI_VPU_FW_CODE_STREAM_NOT_SUPPORTED;
			break;
		default:
			break;
		}
		break;
	}
	case MVE_RESPONSE_CODE_ERROR: {
		size_t message_offset = offsetof(struct mve_response_error, message);
		size_t message_size;

		if (size < message_offset + 1 || size > sizeof(fw_msg.error))
			return -EPROTO;

		message_size = size - message_offset;
		if (fw_msg.error.message[message_size - 1] != '\0')
			return -EPROTO;

		msg->code = MALI_VPU_FW_CODE_ERROR;

		switch (fw_msg.error.error_code) {
		case MVE_ERROR_ABORT:
			msg->data.error.error_code = MALI_VPU_FW_ERROR_ABORT;
			break;
		case MVE_ERROR_OUT_OF_MEMORY:
			msg->data.error.error_code = MALI_VPU_FW_ERROR_OUT_OF_MEMORY;
			break;
		case MVE_ERROR_ASSERT:
			msg->data.error.error_code = MALI_VPU_FW_ERROR_ASSERT;
			break;
		case MVE_ERROR_UNSUPPORTED:
			msg->data.error.error_code = MALI_VPU_FW_ERROR_UNSUPPORTED;
			break;
		case MVE_ERROR_INVALID_BUFFER:
			msg->data.error.error_code = MALI_VPU_FW_ERROR_INVALID_BUFFER;
			break;
		case MVE_ERROR_INVALID_STATE:
			msg->data.error.error_code = MALI_VPU_FW_ERROR_INVALID_STATE;
			break;
		case MVE_ERROR_WATCHDOG:
			msg->data.error.error_code = MALI_VPU_FW_ERROR_WATCHDOG;
			break;
		default:
			msg->data.error.error_code = MALI_VPU_FW_ERROR_UNKNOWN;
			dev_warn(fw->dev, "unsupported fw error code. code=%u.\n",
				 fw_msg.error.error_code);
			break;
		}

		strscpy(msg->data.error.message, fw_msg.error.message,
			min(sizeof(msg->data.error.message), sizeof(fw_msg.error.message)));

		break;
	}
	default:
		dev_warn(fw->dev,
			 "unknown fw message code. code=%u, size=%zu.\n",
			 code, size);
		msg->code = MALI_VPU_FW_CODE_UNKNOWN;
		mali_vpu_fw_print_debug(fw);
		break;
	}

	return ret;
}

static int mali_vpu_fw_track_buffer(struct mali_vpu_fw *fw,
				    struct mali_vpu_buffer *vpu_buf)
{
	u32 id;
	int ret;

	if (vpu_buf->fw_buffer_id)
		return -EBUSY;

	ret = xa_alloc_cyclic(&fw->inflight_buffers, &id, vpu_buf,
			      XA_LIMIT(1, UINT_MAX),
			      &fw->next_buffer_id, GFP_KERNEL);
	if (ret)
		return ret;

	vpu_buf->fw_buffer_id = id;

	return 0;
}

static void mali_vpu_fw_untrack_buffer(struct mali_vpu_fw *fw,
				       struct mali_vpu_buffer *vpu_buf)
{
	void *entry;

	if (!vpu_buf->fw_buffer_id)
		return;

	entry = xa_erase(&fw->inflight_buffers, vpu_buf->fw_buffer_id);
	WARN_ON_ONCE(entry && entry != vpu_buf);

	vpu_buf->fw_buffer_id = 0;
}

static int mali_vpu_fw_put_buffer_frame(struct mali_vpu_fw *fw,
					struct mali_vpu_fw_msg *fw_msg,
					struct mali_vpu_fw_proto_page *host_page,
					struct mali_vpu_fw_proto_page *mve_page)
{
	struct mve_buffer_frame mve_frame_buf = { 0 };
	struct mali_vpu_buffer *vpu_buf = fw_msg->data.vpu_buf;
	struct mve_buffer_frame_planar *planar;
	u32 i;

	mve_frame_buf.host_handle = vpu_buf->fw_buffer_id;

	mve_frame_buf.format = vpu_buf->format->mve_frame_fmt;
	planar = &mve_frame_buf.data.planar;

	for (i = 0; i < vpu_buf->nplanes; i++) {
		struct mali_vpu_buffer_plane *plane = &vpu_buf->planes[i];

		if (plane->stride > 0)
			planar->plane_top[i] = mali_vpu_buffer_plane_va(vpu_buf, i);

		planar->stride[i] = plane->stride;
		planar->max_frame_width = vpu_buf->width;
		planar->max_frame_height = vpu_buf->height;
	}

	return mali_vpu_fw_write_msg(fw, host_page, mve_page, MVE_BUFFER_CODE_FRAME,
				     &mve_frame_buf, sizeof(mve_frame_buf));
}

static int mali_vpu_fw_put_buffer_bitstream(struct mali_vpu_fw *fw,
					    struct mali_vpu_fw_msg *fw_msg,
					    struct mali_vpu_fw_proto_page *host_page,
					    struct mali_vpu_fw_proto_page *mve_page)
{
	struct mve_buffer_bitstream mve_stream_buf = { 0 };
	struct mali_vpu_buffer *vpu_buf = fw_msg->data.vpu_buf;

	if (V4L2_TYPE_IS_OUTPUT(vpu_buf->type))
		mve_stream_buf.bitstream_filled_len = vpu_buf->planes[0].payload_size;

	mve_stream_buf.host_handle = vpu_buf->fw_buffer_id;
	mve_stream_buf.user_data_tag = vpu_buf->user_data;
	mve_stream_buf.bitstream_alloc_bytes = mali_vpu_buffer_plane_size(vpu_buf, 0);
	mve_stream_buf.bitstream_offset = vpu_buf->planes[0].offset;
	mve_stream_buf.bitstream_buf_addr = mali_vpu_buffer_plane_va(vpu_buf, 0);

	if (vpu_buf->flags & MALI_VPU_BUFFER_EOS)
		mve_stream_buf.bitstream_flags |= MVE_BUFFER_BITSTREAM_FLAG_EOS;

	return mali_vpu_fw_write_msg(fw, host_page, mve_page, MVE_BUFFER_CODE_BITSTREAM,
				     &mve_stream_buf, sizeof(mve_stream_buf));
}

int mali_vpu_fw_send_buffer(struct mali_vpu_fw *fw, struct mali_vpu_fw_msg *fw_msg)
{
	struct mali_vpu_buffer *vpu_buf = fw_msg->data.vpu_buf;
	struct mali_vpu_fw_proto_page *host;
	struct mali_vpu_fw_proto_page *mve;
	int ret;

	if (V4L2_TYPE_IS_OUTPUT(fw_msg->data.vpu_buf->type)) {
		host = &fw->buf_in_host;
		mve = &fw->buf_in_mve;
	} else {
		host = &fw->buf_out_host;
		mve = &fw->buf_out_mve;
	}

	ret = mali_vpu_fw_track_buffer(fw, vpu_buf);
	if (ret)
		return ret;

	if (V4L2_TYPE_IS_OUTPUT(fw_msg->data.vpu_buf->type))
		ret = mali_vpu_fw_put_buffer_bitstream(fw, fw_msg, host, mve);
	else
		ret = mali_vpu_fw_put_buffer_frame(fw, fw_msg, host, mve);

	if (ret)
		mali_vpu_fw_untrack_buffer(fw, vpu_buf);

	return ret;
}

static void mali_vpu_fw_dbg_rpc(struct mali_vpu_fw *fw, const char *direction,
				const struct mve_rpc_communication_area *rpc)
{
	dev_dbg(fw->dev, "rpc %s: state=%u call_id=%u size=%u\n",
		direction, rpc->state, rpc->call_id, rpc->size);
}

static struct mali_vpu_mmu_pages *mali_vpu_find_pages(struct mali_vpu_fw *fw,
						      mali_vpu_mmu_va va)
{
	struct mali_vpu_mmu_pages *pages;

	hash_for_each_possible(fw->rpc_mem, pages, node, va) {
		if (pages->va == va)
			return pages;
	}

	return NULL;
}

static void mali_vpu_rpc_mem_resize(struct mali_vpu_fw *fw,
				    struct mve_rpc_communication_area *rpc_area)
{
	union mve_rpc_params *p = &rpc_area->params;
	struct mali_vpu_mmu_pages *pages;
	mali_vpu_mmu_va va = 0;
	int ret;

	pages = mali_vpu_find_pages(fw, p->mem_resize.ve_pointer);
	if (pages) {
		size_t npages;

		/* Resize the allocated pages. */
		npages = DIV_ROUND_UP(p->mem_resize.new_size, MALI_VPU_PAGE_SIZE);
		ret = mali_vpu_mmu_resize_pages(pages, npages);
		if (ret) {
			dev_warn(fw->dev,
				 "failed to resize RPC mapped pages. ret=%d\n",
				 ret);
			goto err;
		}

		va = pages->va;
	} else {
		dev_warn(fw->dev, "could not find pages for RPC resize. va=0x%x\n",
			 p->mem_resize.ve_pointer);
	}

	dev_dbg(fw->dev, "RPC resize memory. va=0x%x, new_size=%u\n",
		p->mem_resize.ve_pointer, p->mem_resize.new_size);

err:
	/*
	 * The memory RPC ABI returns the resulting firmware VA. Firmware treats
	 * a zero VA as allocation failure and reports MVE_ERROR_OUT_OF_MEMORY.
	 * The RPC itself was still handled successfully.
	 */
	rpc_area->size = sizeof(u32);
	p->data[0] = va;
}

static void mali_vpu_rpc_mem_alloc(struct mali_vpu_fw *fw,
				   struct mve_rpc_communication_area *rpc_area)
{
	union mve_rpc_params *p = &rpc_area->params;
	enum mali_vpu_fw_region region;
	struct mali_vpu_mmu_pages *pages;
	size_t npages, max_pages;
	mali_vpu_mmu_va end, begin = 0;
	int ret;

	if (!p->mem_alloc.size ||
	    p->mem_alloc.size > p->mem_alloc.max_size) {
		dev_warn(fw->dev,
			 "invalid RPC allocation:size=%u max_size=%u alignment=%u\n",
			 p->mem_alloc.size, p->mem_alloc.max_size,
			 p->mem_alloc.log2_alignment);
		goto err;
	}

	switch (p->mem_alloc.region) {
	case MVE_MEM_REGION_PROTECTED:
		region = MALI_VPU_FW_REGION_PROTECTED;
		break;
	case MVE_MEM_REGION_OUTBUF:
		region = MALI_VPU_FW_REGION_FRAMEBUF;
		break;
	default:
		dev_err(fw->dev,
			"unsupported RPC mem alloc region. region=%u\n",
			p->mem_alloc.region);
		goto err;
	}

	ret = mali_vpu_fw_get_region(region, &begin, &end);
	if (ret)
		goto err;

	npages = DIV_ROUND_UP(p->mem_alloc.size, MALI_VPU_PAGE_SIZE);
	max_pages = DIV_ROUND_UP(p->mem_alloc.max_size, MALI_VPU_PAGE_SIZE);

	pages = mali_vpu_mmu_alloc_pages(fw->dev, npages, max_pages);
	if (IS_ERR(pages)) {
		begin = 0;
		goto err;
	}

	ret = -EINVAL;
	while (begin < end) {
		ret = mali_vpu_mmu_map_pages(fw->mmu, begin, pages,
					     MALI_VPU_ATTR_SHARED_RW,
					     MALI_VPU_ACCESS_READ_WRITE);
		if (!ret)
			break;

		begin += MALI_VPU_RPC_VA_GRANULE;
	}

	if (ret) {
		dev_err(fw->dev,
			"failed to find memory region for RPC alloc.\n");
		mali_vpu_mmu_free_pages(pages);
		begin = 0;
		goto err;
	}

	hash_add(fw->rpc_mem, &pages->node, pages->va);
	begin = pages->va;

	dev_dbg(fw->dev,
		"RPC alloc memory. size=%u, max_size=%u, region=%u, npages=%zu, begin=0x%x\n",
		p->mem_alloc.size, p->mem_alloc.max_size, p->mem_alloc.region, npages, begin);

err:
	rpc_area->size = sizeof(u32);
	p->data[0] = begin;
}

static void mali_vpu_rpc_mem_free(struct mali_vpu_fw *fw,
				  struct mve_rpc_communication_area *rpc_area)
{
	union mve_rpc_params *p = &rpc_area->params;
	struct mali_vpu_mmu_pages *pages;

	pages = mali_vpu_find_pages(fw, p->mem_free.ve_pointer);
	if (pages) {
		hash_del(&pages->node);
		mali_vpu_mmu_free_pages(pages);
	} else {
		dev_warn(fw->dev,
			 "could not find pages for RPC free. va=0x%x\n",
			 p->mem_free.ve_pointer);
	}

	dev_dbg(fw->dev, "RPC free memory. va=0x%x\n", p->mem_free.ve_pointer);

	rpc_area->size = 0;
}

int mali_vpu_fw_handle_rpc(struct mali_vpu_fw *fw, bool *flush_mmu)
{
	struct mve_rpc_communication_area *rpc_area = fw->rpc.cpu;
	int ret = 0;

	if (flush_mmu)
		*flush_mmu = false;

	dma_sync_single_for_cpu(fw->dev, fw->rpc.dma, sizeof(*rpc_area),
				DMA_FROM_DEVICE);

	if (READ_ONCE(rpc_area->state) == MVE_RPC_STATE_PARAM) {
		/*
		 * Firmware publishes param only after writing the RPC request.
		 */
		dma_rmb();

		ret = 1;

		mali_vpu_fw_dbg_rpc(fw, "fw->host", rpc_area);

		switch (rpc_area->call_id) {
		case MVE_RPC_FUNCTION_DEBUG_PRINTF: {
			if (!rpc_area->size ||
			    rpc_area->size > sizeof(rpc_area->params.debug_print.string)) {
				ret = -EPROTO;
				rpc_area->size = 0;
				break;
			}

			const char *s = rpc_area->params.debug_print.string;
			size_t len = strnlen(s, rpc_area->size);

			/* Strip trailing characters */
			while (len && (s[len - 1] == '\n' || s[len - 1] == '\r'))
				len--;

			dev_info(fw->dev,
				 "firmware DEBUG Print: %.*s\n", (int)len, s);
			break;
		}
		case MVE_RPC_FUNCTION_MEM_ALLOC: {
			if (rpc_area->size != sizeof(rpc_area->params.mem_alloc)) {
				ret = -EPROTO;
				break;
			}

			mali_vpu_rpc_mem_alloc(fw, rpc_area);
			break;
		}
		case MVE_RPC_FUNCTION_MEM_RESIZE: {
			if (rpc_area->size != sizeof(rpc_area->params.mem_resize)) {
				ret = -EPROTO;
				break;
			}

			mali_vpu_rpc_mem_resize(fw, rpc_area);
			if (flush_mmu)
				*flush_mmu = true;
			break;
		}
		case MVE_RPC_FUNCTION_MEM_FREE: {
			if (rpc_area->size != sizeof(rpc_area->params.mem_free)) {
				ret = -EPROTO;
				break;
			}

			mali_vpu_rpc_mem_free(fw, rpc_area);
			if (flush_mmu)
				*flush_mmu = true;
			break;
		}
		default:
			dev_warn_ratelimited(fw->dev,
					     "unsupported RPC request. call_id=%u\n",
					     rpc_area->call_id);
			ret = -EOPNOTSUPP;
			break;
		}

		/*
		 * Make sure the whole RPC message body has been written before
		 * the RPC message area is returned to the firmware.
		 */
		dma_wmb();
		WRITE_ONCE(rpc_area->state, MVE_RPC_STATE_RETURN);

		dma_sync_single_for_device(fw->dev, fw->rpc.dma,
					   sizeof(*rpc_area), DMA_TO_DEVICE);

		/* Log RPC response. */
		mali_vpu_fw_dbg_rpc(fw, "host->fw", rpc_area);
	}

	return ret;
}
