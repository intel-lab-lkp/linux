// SPDX-License-Identifier: GPL-2.0
// Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved.

#include <linux/dma-mapping.h>
#include <linux/delay.h>

#include "hinic3_common.h"

int hinic3_dma_zalloc_coherent_align(struct device *dev, u32 size, u32 align,
				     gfp_t flag,
				     struct hinic3_dma_addr_align *mem_align)
{
	dma_addr_t paddr, align_paddr;
	void *vaddr, *align_vaddr;
	u32 real_size = size;

	vaddr = dma_alloc_coherent(dev, real_size, &paddr, flag);
	if (!vaddr)
		return -ENOMEM;

	align_paddr = ALIGN(paddr, align);
	if (align_paddr == paddr) {
		align_vaddr = vaddr;
		goto out;
	}

	dma_free_coherent(dev, real_size, vaddr, paddr);

	/* realloc memory for align */
	real_size = size + align;
	vaddr = dma_alloc_coherent(dev, real_size, &paddr, flag);
	if (!vaddr)
		return -ENOMEM;

	align_paddr = ALIGN(paddr, align);
	align_vaddr = vaddr + (align_paddr - paddr);

out:
	mem_align->real_size = real_size;
	mem_align->ori_vaddr = vaddr;
	mem_align->ori_paddr = paddr;
	mem_align->align_vaddr = align_vaddr;
	mem_align->align_paddr = align_paddr;

	return 0;
}

void hinic3_dma_free_coherent_align(struct device *dev,
				    struct hinic3_dma_addr_align *mem_align)
{
	dma_free_coherent(dev, mem_align->real_size,
			  mem_align->ori_vaddr, mem_align->ori_paddr);
}

int hinic3_wait_for_timeout(void *priv_data, wait_cpl_handler handler,
			    u32 wait_total_ms, u32 wait_once_us)
{
	/* Take 9/10 * wait_once_us as the minimum sleep time of usleep_range */
	u32 usleep_min = wait_once_us - wait_once_us / 10;
	enum hinic3_wait_return ret;
	unsigned long end;

	end = jiffies + msecs_to_jiffies(wait_total_ms);
	do {
		ret = handler(priv_data);
		if (ret == WAIT_PROCESS_CPL)
			return 0;

		/* Sleep more than 20ms using msleep is accurate */
		if (wait_once_us >= 20 * USEC_PER_MSEC)
			msleep(wait_once_us / USEC_PER_MSEC);
		else
			usleep_range(usleep_min, wait_once_us);
	} while (time_before(jiffies, end));

	ret = handler(priv_data);
	if (ret == WAIT_PROCESS_CPL)
		return 0;

	return -ETIMEDOUT;
}

/* Data provided to/by cmdq is arranged in structs with little endian fields but
 * every dword (32bits) should be swapped since HW swaps it again when it
 * copies it from/to host memory. This is a mandatory swap regardless of the
 * CPU endianness.
 */
void cmdq_buf_swab32(void *data, int len)
{
	int i, chunk_sz = sizeof(u32);
	int data_len = len;
	u32 *mem = data;

	data_len = data_len / chunk_sz;

	for (i = 0; i < data_len; i++)
		mem[i] = swab32(mem[i]);
}
