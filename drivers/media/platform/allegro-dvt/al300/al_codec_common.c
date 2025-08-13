// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Core MCU functionality including firmware loading,
 * memory allocation, and general MCU interaction interfaces
 *
 * Copyright (c) 2025 Allegro DVT.
 * Author: Yassine OUAISSA <yassine.ouaissa@allegrodvt.fr>
 */

#include <linux/cleanup.h>
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/interrupt.h>
#include <linux/of_reserved_mem.h>
#include <linux/slab.h>
#include <linux/types.h>

#include "al_codec_common.h"

#define AL_CODEC_UID 0x0000
#define AL_CODEC_RESET 0x0010
#define AL_CODEC_IRQ_MASK 0x0014
#define AL_CODEC_IRQ_STATUS_CLEAR 0x0018
#define AL_CODEC_MCU_CLK 0x0400
#define AL_CODEC_MCU_RST 0x0404
#define AL_CODEC_MCU_IRQ 0x040C
#define AL_CODEC_MCU_BOOT_ADDR_HI 0x0410
#define AL_CODEC_MCU_BOOT_ADDR_LO 0x0414
#define AL_CODEC_MCU_IP_START_ADDR_HI 0x0418
#define AL_CODEC_MCU_IP_START_ADDR_LO 0x041C
#define AL_CODEC_MCU_IP_END_ADDR_HI 0x0420
#define AL_CODEC_MCU_IP_END_ADDR_LO 0x0424
#define AL_CODEC_MCU_PERIPH_ADDR_HI 0x0428
#define AL_CODEC_MCU_PERIPH_ADDR_LO 0x042C
#define AL_CODEC_MCU_IRQ_MASK 0x0440
#define AL_CODEC_INST_OFFSET_HI 0x0450
#define AL_CODEC_INST_OFFSET_LO 0x0454
#define AL_CODEC_DATA_OFFSET_HI 0x0458
#define AL_CODEC_DATA_OFFSET_LO 0x045C

#define AL_CODEC_UID_ID 0x30AB6E51
#define AL_CODEC_IRQ_MCU_2_CPU BIT(30)
#define AL_CODEC_IP_OFFSET GENMASK(26, 25)
#define AL_CODEC_APB_MASK GENMASK(26, 0)
#define MCU_FLAT_MEMORY_SIZE (256ULL * 1024 * 1024 * 1024) /* 256GB */

#define AL_CODEC_MCU_BOOT_RESET_WAIT 2 /* in ms */
#define AL_CODEC_REG_ENABLE BIT(0)
#define AL_CODEC_REG_DISABLE 0

/*
 * struct codec_dma_buf - Allocated dma buffer
 *
 * @list: list head for buffer queue
 * @paddr: physical address of the allcated DMA buffer
 * @vaddr: virtual address of the allocated DMA buffer
 * @size: Size of allocated dma memory
 */
struct codec_dma_buf {
	void *vaddr;
	dma_addr_t paddr;
	u32 size;
	struct list_head list;
};

struct mb_header {
	u64 start;
	u64 end;
} __packed;

struct boot_header {
	u32 bh_version;
	u32 fw_version;
	char model[16];
	u64 vaddr_start;
	u64 vaddr_end;
	u64 vaddr_boot;
	struct mb_header h2m;
	struct mb_header m2h;
	u64 machine_id;
	/* fill by driver before fw boot */
	u64 ip_start;
	u64 ip_end;
	u64 mcu_clk_rate;
} __packed;

static u32 al_common_read(struct al_codec_dev *codec, u32 offset)
{
	return readl(codec->regs + offset);
}

static void al_common_write(struct al_codec_dev *codec, u32 offset, u32 val)
{
	writel(val, codec->regs + offset);
}

static void al_common_trigger_mcu_irq(void *arg)
{
	struct al_codec_dev *codec = arg;

	al_common_write(codec, AL_CODEC_MCU_IRQ, BIT(0));
}

static inline void al_common_reset(struct al_codec_dev *codec)
{
	/* reset ip */
	al_common_write(codec, AL_CODEC_RESET, AL_CODEC_REG_ENABLE);

	/* reset and stop mcu */
	al_common_write(codec, AL_CODEC_MCU_CLK, AL_CODEC_REG_ENABLE);
	al_common_write(codec, AL_CODEC_MCU_RST, AL_CODEC_REG_ENABLE);
	/* time to reset the mct */
	mdelay(AL_CODEC_MCU_BOOT_RESET_WAIT);
	al_common_write(codec, AL_CODEC_MCU_CLK, AL_CODEC_REG_DISABLE);

	al_common_write(codec, AL_CODEC_MCU_IRQ, AL_CODEC_REG_DISABLE);
	al_common_write(codec, AL_CODEC_MCU_IRQ_MASK, AL_CODEC_REG_DISABLE);

	mdelay(AL_CODEC_MCU_BOOT_RESET_WAIT * 5);
	al_common_write(codec, AL_CODEC_MCU_RST, AL_CODEC_REG_DISABLE);
}

static int al_common_setup_hw_regs(struct al_codec_dev *codec)
{
	u64 reg_start, reg_end;
	dma_addr_t boot_addr;
	unsigned int id;

	id = al_common_read(codec, AL_CODEC_UID);

	if (id != AL_CODEC_UID_ID) {
		al_codec_err(codec,
			     "bad device id, expected 0x%08x, got 0x%08x",
			     AL_CODEC_UID_ID, id);
		return -ENODEV;
	}

	boot_addr = codec->firmware.phys + codec->firmware.bin_data.offset -
		    codec->offset;

	/* Reset MCU step */
	al_common_reset(codec);

	/* Configure the MCU*/
	al_common_write(codec, AL_CODEC_IRQ_MASK, AL_CODEC_IRQ_MCU_2_CPU);
	/* Set Instruction and data offset */
	al_common_write(codec, AL_CODEC_INST_OFFSET_HI,
			upper_32_bits(codec->offset));
	al_common_write(codec, AL_CODEC_INST_OFFSET_LO,
			lower_32_bits(codec->offset));
	al_common_write(codec, AL_CODEC_DATA_OFFSET_HI,
			upper_32_bits(codec->offset));
	al_common_write(codec, AL_CODEC_DATA_OFFSET_LO,
			lower_32_bits(codec->offset));

	reg_start = codec->regs_info->start;
	reg_end = reg_start + resource_size(codec->regs_info);
	al_common_write(codec, AL_CODEC_MCU_IP_START_ADDR_HI,
			upper_32_bits(reg_start));
	al_common_write(codec, AL_CODEC_MCU_IP_START_ADDR_LO,
			lower_32_bits(reg_start));
	al_common_write(codec, AL_CODEC_MCU_IP_END_ADDR_HI,
			upper_32_bits(reg_end));
	al_common_write(codec, AL_CODEC_MCU_IP_END_ADDR_HI,
			lower_32_bits(reg_end));

	al_common_write(codec, AL_CODEC_MCU_PERIPH_ADDR_HI,
			upper_32_bits(codec->apb));
	al_common_write(codec, AL_CODEC_MCU_PERIPH_ADDR_LO,
			lower_32_bits(codec->apb));

	al_common_write(codec, AL_CODEC_MCU_BOOT_ADDR_HI,
			upper_32_bits(boot_addr));
	al_common_write(codec, AL_CODEC_MCU_BOOT_ADDR_LO,
			lower_32_bits(boot_addr));

	return 0;
}

static void al_common_dma_buf_insert(struct al_codec_dev *codec,
				     struct codec_dma_buf *buf)
{
	guard(mutex)(&codec->buf_lock);
	list_add(&buf->list, &codec->alloc_buffers);
}

static void al_common_dma_buf_remove(struct al_codec_dev *codec,
				     struct codec_dma_buf *buf)
{
	struct device *dev = &codec->pdev->dev;

	guard(mutex)(&codec->buf_lock);
	dma_free_coherent(dev, buf->size, buf->vaddr, buf->paddr);
	list_del(&buf->list);
	kfree(buf);
}

static struct codec_dma_buf *
al_common_dma_buf_lookup(struct al_codec_dev *codec, dma_addr_t buf_paddr)
{
	struct codec_dma_buf *buf = NULL;

	guard(mutex)(&codec->buf_lock);
	list_for_each_entry(buf, &codec->alloc_buffers, list)
		if (likely(buf->paddr == buf_paddr))
			break;

	return list_entry_is_head(buf, &codec->alloc_buffers, list) ? NULL :
								      buf;
}

static void al_common_dma_buf_cleanup(struct al_codec_dev *codec)
{
	struct device *dev = &codec->pdev->dev;
	struct codec_dma_buf *buf, *tmp;

	guard(mutex)(&codec->buf_lock);
	list_for_each_entry_safe(buf, tmp, &codec->alloc_buffers, list) {
		dma_free_coherent(dev, buf->size, buf->vaddr, buf->paddr);
		list_del(&buf->list);
		kfree(buf);
	}
}

static inline uint64_t get_mcu_offset(uint64_t paddr)
{
	if (paddr < MCU_FLAT_MEMORY_SIZE)
		return 0;

	return paddr - MCU_FLAT_MEMORY_SIZE;
}

static int al_common_setup_dma(struct al_codec_dev *codec)
{
	struct device *dev = &codec->pdev->dev;
	struct device_node *node;
	struct resource r;
	int ret;

	/* setup dma memory mask */
	if (!dev->coherent_dma_mask) {
		ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(64));
		if (ret) {
			al_codec_err(codec, "Failed to set dma mask %d\n", ret);
			return ret;
		}
	}

	node = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!node) {
		dev_warn(dev, "No reserved memory, use cma instead\n");
		return 0;
	}

	/* Try to use reserved memory if we got one */
	ret = of_reserved_mem_device_init(dev);
	if (ret) {
		al_codec_err(codec, "Couldn't get reserved memory (errno: %d)",
			     ret);
		return ret;
	}

	ret = of_address_to_resource(node, 0, &r);
	of_node_put(node);
	if (ret) {
		al_codec_err(codec,
			     "Failed to parse the memory-region resources");
		return ret;
	}

	codec->offset = get_mcu_offset(r.start);

	return 0;
}

void al_common_remove(struct al_codec_dev *codec)
{
	/* Cleanup allocated internal buffers used by the mcu*/
	al_common_dma_buf_cleanup(codec);

	/* reset codecice */
	al_common_reset(codec);
	clk_disable_unprepare(codec->clk);
	/* Free the allocated memory for the firmware */
	dma_free_coherent(&codec->pdev->dev, codec->firmware.size,
			  codec->firmware.virt, codec->firmware.phys);

	if (codec->firmware.firmware)
		release_firmware(codec->firmware.firmware);
}

static void handle_alloc_memory_req(struct al_codec_dev *codec,
				    struct msg_itf_header *hdr)
{
	struct device *dev = &codec->pdev->dev;
	struct msg_itf_alloc_mem_req req;
	struct msg_itf_alloc_mem_reply_full reply = {
		.reply.phyAddr = 0,
		.hdr.type = MSG_ITF_TYPE_ALLOC_MEM_REPLY,
		.hdr.drv_ctx_hdl = hdr->drv_ctx_hdl,
		.hdr.drv_cmd_hdl = hdr->drv_cmd_hdl,
		.hdr.payload_len = sizeof(struct msg_itf_alloc_mem_reply),
	};
	struct codec_dma_buf *buf;
	int ret;

	ret = al_common_get_data(codec, (char *)&req, hdr->payload_len);
	if (ret) {
		al_codec_err(codec, "Unable to get cma req %d", ret);
		return;
	}

	buf = kmalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf)
		goto send_reply;

	buf->size = req.uSize;
	buf->vaddr =
		dma_alloc_coherent(dev, buf->size, &buf->paddr, GFP_KERNEL);
	if (!buf->vaddr) {
		dev_warn(dev, "Failed to allocate DMA buffer\n");
		goto send_reply;
	}

	reply.reply.phyAddr = (u64)buf->paddr;
	al_common_dma_buf_insert(codec, buf);

send_reply:
	ret = al_common_send(codec, &reply.hdr);
	if (ret) {
		al_codec_err(codec, "Unable to reply to cma alloc");
		al_common_dma_buf_remove(codec, buf);
	}
}

static void handle_free_memory_req(struct al_codec_dev *codec,
				   struct msg_itf_header *hdr)
{
	struct msg_itf_free_mem_req req;
	struct msg_itf_free_mem_reply_full reply = {
		.hdr.type = MSG_ITF_TYPE_FREE_MEM_REPLY,
		.hdr.drv_ctx_hdl = hdr->drv_ctx_hdl,
		.hdr.drv_cmd_hdl = hdr->drv_cmd_hdl,
		.hdr.payload_len = sizeof(struct msg_itf_free_mem_reply),
		.reply.ret = -1,
	};
	struct codec_dma_buf *buf;
	int ret;

	ret = al_common_get_data(codec, (char *)&req, hdr->payload_len);
	if (ret) {
		al_codec_err(codec, "Unable to put cma req");
		return;
	}

	buf = al_common_dma_buf_lookup(codec, req.phyAddr);
	if (!buf) {
		al_codec_err(codec, "Unable to get dma handle for %p",
			     (void *)(long)req.phyAddr);
		reply.reply.ret = -EINVAL;
		goto send_reply;
	}

	al_codec_dbg(codec, "Free memory %p, size %d",
		     (void *)(long)req.phyAddr, buf->size);

	al_common_dma_buf_remove(codec, buf);
	reply.reply.ret = 0;

send_reply:
	ret = al_common_send(codec, &reply.hdr);
	if (ret)
		al_codec_err(codec, "Unable to reply to cma free");
}

static void handle_mcu_console_print(struct al_codec_dev *codec,
				     struct msg_itf_header *hdr)
{
	struct msg_itf_write_req *req;
	int ret;

	/* one more byte to be sure to have a zero terminated string */
	req = kzalloc(hdr->payload_len + 1, GFP_KERNEL);
	if (!req) {
		al_common_skip_data(codec, hdr->payload_len);
		al_codec_err(codec, "Unable to alloc memory");
		return;
	}

	ret = al_codec_msg_get_data(&codec->mb_m2h, (char *)req,
				    hdr->payload_len);
	if (ret) {
		al_codec_err(codec, "Unable to get request");
		kfree(req);
		return;
	}

	/* Print the mcu logs */
	al_mcu_dbg(codec, "%s", (char *)(req + 1));
	kfree(req);
}

static void process_one_message(struct al_codec_dev *codec,
				struct msg_itf_header *hdr)
{
	switch (hdr->type) {
	case MSG_ITF_TYPE_ALLOC_MEM_REQ:
		handle_alloc_memory_req(codec, hdr);
		break;
	case MSG_ITF_TYPE_FREE_MEM_REQ:
		handle_free_memory_req(codec, hdr);
		break;
	case MSG_ITF_TYPE_WRITE_REQ:
		handle_mcu_console_print(codec, hdr);
		break;
	case MSG_ITF_TYPE_MCU_ALIVE:
		complete(&codec->completion);
		break;
	default:
		codec->process_msg_cb(codec->cb_arg, hdr);
		break;
	}
}

static irqreturn_t al_common_irq_handler(int irq, void *data)
{
	struct al_codec_dev *codec = data;
	struct msg_itf_header hdr;

	/* poll all messages */
	while (al_codec_msg_get_header(&codec->mb_m2h, &hdr) == 0)
		process_one_message(codec, &hdr);

	return IRQ_HANDLED;
}

static irqreturn_t al_common_hardirq_handler(int irq, void *data)
{
	struct al_codec_dev *codec = data;

	if (al_common_read(codec, AL_CODEC_IRQ_STATUS_CLEAR) == 0)
		return IRQ_NONE;

	al_common_write(codec, AL_CODEC_IRQ_STATUS_CLEAR,
			AL_CODEC_IRQ_MCU_2_CPU);

	return IRQ_WAKE_THREAD;
}

static int al_common_start_fw(struct al_codec_dev *codec)
{
	/* Enable the MCU clock */
	al_common_write(codec, AL_CODEC_MCU_CLK, AL_CODEC_REG_ENABLE);

	return !wait_for_completion_timeout(&codec->completion, 2 * HZ);
}

static void al_common_copy_firmware_image(struct al_codec_dev *codec)
{
	const struct firmware *firmware = codec->firmware.firmware;
	u32 *virt = codec->firmware.virt;

	/* copy the whole thing taking into account endianness */
	for (size_t i = 0; i < firmware->size / sizeof(u32); i++)
		virt[i] = le32_to_cpu(((__le32 *)firmware->data)[i]);
}

static int al_common_read_firmware(struct al_codec_dev *codec, const char *name)
{
	struct device *dev = &codec->pdev->dev;
	const struct boot_header *bh;
	int err;

	/* request_firmware prints error if it fails */
	err = request_firmware(&codec->firmware.firmware, name, dev);
	if (err < 0)
		return err;

	bh = (struct boot_header *)codec->firmware.firmware->data;
	codec->firmware.size = bh->vaddr_end - bh->vaddr_start;

	return 0;
}

static int al_common_parse_firmware_image(struct al_codec_dev *codec)
{
	struct boot_header *bh = (void *)codec->firmware.virt;

	if (bh->bh_version < AL_BOOT_VERSION(2, 0, 0) ||
	    bh->bh_version >= AL_BOOT_VERSION(3, 0, 0)) {
		al_codec_err(codec, "Unsupported firmware version");
		return -EINVAL;
	}

	codec->firmware.bin_data.offset = bh->vaddr_boot - bh->vaddr_start;
	codec->firmware.bin_data.size = bh->vaddr_end - bh->vaddr_start;

	codec->firmware.mb_h2m.offset = bh->h2m.start - bh->vaddr_start;
	codec->firmware.mb_h2m.size = bh->h2m.end - bh->h2m.start;
	codec->firmware.mb_m2h.offset = bh->m2h.start - bh->vaddr_start;
	codec->firmware.mb_m2h.size = bh->m2h.end - bh->m2h.start;

	/* Override some data */
	bh->ip_start = codec->apb + AL_CODEC_IP_OFFSET;
	bh->ip_end = bh->ip_start + resource_size(codec->regs_info);
	bh->mcu_clk_rate = clk_get_rate(codec->clk);

	al_codec_dbg(codec, "bh version     = 0x%08x", bh->bh_version);
	al_codec_dbg(codec, "fw version     = 0x%08x", bh->fw_version);
	al_codec_dbg(codec, "fw model       = %s", bh->model);
	al_codec_dbg(codec, "vaddress start = 0x%016llx", bh->vaddr_start);
	al_codec_dbg(codec, "vaddress end   = 0x%016llx", bh->vaddr_end);
	al_codec_dbg(codec, "boot address   = 0x%016llx", bh->vaddr_boot);
	al_codec_dbg(codec, "machineid      = %lld", bh->machine_id);
	al_codec_dbg(codec, "periph address = 0x%llx", (u64)codec->apb);
	al_codec_dbg(codec, "ip start       = 0x%016llx", bh->ip_start);
	al_codec_dbg(codec, "ip end         = 0x%016llx", bh->ip_end);
	al_codec_dbg(codec, "mcu clk	      = %llu", bh->mcu_clk_rate);

	return 0;
}

static int al_common_load_firmware_start(struct al_codec_dev *codec,
					 const char *name)
{
	struct device *dev = &codec->pdev->dev;
	dma_addr_t phys;
	size_t size;
	void *virt;
	int err;

	if (codec->firmware.virt)
		return 0;

	err = al_common_read_firmware(codec, name);
	if (err)
		return err;

	size = codec->firmware.size;

	virt = dma_alloc_coherent(dev, size, &phys, GFP_KERNEL);
	err = dma_mapping_error(dev, phys);
	if (err < 0)
		return err;

	codec->firmware.virt = virt;
	codec->firmware.phys = phys;

	al_common_copy_firmware_image(codec);
	err = al_common_parse_firmware_image(codec);
	if (err) {
		al_codec_err(codec, "failed to parse firmware image");
		goto cleanup;
	}

	err = al_common_setup_hw_regs(codec);
	if (err) {
		al_codec_err(codec, "Unable to setup hw registers");
		goto cleanup;
	}

	al_codec_mb_init(&codec->mb_h2m, virt + codec->firmware.mb_h2m.offset,
			 codec->firmware.mb_h2m.size, MB_IFT_MAGIC_H2M);

	al_codec_mb_init(&codec->mb_m2h, virt + codec->firmware.mb_m2h.offset,
			 codec->firmware.mb_m2h.size, MB_IFT_MAGIC_M2H);

	err = al_common_start_fw(codec);
	if (err) {
		al_codec_err(codec, "fw start has failed");
		goto cleanup;
	}

	al_codec_dbg(codec, "mcu has boot successfully !");
	codec->fw_ready_cb(codec->cb_arg);

	release_firmware(codec->firmware.firmware);
	codec->firmware.firmware = NULL;

	return 0;

cleanup:
	dma_free_coherent(dev, size, virt, phys);

	return err;
}

static u64 al_common_get_periph_addr(struct al_codec_dev *codec)
{
	struct resource *res;

	res = platform_get_resource_byname(codec->pdev, IORESOURCE_MEM, "apb");
	if (!res) {
		al_codec_err(codec, "Unable to find APB start address");
		return 0;
	}

	if (res->start & AL_CODEC_APB_MASK) {
		al_codec_err(codec, "APB start address is invalid");
		return 0;
	}

	return res->start;
}

int al_common_probe(struct al_codec_dev *codec, const char *name)
{
	struct platform_device *pdev = codec->pdev;
	int irq;
	int ret;

	mutex_init(&codec->buf_lock);
	INIT_LIST_HEAD(&codec->alloc_buffers);
	init_completion(&codec->completion);
	codec->offset = 0;

	/* setup dma memory */
	ret = al_common_setup_dma(codec);
	if (ret)
		return ret;

	/* Hw registers */
	codec->regs_info =
		platform_get_resource_byname(pdev, IORESOURCE_MEM, "top");
	if (!codec->regs_info) {
		al_codec_err(codec, "regs resource missing from device tree");
		return -EINVAL;
	}

	codec->regs = devm_ioremap_resource(&pdev->dev, codec->regs_info);
	if (!codec->regs) {
		al_codec_err(codec, "failed to map registers");
		return -ENOMEM;
	}

	codec->apb = al_common_get_periph_addr(codec);
	if (!codec->apb)
		return -EINVAL;

	/* The MCU has already default clock value */
	codec->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(codec->clk)) {
		al_codec_err(codec, "failed to get MCU core clock");
		return PTR_ERR(codec->clk);
	}

	ret = clk_prepare_enable(codec->clk);
	if (ret) {
		al_codec_err(codec, "Cannot enable MCU clock: %d\n", ret);
		return ret;
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		al_codec_err(codec, "Failed to get IRQ");
		ret = -EINVAL;
		goto disable_clk;
	}

	ret = devm_request_threaded_irq(&pdev->dev, irq,
					al_common_hardirq_handler,
					al_common_irq_handler, IRQF_SHARED,
					dev_name(&pdev->dev), codec);
	if (ret) {
		al_codec_err(codec, "Unable to register irq handler");
		goto disable_clk;
	}

	/* ok so request the fw */
	ret = al_common_load_firmware_start(codec, name);
	if (ret) {
		al_codec_err(codec, "failed to load firmware : %s", name);
		goto disable_clk;
	}

	return 0;

disable_clk:
	clk_disable_unprepare(codec->clk);

	return ret;
}

int al_common_send(struct al_codec_dev *codec, struct msg_itf_header *hdr)
{
	return al_codec_msg_send(&codec->mb_h2m, hdr, al_common_trigger_mcu_irq,
				 codec);
}

int al_common_send_req_reply(struct al_codec_dev *codec,
			     struct list_head *cmd_list,
			     struct msg_itf_header *hdr,
			     struct al_common_mcu_req *req)
{
	struct al_codec_cmd *cmd = NULL;
	int ret;

	hdr->drv_cmd_hdl = 0;

	if (req->reply_size && req->reply) {
		cmd = al_codec_cmd_create(req->reply_size);
		if (!cmd)
			return -ENOMEM;

		hdr->drv_cmd_hdl = al_virt_to_phys(cmd);
	}

	hdr->drv_ctx_hdl = req->pCtx;
	hdr->type = req->req_type;
	hdr->payload_len = req->req_size;

	/* Add the list to the cmd list */
	if (cmd)
		list_add(&cmd->list, cmd_list);

	ret = al_common_send(codec, hdr);
	if (ret)
		goto remove_cmd;

	al_codec_dbg(codec, "Send req to mcu %d : %zu", req->req_type,
		     req->req_size);

	if (!cmd)
		return 0;

	ret = wait_for_completion_timeout(&cmd->done, 5 * HZ);
	if (ret <= 0) {
		al_codec_err(codec, "cmd %p has %d (%s)", cmd, ret,
			     (ret == 0) ? "failed" : "timedout");
		ret = -ETIMEDOUT;
		goto remove_cmd;
	}

	ret = 0;
	memcpy(req->reply, cmd->reply, req->reply_size);

remove_cmd:

	if (cmd) {
		list_del(&cmd->list);
		al_codec_cmd_put(cmd);
	}
	return ret;
}

bool al_common_mcu_is_alive(struct al_codec_dev *codec)
{
	static const struct msg_itf_header hdr = {
		.type = MSG_ITF_TYPE_MCU_ALIVE,
		.payload_len = 0,
	};
	int ret;

	ret = al_common_send(codec, (struct msg_itf_header *)&hdr);
	if (ret)
		return false;

	ret = wait_for_completion_timeout(&codec->completion, 5 * HZ);
	if (ret <= 0)
		return false;

	return true;
}
