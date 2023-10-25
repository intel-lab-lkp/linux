// SPDX-License-Identifier: MIT
/*
 * AMD Trusted Execution Environment (TEE) interface
 *
 * Author: Rijo Thomas <Rijo-john.Thomas@amd.com>
 * Author: Devaraj Rangasamy <Devaraj.Rangasamy@amd.com>
 *
 * Copyright (C) 2019,2021 Advanced Micro Devices, Inc.
 */

#include <linux/bitfield.h>
#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/dma-direct.h>
#include <linux/iommu.h>
#include <linux/gfp.h>
#include <linux/psp.h>
#include <linux/psp-tee.h>

#include "psp-dev.h"
#include "tee-dev.h"

static bool psp_dead;

struct psp_tee_buffer *psp_tee_alloc_buffer(unsigned long size, gfp_t gfp)
{
	struct psp_device *psp = psp_get_master_device();
	struct psp_tee_buffer *tee_buf;
	struct iommu_domain *dom;

	if (!psp || !size)
		return NULL;

	tee_buf = kzalloc(sizeof(*tee_buf), GFP_KERNEL);
	if (!tee_buf)
		return NULL;

	tee_buf->vaddr = dma_alloc_coherent(psp->dev, size, &tee_buf->dma, gfp);
	if (!tee_buf->vaddr || !tee_buf->dma) {
		kfree(tee_buf);
		return NULL;
	}

	tee_buf->size = size;

	/* Check whether IOMMU is present. If present, translate IOVA
	 * to physical address, else the dma handle is the physical
	 * address.
	 */
	dom = iommu_get_domain_for_dev(psp->dev);
	if (dom)
		tee_buf->paddr = iommu_iova_to_phys(dom, tee_buf->dma);
	else
		tee_buf->paddr = tee_buf->dma;

	return tee_buf;
}
EXPORT_SYMBOL(psp_tee_alloc_buffer);

void psp_tee_free_buffer(struct psp_tee_buffer *tee_buf)
{
	struct psp_device *psp = psp_get_master_device();

	if (!psp || !tee_buf)
		return;

	dma_free_coherent(psp->dev, tee_buf->size,
			  tee_buf->vaddr, tee_buf->dma);

	kfree(tee_buf);
}
EXPORT_SYMBOL(psp_tee_free_buffer);

static int tee_alloc_ring(struct psp_tee_device *tee, int ring_size)
{
	struct ring_buf_manager *rb_mgr = &tee->rb_mgr;

	if (!ring_size)
		return -EINVAL;

	rb_mgr->ring_buf = psp_tee_alloc_buffer(ring_size,
						GFP_KERNEL | __GFP_ZERO);
	if (!rb_mgr->ring_buf) {
		dev_err(tee->dev, "ring allocation failed\n");
		return -ENOMEM;
	}
	mutex_init(&rb_mgr->mutex);

	return 0;
}

static void tee_free_ring(struct psp_tee_device *tee)
{
	struct ring_buf_manager *rb_mgr = &tee->rb_mgr;

	psp_tee_free_buffer(rb_mgr->ring_buf);

	mutex_destroy(&rb_mgr->mutex);
}

static int tee_wait_cmd_poll(struct psp_tee_device *tee, unsigned int timeout,
			     unsigned int *reg)
{
	/* ~10ms sleep per loop => nloop = timeout * 100 */
	int nloop = timeout * 100;

	while (--nloop) {
		*reg = ioread32(tee->io_regs + tee->vdata->cmdresp_reg);
		if (FIELD_GET(PSP_CMDRESP_RESP, *reg))
			return 0;

		usleep_range(10000, 10100);
	}

	dev_err(tee->dev, "tee: command timed out, disabling PSP\n");
	psp_dead = true;

	return -ETIMEDOUT;
}

struct psp_tee_buffer *tee_alloc_cmd_buffer(struct psp_tee_device *tee)
{
	struct tee_init_ring_cmd *cmd;
	struct psp_tee_buffer *cmd_buffer;

	cmd_buffer = psp_tee_alloc_buffer(sizeof(*cmd),
					  GFP_KERNEL | __GFP_ZERO);
	if (!cmd_buffer)
		return NULL;

	cmd = (struct tee_init_ring_cmd *)cmd_buffer->vaddr;
	cmd->hi_addr = upper_32_bits(tee->rb_mgr.ring_buf->paddr);
	cmd->low_addr = lower_32_bits(tee->rb_mgr.ring_buf->paddr);
	cmd->size = tee->rb_mgr.ring_buf->size;

	dev_dbg(tee->dev, "tee: ring address: high = 0x%x low = 0x%x size = %u\n",
		cmd->hi_addr, cmd->low_addr, cmd->size);

	return cmd_buffer;
}

static inline void tee_free_cmd_buffer(struct psp_tee_buffer *cmd_buffer)
{
	psp_tee_free_buffer(cmd_buffer);
}

static int tee_init_ring(struct psp_tee_device *tee)
{
	int ring_size = MAX_RING_BUFFER_ENTRIES * sizeof(struct tee_ring_cmd);
	struct psp_tee_buffer *cmd_buffer;
	unsigned int reg;
	int ret;

	BUILD_BUG_ON(sizeof(struct tee_ring_cmd) != 1024);

	ret = tee_alloc_ring(tee, ring_size);
	if (ret) {
		dev_err(tee->dev, "tee: ring allocation failed %d\n", ret);
		return ret;
	}

	tee->rb_mgr.wptr = 0;

	cmd_buffer = tee_alloc_cmd_buffer(tee);
	if (!cmd_buffer) {
		tee_free_ring(tee);
		return -ENOMEM;
	}

	/* Send command buffer details to Trusted OS by writing to
	 * CPU-PSP message registers
	 */

	iowrite32(lower_32_bits(cmd_buffer->paddr),
		  tee->io_regs + tee->vdata->cmdbuff_addr_lo_reg);
	iowrite32(upper_32_bits(cmd_buffer->paddr),
		  tee->io_regs + tee->vdata->cmdbuff_addr_hi_reg);
	iowrite32(TEE_RING_INIT_CMD,
		  tee->io_regs + tee->vdata->cmdresp_reg);

	ret = tee_wait_cmd_poll(tee, TEE_DEFAULT_TIMEOUT, &reg);
	if (ret) {
		dev_err(tee->dev, "tee: ring init command timed out\n");
		tee_free_ring(tee);
		goto free_buf;
	}

	if (FIELD_GET(PSP_CMDRESP_STS, reg)) {
		dev_err(tee->dev, "tee: ring init command failed (%#010lx)\n",
			FIELD_GET(PSP_CMDRESP_STS, reg));
		tee_free_ring(tee);
		ret = -EIO;
	}

free_buf:
	tee_free_cmd_buffer(cmd_buffer);

	return ret;
}

static void tee_destroy_ring(struct psp_tee_device *tee)
{
	unsigned int reg;
	int ret;

	if (!tee->rb_mgr.ring_buf->vaddr)
		return;

	if (psp_dead)
		goto free_ring;

	iowrite32(TEE_RING_DESTROY_CMD,
		  tee->io_regs + tee->vdata->cmdresp_reg);

	ret = tee_wait_cmd_poll(tee, TEE_DEFAULT_TIMEOUT, &reg);
	if (ret) {
		dev_err(tee->dev, "tee: ring destroy command timed out\n");
	} else if (FIELD_GET(PSP_CMDRESP_STS, reg)) {
		dev_err(tee->dev, "tee: ring destroy command failed (%#010lx)\n",
			FIELD_GET(PSP_CMDRESP_STS, reg));
	}

free_ring:
	tee_free_ring(tee);
}

int tee_dev_init(struct psp_device *psp)
{
	struct device *dev = psp->dev;
	struct psp_tee_device *tee;
	int ret;

	ret = -ENOMEM;
	tee = devm_kzalloc(dev, sizeof(*tee), GFP_KERNEL);
	if (!tee)
		goto e_err;

	psp->tee_data = tee;

	tee->dev = dev;
	tee->psp = psp;

	tee->io_regs = psp->io_regs;

	tee->vdata = (struct tee_vdata *)psp->vdata->tee;
	if (!tee->vdata) {
		ret = -ENODEV;
		dev_err(dev, "tee: missing driver data\n");
		goto e_err;
	}

	ret = tee_init_ring(tee);
	if (ret) {
		dev_err(dev, "tee: failed to init ring buffer\n");
		goto e_err;
	}

	dev_notice(dev, "tee enabled\n");

	return 0;

e_err:
	psp->tee_data = NULL;

	dev_notice(dev, "tee initialization failed\n");

	return ret;
}

void tee_dev_destroy(struct psp_device *psp)
{
	struct psp_tee_device *tee = psp->tee_data;

	if (!tee)
		return;

	tee_destroy_ring(tee);
}

static int tee_submit_cmd(struct psp_tee_device *tee, enum tee_cmd_id cmd_id,
			  void *buf, size_t len, struct tee_ring_cmd **resp)
{
	struct tee_ring_cmd *cmd;
	int nloop = 1000, ret = 0;
	u32 rptr;

	*resp = NULL;

	mutex_lock(&tee->rb_mgr.mutex);

	/* Loop until empty entry found in ring buffer */
	do {
		/* Get pointer to ring buffer command entry */
		cmd = (struct tee_ring_cmd *)
			(tee->rb_mgr.ring_buf->vaddr + tee->rb_mgr.wptr);

		rptr = ioread32(tee->io_regs + tee->vdata->ring_rptr_reg);

		/* Check if ring buffer is full or command entry is waiting
		 * for response from TEE
		 */
		if (!(tee->rb_mgr.wptr + sizeof(struct tee_ring_cmd) == rptr ||
		      cmd->flag == CMD_WAITING_FOR_RESPONSE))
			break;

		dev_dbg(tee->dev, "tee: ring buffer full. rptr = %u wptr = %u\n",
			rptr, tee->rb_mgr.wptr);

		/* Wait if ring buffer is full or TEE is processing data */
		mutex_unlock(&tee->rb_mgr.mutex);
		schedule_timeout_interruptible(msecs_to_jiffies(10));
		mutex_lock(&tee->rb_mgr.mutex);

	} while (--nloop);

	if (!nloop &&
	    (tee->rb_mgr.wptr + sizeof(struct tee_ring_cmd) == rptr ||
	     cmd->flag == CMD_WAITING_FOR_RESPONSE)) {
		dev_err(tee->dev, "tee: ring buffer full. rptr = %u wptr = %u response flag %u\n",
			rptr, tee->rb_mgr.wptr, cmd->flag);
		ret = -EBUSY;
		goto unlock;
	}

	/* Do not submit command if PSP got disabled while processing any
	 * command in another thread
	 */
	if (psp_dead) {
		ret = -EBUSY;
		goto unlock;
	}

	/* Write command data into ring buffer */
	cmd->cmd_id = cmd_id;
	cmd->cmd_state = TEE_CMD_STATE_INIT;
	memset(&cmd->buf[0], 0, sizeof(cmd->buf));
	memcpy(&cmd->buf[0], buf, len);

	/* Indicate driver is waiting for response */
	cmd->flag = CMD_WAITING_FOR_RESPONSE;

	/* Update local copy of write pointer */
	tee->rb_mgr.wptr += sizeof(struct tee_ring_cmd);
	if (tee->rb_mgr.wptr >= tee->rb_mgr.ring_buf->size)
		tee->rb_mgr.wptr = 0;

	/* Trigger interrupt to Trusted OS */
	iowrite32(tee->rb_mgr.wptr, tee->io_regs + tee->vdata->ring_wptr_reg);

	/* The response is provided by Trusted OS in same
	 * location as submitted data entry within ring buffer.
	 */
	*resp = cmd;

unlock:
	mutex_unlock(&tee->rb_mgr.mutex);

	return ret;
}

static int tee_wait_cmd_completion(struct psp_tee_device *tee,
				   struct tee_ring_cmd *resp,
				   unsigned int timeout)
{
	/* ~1ms sleep per loop => nloop = timeout * 1000 */
	int nloop = timeout * 1000;

	while (--nloop) {
		if (resp->cmd_state == TEE_CMD_STATE_COMPLETED)
			return 0;

		usleep_range(1000, 1100);
	}

	dev_err(tee->dev, "tee: command 0x%x timed out, disabling PSP\n",
		resp->cmd_id);

	psp_dead = true;

	return -ETIMEDOUT;
}

int psp_tee_process_cmd(enum tee_cmd_id cmd_id, void *buf, size_t len,
			u32 *status)
{
	struct psp_device *psp = psp_get_master_device();
	struct psp_tee_device *tee;
	struct tee_ring_cmd *resp;
	int ret;

	if (!buf || !status || !len || len > sizeof(resp->buf))
		return -EINVAL;

	*status = 0;

	if (!psp || !psp->tee_data)
		return -ENODEV;

	if (psp_dead)
		return -EBUSY;

	tee = psp->tee_data;

	ret = tee_submit_cmd(tee, cmd_id, buf, len, &resp);
	if (ret)
		return ret;

	ret = tee_wait_cmd_completion(tee, resp, TEE_DEFAULT_TIMEOUT);
	if (ret) {
		resp->flag = CMD_RESPONSE_TIMEDOUT;
		return ret;
	}

	memcpy(buf, &resp->buf[0], len);
	*status = resp->status;

	resp->flag = CMD_RESPONSE_COPIED;

	return 0;
}
EXPORT_SYMBOL(psp_tee_process_cmd);

int psp_check_tee_status(void)
{
	struct psp_device *psp = psp_get_master_device();

	if (!psp || !psp->tee_data)
		return -ENODEV;

	return 0;
}
EXPORT_SYMBOL(psp_check_tee_status);
