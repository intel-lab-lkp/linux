// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 ARM Limited.
 */

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/string.h>

#include "mali-vpu-firmware.h"
#include "mali-vpu-firmware-cache.h"
#include "mali-vpu-mmu.h"
#include "mali-vpu-protocol-def-v3.h"

#define MALI_VPU_FW_MAX_CORES 8

static int mali_vpu_fw_get_region(enum mali_vpu_fw_region region,
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
	if (!fw)
		return;

	mali_vpu_fw_unmap_protocol(fw);
	mali_vpu_fw_unmap_core_regions(fw);
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

	return mali_vpu_fw_write_msg(fw, &fw->msg_host, &fw->msg_mve, code, NULL, 0);
}

int mali_vpu_fw_parse_msg(struct mali_vpu_fw *fw, struct mali_vpu_fw_msg *msg)
{
	u16 code;
	union {
		struct mve_response_state_change state_change;
		struct mve_response_error error;
	} fw_msg = {};
	size_t size = sizeof(fw_msg);
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
