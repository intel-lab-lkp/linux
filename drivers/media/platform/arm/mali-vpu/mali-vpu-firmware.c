// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 ARM Limited.
 */

#include <linux/device.h>
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
