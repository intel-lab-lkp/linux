// SPDX-License-Identifier: GPL-2.0
/*
 * Core MCU functionality including firmware loading,
 * memory allocation, and general MCU interaction interfaces
 *
 * Copyright (c) 2025 Allegro DVT.
 * Author: Yassine OUAISSA <yassine.ouaissa@allegrodvt.fr>
 */
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/interrupt.h>
#include <linux/of_reserved_mem.h>
#include <linux/slab.h>
#include <linux/types.h>

#include "al_codec_common.h"

#define AL_CODEC_UID 0x0000
#define AL_CODEC_UID_ID 0x30AB6E51
#define AL_CODEC_RESET 0x0010
#define AL_CODEC_RESET_CMD BIT(0)
#define AL_CODEC_COMM_MASK BIT(0)
#define AL_CODEC_IRQ_MASK 0x0014
#define AL_CODEC_IRQ_STATUS_CLEAR 0x0018
#define AL_CODEC_IRQ_MCU_2_CPU BIT(30)
#define AL_CODEC_MCU_CLK 0x0400
#define AL_CODEC_MCU_CLK_ENABLE BIT(0)
#define AL_CODEC_MCU_CLK_DISABLE 0
#define AL_CODEC_MCU_RST 0x0404
#define AL_CODEC_MCU_RST_ENABLE BIT(0)
#define AL_CODEC_MCU_RST_DISABLE 0
#define AL_CODEC_MCU_IRQ 0x040C
#define AL_CODEC_MCU_BOOT_ADDR 0x0410
#define AL_CODEC_MCU_APB_ADDR 0x0418
#define AL_CODEC_MCU_PERIPHERAL_ADDR 0x0428
#define AL_CODEC_MCU_IP_INTERRUPT_MASK 0x0440
#define AL_CODEC_INSTRUCTION_DATA_OFFSET 0x0450
#define AL_CODEC_IP_OFFSET GENMASK(26, 25)
#define AL_CODEC_APB_MASK GENMASK(26, 0)
#define AL_CODEC_MAX_ADDR GENMASK_ULL(38, 0)

#define AL_CODEC_MCU_BOOT_RESET_WAIT 2 /* in ms */

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

/* Regmap config */
static const struct regmap_config al_regmap_config = {
	.name = "regs",
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = 0xfff,
	.fast_io = true,
	.cache_type = REGCACHE_NONE,
	.can_multi_write = true,
};

static inline int al_common_read(struct al_common_dev *dev, u32 offset,
				 u32 *val)
{
	return regmap_read(dev->regmap, offset, val);
}

static inline int al_common_write(struct al_common_dev *dev, u32 offset,
				  u32 val)
{
	return regmap_write(dev->regmap, offset, val);
}

static inline int al_common_write_multiple(struct al_common_dev *dev,
					   u32 offset, const void *val,
					   u32 num_regs)
{
	return regmap_bulk_write(dev->regmap, offset, val, num_regs);
}

static inline int common_update_bits(struct al_common_dev *dev, u32 offset,
				     u32 mask, u32 val)
{
	return regmap_update_bits(dev->regmap, offset, mask, val);
}

static void common_trigger_mcu_irq(void *arg)
{
	struct al_common_dev *dev = arg;

	common_update_bits(dev, AL_CODEC_MCU_IRQ, AL_CODEC_COMM_MASK, BIT(0));
}

static void common_reset(struct al_common_dev *dev)
{
	/* reset ip */
	common_update_bits(dev, AL_CODEC_RESET, AL_CODEC_COMM_MASK,
			   AL_CODEC_RESET_CMD);

	/* reset and stop mcu */
	al_common_write(dev, AL_CODEC_MCU_CLK, AL_CODEC_MCU_CLK_ENABLE);
	al_common_write(dev, AL_CODEC_MCU_RST, AL_CODEC_MCU_RST_ENABLE);
	/* time to reset the mct */
	mdelay(AL_CODEC_MCU_BOOT_RESET_WAIT);
	al_common_write(dev, AL_CODEC_MCU_CLK, AL_CODEC_MCU_CLK_DISABLE);

	common_update_bits(dev, AL_CODEC_MCU_IRQ, AL_CODEC_COMM_MASK, 0);
	al_common_write(dev, AL_CODEC_MCU_IP_INTERRUPT_MASK, 0);

	mdelay(AL_CODEC_MCU_BOOT_RESET_WAIT * 5);
	common_update_bits(dev, AL_CODEC_MCU_RST, AL_CODEC_COMM_MASK,
			   AL_CODEC_MCU_RST_DISABLE);
}

static int common_probe_check_and_setup_hw(struct al_common_dev *dev)
{
	unsigned int id;
	int ret;

	/* Check regmap */
	if (WARN_ON(!dev->regmap))
		return -EINVAL;

	ret = al_common_read(dev, AL_CODEC_UID, &id);

	if (ret)
		return ret;

	if (id != AL_CODEC_UID_ID) {
		al_codec_err(dev, "bad device id, expected 0x%08x, got 0x%08x",
			     AL_CODEC_UID_ID, id);
		return -ENODEV;
	}

	common_reset(dev);
	al_common_write(dev, AL_CODEC_IRQ_MASK, AL_CODEC_IRQ_MCU_2_CPU);

	return 0;
}

static void common_dma_buf_insert(struct al_common_dev *dev,
				  struct codec_dma_buf *buf)
{
	mutex_lock(&dev->buf_lock);
	list_add(&buf->list, &dev->alloc_buffers);
	mutex_unlock(&dev->buf_lock);
}

static void common_dma_buf_remove(struct al_common_dev *dev,
				  struct codec_dma_buf *buf)
{
	mutex_lock(&dev->buf_lock);
	list_del(&buf->list);
	mutex_unlock(&dev->buf_lock);
}

static struct codec_dma_buf *common_dma_buf_lookup(struct al_common_dev *dev,
						   dma_addr_t buf_paddr)
{
	struct codec_dma_buf *buf = NULL;

	mutex_lock(&dev->buf_lock);
	list_for_each_entry(buf, &dev->alloc_buffers, list)
		if (likely(buf->paddr == buf_paddr))
			break;

	mutex_unlock(&dev->buf_lock);

	return list_entry_is_head(buf, &dev->alloc_buffers, list) ? NULL : buf;
}

static void common_dma_buf_cleanup(struct al_common_dev *dev)
{
	struct codec_dma_buf *buf, *tmp;

	mutex_lock(&dev->buf_lock);
	list_for_each_entry_safe(buf, tmp, &dev->alloc_buffers, list) {
		dma_free_coherent(&dev->pdev->dev, buf->size, buf->vaddr,
				  buf->paddr);
		list_del(&buf->list);
		kfree(buf);
	}
	mutex_unlock(&dev->buf_lock);
}

static void *common_dma_alloc(struct al_common_dev *dev, size_t size,
			      dma_addr_t *paddr, gfp_t flag)
{
	void *vaddr;

	vaddr = dma_alloc_coherent(&dev->pdev->dev, size, paddr, flag);

	if (!vaddr)
		return NULL;

	/* PADDR <= (2^39 - 1) (39-bit MCU PADDR) */
	if ((*paddr + size) > AL_CODEC_MAX_ADDR) {
		al_codec_err(dev, "mem check failed for 0x%16llx of size %zu",
			     *paddr, size);
		dma_free_coherent(&dev->pdev->dev, size, vaddr, *paddr);
		return NULL;
	}

	return vaddr;
}

void al_common_remove(struct al_common_dev *dev)
{
	common_dma_buf_cleanup(dev);
	if (dev->fw_cpu_mem)
		dma_free_coherent(&dev->pdev->dev, dev->fw_size,
				  dev->fw_cpu_mem, dev->fw_phys_addr);

	/* reset device */
	common_reset(dev);
	clk_disable_unprepare(dev->mcu_clk);
}

static void handle_alloc_memory_req(struct al_common_dev *dev,
				    struct msg_itf_header *hdr)
{
	struct msg_itf_alloc_mem_reply_full reply;
	struct msg_itf_alloc_mem_req req;
	struct codec_dma_buf *buf;
	int ret;

	reply.reply.phyAddr = 0;
	ret = al_common_get_data(dev, (char *)&req, hdr->payload_len);
	if (ret) {
		al_codec_err(dev, "Unable to get cma req");
		return;
	}

	buf = kmalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf)
		goto send_reply;

	buf->size = req.uSize;
	buf->vaddr = common_dma_alloc(dev, req.uSize, &buf->paddr, GFP_KERNEL);
	if (!buf->vaddr)
		goto send_reply;

	reply.reply.phyAddr = (u64)buf->paddr;
	common_dma_buf_insert(dev, buf);

send_reply:
	reply.hdr.type = MSG_ITF_TYPE_ALLOC_MEM_REPLY;
	/* both fields embed info need to finish request */
	reply.hdr.drv_ctx_hdl = hdr->drv_ctx_hdl;
	reply.hdr.drv_cmd_hdl = hdr->drv_cmd_hdl;
	reply.hdr.payload_len = sizeof(reply.reply);

	ret = al_common_send(dev, &reply.hdr);
	if (ret) {
		al_codec_err(dev, "Unable to reply to cma alloc");
		common_dma_buf_remove(dev, buf);
	}
}

static void handle_free_memory_req(struct al_common_dev *dev,
				   struct msg_itf_header *hdr)
{
	struct msg_itf_free_mem_reply_full reply;
	struct msg_itf_free_mem_req req;
	struct codec_dma_buf *buf;
	int ret;

	reply.reply.ret = -1;
	ret = al_common_get_data(dev, (char *)&req, hdr->payload_len);
	if (ret) {
		al_codec_err(dev, "Unable to put cma req");
		return;
	}

	buf = common_dma_buf_lookup(dev, req.phyAddr);
	al_codec_dbg(3, "req.phyAddr = %p => %p, Size %d",
		     (void *)(long)req.phyAddr, buf, buf->size);
	if (!buf) {
		al_codec_err(dev, "Unable to get dma handle for %p",
			     (void *)(long)req.phyAddr);
		reply.reply.ret = -EINVAL;
		goto send_reply;
	}

	dma_free_coherent(&dev->pdev->dev, buf->size, buf->vaddr, buf->paddr);
	common_dma_buf_remove(dev, buf);
	reply.reply.ret = 0;

send_reply:
	reply.hdr.type = MSG_ITF_TYPE_FREE_MEM_REPLY;
	/* both fields embed info need to hinish request */
	reply.hdr.drv_ctx_hdl = hdr->drv_ctx_hdl;
	reply.hdr.drv_cmd_hdl = hdr->drv_cmd_hdl;
	reply.hdr.payload_len = sizeof(reply.reply);

	ret = al_common_send(dev, &reply.hdr);
	if (ret)
		al_codec_err(dev, "Unable to reply to cma free");
}

static void handle_mcu_console_print(struct al_common_dev *dev,
				     struct msg_itf_header *hdr)
{
#if defined(DEBUG)
	struct msg_itf_write_req *req;
	int ret;

	/* one more byte to be sure to have a zero terminated string */
	req = kzalloc(hdr->payload_len + 1, GFP_KERNEL);
	if (!req) {
		al_common_skip_data(dev, hdr->payload_len);
		al_codec_err(dev, "Unable to alloc memory");
		return;
	}

	ret = al_codec_msg_get_data(&dev->mb_m2h, (char *)req,
				    hdr->payload_len);
	if (ret) {
		al_codec_err(dev, "Unable to get request");
		kfree(req);
		return;
	}

	al_mcu_dbg("%s", (char *)(req + 1));
	kfree(req);
#else
	al_common_skip_data(dev, hdr->payload_len);
#endif
}

static void process_one_message(struct al_common_dev *dev,
				struct msg_itf_header *hdr)
{
	if (hdr->type == MSG_ITF_TYPE_ALLOC_MEM_REQ)
		handle_alloc_memory_req(dev, hdr);
	else if (hdr->type == MSG_ITF_TYPE_FREE_MEM_REQ)
		handle_free_memory_req(dev, hdr);
	else if (hdr->type == MSG_ITF_TYPE_WRITE_REQ)
		handle_mcu_console_print(dev, hdr);
	else if (hdr->type == MSG_ITF_TYPE_MCU_ALIVE)
		complete(&dev->completion);
	else
		dev->process_msg_cb(dev->cb_arg, hdr);
}

static void common_reply_handle(struct al_common_dev *dev)
{
	struct msg_itf_header hdr;
	int ret;

	while (1) {
		ret = al_codec_msg_get_header(&dev->mb_m2h, &hdr);
		if (ret)
			break;

		process_one_message(dev, &hdr);
	}
}

static irqreturn_t common_irq_handler(int irq, void *data)
{
	struct al_common_dev *dev = data;

	/* poll all messages */
	common_reply_handle(dev);

	return IRQ_HANDLED;
}

static irqreturn_t common_hardirq_handler(int irq, void *data)
{
	struct al_common_dev *dev = data;
	u32 irq_status;
	int ret;

	ret = al_common_read(dev, AL_CODEC_IRQ_STATUS_CLEAR, &irq_status);
	if (ret || !irq_status)
		return IRQ_NONE;

	al_common_write(dev, AL_CODEC_IRQ_STATUS_CLEAR, AL_CODEC_IRQ_MCU_2_CPU);

	return IRQ_WAKE_THREAD;
}

static inline u64 get_machine_boot_addr(struct al_common_dev *dev,
					struct boot_header *bh)
{
	return dev->fw_phys_addr + bh->vaddr_boot - bh->vaddr_start;
}

static int common_start_fw(struct al_common_dev *dev, struct boot_header *bh)
{
	u64 boot_addr;
	u32 regbuf[2] = { 0 };
	int ret;

	boot_addr = get_machine_boot_addr(dev, bh);
	regbuf[0] = upper_32_bits(boot_addr);
	regbuf[1] = lower_32_bits(boot_addr);

	ret = al_common_write_multiple(dev, AL_CODEC_MCU_BOOT_ADDR, regbuf,
				       ARRAY_SIZE(regbuf));
	if (ret) {
		al_codec_err(dev, "Unable to set the MCU boot address");
		return ret;
	}

	al_codec_dbg(3, "boot_addr = %pad\n", &boot_addr);

	/* Enable the MCU clock */
	ret = common_update_bits(dev, AL_CODEC_MCU_CLK, AL_CODEC_COMM_MASK,
				 AL_CODEC_MCU_CLK_ENABLE);

	if (ret) {
		al_codec_err(dev, " failed to enable the MCU clock");
		return ret;
	}

	return !wait_for_completion_timeout(&dev->completion, HZ);
}

static inline u64 common_get_periph_addr(struct al_common_dev *dev)
{
	struct resource *res;

	res = platform_get_resource_byname(dev->pdev, IORESOURCE_MEM, "apb");
	if (!res) {
		al_codec_err(dev, "Unable to find APB start address");
		return 0;
	}

	if (res->start & AL_CODEC_APB_MASK) {
		al_codec_err(dev, "APB start address is invalid");
		return 0;
	}

	return res->start;
}

static int common_alloc_and_setup_fw_memory(struct al_common_dev *dev,
					    struct boot_header *bh)
{
	u64 periph_addr;
	u32 regbuf[4] = { 0 };
	int ret;

	dev->fw_cpu_mem = common_dma_alloc(dev, dev->fw_size,
					   &dev->fw_phys_addr, GFP_KERNEL);
	if (!dev->fw_cpu_mem)
		return -ENOMEM;

	ret = al_common_write_multiple(dev, AL_CODEC_INSTRUCTION_DATA_OFFSET,
				       regbuf, ARRAY_SIZE(regbuf));
	if (ret) {
		al_codec_err(dev, "failed to set the (i/d)Cache address");
		return ret;
	}

	periph_addr = common_get_periph_addr(dev);

	regbuf[0] = upper_32_bits(periph_addr);
	regbuf[1] = lower_32_bits(periph_addr);
	ret = al_common_write_multiple(dev, AL_CODEC_MCU_PERIPHERAL_ADDR,
				       regbuf, 2);
	if (ret) {
		al_codec_err(dev, "failed to set the periph address");
		return ret;
	}

	al_codec_dbg(3, "fw phys_addr = %pad", &dev->fw_phys_addr);
	al_codec_dbg(3, "fw virt_addr = 0x%p", dev->fw_cpu_mem);
	al_codec_dbg(3, "periph_addr  = %pad", &periph_addr);

	return 0;
}

static void common_fw_callback(const struct firmware *fw, void *context)
{
	struct al_common_dev *dev = context;
	struct boot_header bh, *bhw;
	u64 periph_addr = 0;
	int ret;

	if (!fw) {
		al_codec_err(dev, "The MCU firmware not found!");
		return;
	}

	/* Copy the Firmware header */
	memcpy(&bh, fw->data, sizeof(bh));
	dev->fw_size = bh.vaddr_end - bh.vaddr_start;

	if (bh.bh_version < AL_BOOT_VERSION(2, 0, 0) ||
	    bh.bh_version >= AL_BOOT_VERSION(3, 0, 0)) {
		al_codec_err(dev, "bad boot header version");
		goto fw_release;
	}

	if (WARN(bh.machine_id != 2, "Wrong machine ID"))
		goto fw_release;

	periph_addr = common_get_periph_addr(dev);

	if (!periph_addr) {
		al_codec_err(dev, "Unable to get the periph addr");
		goto fw_release;
	}

	al_codec_dbg(3, "bh version     = 0x%08x", bh.bh_version);
	al_codec_dbg(3, "fw version     = 0x%08x", bh.fw_version);
	al_codec_dbg(3, "fw model       = %s", bh.model);
	al_codec_dbg(3, "vaddress start = 0x%016llx", bh.vaddr_start);
	al_codec_dbg(3, "vaddress end   = 0x%016llx", bh.vaddr_end);
	al_codec_dbg(3, "boot address   = 0x%016llx", bh.vaddr_boot);
	al_codec_dbg(3, "machineid      = %lld", bh.machine_id);
	al_codec_dbg(3, "periph address = 0x%016llx", periph_addr);

	ret = common_alloc_and_setup_fw_memory(dev, &bh);
	if (ret) {
		al_codec_err(dev, "out of memory %d", ret);
		goto fw_release;
	}

	al_codec_dbg(2, "Copy %zu bytes of fw", fw->size);
	memcpy(dev->fw_cpu_mem, fw->data, fw->size);

	al_codec_mb_init(&dev->mb_h2m,
			 dev->fw_cpu_mem + bh.h2m.start - bh.vaddr_start,
			 bh.h2m.end - bh.h2m.start, MB_IFT_MAGIC_H2M);
	al_codec_mb_init(&dev->mb_m2h,
			 dev->fw_cpu_mem + bh.m2h.start - bh.vaddr_start,
			 bh.m2h.end - bh.m2h.start, MB_IFT_MAGIC_M2H);

	/* give fw information about registers location */
	bhw = dev->fw_cpu_mem;
	bhw->ip_start = periph_addr + AL_CODEC_IP_OFFSET;
	bhw->ip_end = bhw->ip_start + resource_size(&dev->regs_info);
	bhw->mcu_clk_rate = clk_get_rate(dev->mcu_clk);

	al_codec_dbg(3, "ip start     = 0x%016llx", bhw->ip_start);
	al_codec_dbg(3, "ip end       =   0x%016llx", bhw->ip_end);
	al_codec_dbg(3, "mcu clock rate is %llu", bhw->mcu_clk_rate);

	ret = common_start_fw(dev, &bh);
	if (ret) {
		al_codec_err(dev, "fw start has failed");
		goto fw_release;
	}

	dev_info(&dev->pdev->dev, "mcu has boot successfully !\n");
	dev->fw_ready_cb(dev->cb_arg);

fw_release:
	release_firmware(fw);
}

static int common_firmware_request_nowait(struct al_common_dev *dev)
{
	al_codec_dbg(2, "request fw %s", dev->fw_name);

	return request_firmware_nowait(THIS_MODULE, true, dev->fw_name,
				       &dev->pdev->dev, GFP_KERNEL, dev,
				       common_fw_callback);
}

static int common_setup_dma(struct al_common_dev *dev)
{
	int ret;

	/* setup dma memory mask */
	ret = dma_set_mask_and_coherent(&dev->pdev->dev, DMA_BIT_MASK(64));
	if (ret) {
		al_codec_err(dev, "failed to set dma");
		return -EINVAL;
	}

	/* Try to use reserved memory if we got one */
	ret = of_reserved_mem_device_init(&dev->pdev->dev);
	if (ret && ret != ENODEV)
		dev_warn(&dev->pdev->dev,
			 "No reserved memory, use cma instead\n");

	return 0;
}

int al_common_probe(struct platform_device *pdev, struct al_common_dev *dev)
{
	struct resource *res;
	void __iomem *regs;
	int irq;
	int ret;

	dev->pdev = pdev;
	mutex_init(&dev->buf_lock);
	INIT_LIST_HEAD(&dev->alloc_buffers);
	init_completion(&dev->completion);

	/* The MCU has already default clock value */
	dev->mcu_clk = devm_clk_get(&pdev->dev, "mcu_clk");
	if (IS_ERR(dev->mcu_clk)) {
		al_codec_err(dev, "failed to get MCU core clock");
		return PTR_ERR(dev->mcu_clk);
	}

	ret = clk_prepare_enable(dev->mcu_clk);
	if (ret) {
		al_codec_err(dev, "Cannot enable MCU clock: %d\n", ret);
		return ret;
	}

	/* setup dma memory */
	ret = common_setup_dma(dev);
	if (ret)
		goto disable_clk;

	/* Hw registers */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "regs");
	if (!res) {
		al_codec_err(dev, "regs resource missing from device tree");
		ret = -EINVAL;
		goto disable_clk;
	}

	dev->regs_info = *res;

	regs = devm_ioremap_resource(&pdev->dev, res);
	if (!regs) {
		al_codec_err(dev, "failed to map registers");
		ret = -ENOMEM;
		goto disable_clk;
	}

	dev->regmap =
		devm_regmap_init_mmio(&pdev->dev, regs, &al_regmap_config);

	if (IS_ERR(dev->regmap)) {
		al_codec_err(dev, "init regmap failed");
		ret = PTR_ERR(dev->regmap);
		goto disable_clk;
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		al_codec_err(dev, "Failed to get IRQ");
		ret = -EINVAL;
		goto disable_clk;
	}

	ret = devm_request_threaded_irq(&pdev->dev, irq, common_hardirq_handler,
					common_irq_handler, IRQF_SHARED,
					dev_name(&pdev->dev), dev);
	if (ret) {
		al_codec_err(dev, "Unable to register irq handler");
		goto disable_clk;
	}

	ret = common_probe_check_and_setup_hw(dev);
	if (ret) {
		al_codec_err(dev, "Unable to setup hw");
		goto disable_clk;
	}

	/* ok so request the fw */
	ret = common_firmware_request_nowait(dev);
	if (ret) {
		al_codec_err(dev, "failed to request firmware");
		goto disable_clk;
	}

	return 0;

disable_clk:
	clk_disable_unprepare(dev->mcu_clk);

	return ret;
}

int al_common_send(struct al_common_dev *dev, struct msg_itf_header *hdr)
{
	return al_codec_msg_send(&dev->mb_h2m, hdr, common_trigger_mcu_irq,
				 dev);
}

int al_common_send_req_reply(struct al_codec_dev *dev,
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

	ret = al_common_send(&dev->common, hdr);
	if (ret)
		goto remove_cmd;

	al_v4l2_dbg(3, "Send req to mcu %d : %ld ", req->req_type,
		    req->req_size);

	if (!cmd)
		return 0;

	ret = wait_for_completion_timeout(&cmd->done, 5 * HZ);
	if (ret <= 0) {
		al_v4l2_err(dev, "cmd %p has %d (%s)", cmd, ret,
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

bool al_common_mcu_is_alive(struct al_codec_dev *dev)
{
	static const struct msg_itf_header hdr = {
		.type = MSG_ITF_TYPE_MCU_ALIVE,
		.payload_len = 0,
	};
	struct al_common_dev *cdev = &dev->common;
	int ret;

	ret = al_common_send(cdev, (struct msg_itf_header *)&hdr);
	if (ret)
		return false;

	ret = wait_for_completion_timeout(&cdev->completion, 5 * HZ);
	if (ret <= 0)
		return false;

	return true;
}
