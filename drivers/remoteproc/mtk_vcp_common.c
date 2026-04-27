// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025 MediaTek Corporation. All rights reserved.
 */

#include <linux/device.h>
#include <linux/delay.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/iommu.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_fdt.h>
#include <linux/of_reserved_mem.h>
#include <linux/slab.h>
#include <uapi/linux/dma-heap.h>

#include "mtk_vcp_common.h"
#include "mtk_vcp_rproc.h"

phys_addr_t vcp_get_reserve_mem_phys(struct mtk_vcp_device *vcp,
				     enum vcp_reserve_mem_id id)
{
	if (id >= 0 && id < NUMS_MEM_ID)
		return vcp->vcp_cluster->vcp_memory_tb[id].phys;

	return 0;
}

dma_addr_t vcp_get_reserve_mem_iova(struct mtk_vcp_device *vcp,
				    enum vcp_reserve_mem_id id)
{
	if (id >= 0 && id < NUMS_MEM_ID)
		return vcp->vcp_cluster->vcp_memory_tb[id].iova;

	return 0;
}

void __iomem *vcp_get_reserve_mem_virt(struct mtk_vcp_device *vcp,
				       enum vcp_reserve_mem_id id)
{
	if (id >= 0 && id < NUMS_MEM_ID)
		return vcp->vcp_cluster->vcp_memory_tb[id].virt;

	return NULL;
}

size_t vcp_get_reserve_mem_size(struct mtk_vcp_device *vcp,
				enum vcp_reserve_mem_id id)
{
	if (id >= 0 && id < NUMS_MEM_ID)
		return vcp->vcp_cluster->vcp_memory_tb[id].size;

	return 0;
}

void __iomem *vcp_get_internal_sram_virt(struct mtk_vcp_device *vcp)
{
	return vcp->vcp_cluster->sram_base;
}

int vcp_reserve_memory_init(struct mtk_vcp_device *vcp)
{
	struct device_node *rmem_node;
	struct resource res;
	struct iommu_domain *domain;
	void __iomem *share_memory_virt;
	void __iomem *rtos_memory_virt;
	phys_addr_t mblock_phys;
	phys_addr_t share_memory_phys;
	dma_addr_t share_memory_iova;
	size_t mblock_size;
	size_t share_memory_size;
	enum vcp_reserve_mem_id id;
	u32 offset;
	int ret;

	rmem_node = of_parse_phandle(vcp->dev->of_node, "memory-region", 0);
	if (!rmem_node)
		return dev_err_probe(vcp->dev, -ENODEV, "No reserved memory-region found.\n");

	ret = of_address_to_resource(rmem_node, 0, &res);
	of_node_put(rmem_node);
	if (ret)
		return dev_err_probe(vcp->dev, ret, "failed to parse reserved memory\n");

	mblock_phys = (phys_addr_t)res.start;
	mblock_size = (size_t)resource_size(&res);

	offset = 0;
	for (id = 0; id < NUMS_MEM_ID; id++) {
		vcp->vcp_cluster->vcp_memory_tb[id].phys = mblock_phys + offset;
		vcp->vcp_cluster->vcp_memory_tb[id].size = vcp->platdata->memory_tb[id].size;
		offset += vcp->vcp_cluster->vcp_memory_tb[id].size;
	}
	if (offset > mblock_size)
		return dev_err_probe(vcp->dev, -EINVAL, "Not enough reserved memory\n");

	share_memory_size = offset - vcp->vcp_cluster->vcp_memory_tb[VCP_RTOS_MEM_ID].size;
	share_memory_phys = vcp->vcp_cluster->vcp_memory_tb[VCP_RTOS_MEM_ID].phys +
			    vcp->vcp_cluster->vcp_memory_tb[VCP_RTOS_MEM_ID].size;

	rtos_memory_virt = devm_ioremap(vcp->dev,
					vcp->vcp_cluster->vcp_memory_tb[VCP_RTOS_MEM_ID].phys,
					vcp->vcp_cluster->vcp_memory_tb[VCP_RTOS_MEM_ID].size);
	if (!rtos_memory_virt)
		return dev_err_probe(vcp->dev, -ENOMEM, "Failed to map RTOS memory\n");

	domain = iommu_get_domain_for_dev(vcp->dev);
	ret = iommu_map(domain, vcp->platdata->rtos_static_iova,
			vcp->vcp_cluster->vcp_memory_tb[VCP_RTOS_MEM_ID].phys,
			vcp->vcp_cluster->vcp_memory_tb[VCP_RTOS_MEM_ID].size,
			IOMMU_READ | IOMMU_WRITE | IOMMU_PRIV, GFP_KERNEL);
	if (ret)
		return dev_err_probe(vcp->dev, ret, "iommu map failed\n");

	vcp->vcp_cluster->vcp_memory_tb[VCP_RTOS_MEM_ID].virt = rtos_memory_virt;
	vcp->vcp_cluster->vcp_memory_tb[VCP_RTOS_MEM_ID].iova = vcp->platdata->rtos_static_iova;

	ret = dma_set_mask_and_coherent(vcp->dev, DMA_BIT_MASK(DMA_MAX_MASK_BIT));
	if (ret) {
		ret = dev_err_probe(vcp->dev, ret, "Failed to set DMA mask\n");
		goto unmap_iommu;
	}

	if (!vcp->dev->dma_parms) {
		vcp->dev->dma_parms = devm_kzalloc(vcp->dev,
						   sizeof(*vcp->dev->dma_parms),
						   GFP_KERNEL);
		if (!vcp->dev->dma_parms) {
			ret = -ENOMEM;
			goto unmap_iommu;
		}
		dma_set_max_seg_size(vcp->dev, (u32)DMA_BIT_MASK(33));
	}

	share_memory_virt = dma_alloc_coherent(vcp->dev,
					       share_memory_size,
					       &share_memory_iova,
					       GFP_KERNEL);
	if (!share_memory_virt) {
		ret = dev_err_probe(vcp->dev, -ENOMEM, "dma_alloc_coherent failed\n");
		goto unmap_iommu;
	}

	offset = 0;
	for (id = VCP_RTOS_MEM_ID + 1; id < NUMS_MEM_ID; id++)  {
		vcp->vcp_cluster->vcp_memory_tb[id].phys = share_memory_phys + offset;
		vcp->vcp_cluster->vcp_memory_tb[id].iova = share_memory_iova + offset;
		vcp->vcp_cluster->vcp_memory_tb[id].virt = share_memory_virt + offset;
		offset += (u32)vcp->vcp_cluster->vcp_memory_tb[id].size;
	}

	vcp->vcp_cluster->share_mem_iova = share_memory_iova;
	vcp->vcp_cluster->share_mem_size = share_memory_size;

	return 0;

unmap_iommu:
	iommu_unmap(domain, vcp->platdata->rtos_static_iova,
		    vcp->vcp_cluster->vcp_memory_tb[VCP_RTOS_MEM_ID].size);
	return ret;
}

int wait_core_hart_shutdown(struct mtk_vcp_device *vcp,
			    enum vcp_core_id core_id)
{
	u32 num_harts;
	u32 status;
	int ret;

	if (core_id >= VCP_CORE_TOTAL) {
		dev_err(vcp->dev, "%s, Invalid core id %d\n", __func__, core_id);
		return -EINVAL;
	}

	num_harts = vcp->vcp_cluster->hart_count[core_id];

	/* Wait for hart0 shutdown */
	if (core_id == VCP_ID) {
		ret = readl_poll_timeout(vcp->vcp_cluster->cfg + VCP_C0_GPR5_H0_REBOOT,
					 status, (status & CORE_RDY_TO_REBOOT),
					 USEC_PER_MSEC,
					 CORE_HART_SHUTDOWN_TIMEOUT_MS * USEC_PER_MSEC);
		if (ret) {
			dev_err(vcp->dev, "VCP hart0 shutdown timeout\n");
			return ret;
		}

		/* Wait for hart1 shutdown if this core has 2 harts */
		if (num_harts > 1) {
			ret = readl_poll_timeout(vcp->vcp_cluster->cfg + VCP_C0_GPR6_H1_REBOOT,
						 status, (status & CORE_RDY_TO_REBOOT),
						 USEC_PER_MSEC,
						 CORE_HART_SHUTDOWN_TIMEOUT_MS * USEC_PER_MSEC);
			if (ret) {
				dev_err(vcp->dev, "VCP hart1 shutdown timeout\n");
				return ret;
			}
		}
	} else if (core_id == MMUP_ID) {
		ret = readl_poll_timeout(vcp->vcp_cluster->cfg + VCP_C1_GPR5_H0_REBOOT,
					 status, (status & CORE_RDY_TO_REBOOT),
					 USEC_PER_MSEC,
					 CORE_HART_SHUTDOWN_TIMEOUT_MS * USEC_PER_MSEC);
		if (ret) {
			dev_err(vcp->dev, "MMUP hart0 shutdown timeout\n");
			return ret;
		}

		/* Wait for hart1 shutdown if this core has 2 harts */
		if (num_harts > 1) {
			ret = readl_poll_timeout(vcp->vcp_cluster->cfg + VCP_C1_GPR6_H1_REBOOT,
						 status, (status & CORE_RDY_TO_REBOOT),
						 USEC_PER_MSEC,
						 CORE_HART_SHUTDOWN_TIMEOUT_MS * USEC_PER_MSEC);
			if (ret) {
				dev_err(vcp->dev, "MMUP hart1 shutdown timeout\n");
				return ret;
			}
		}
	}

	return 0;
}

int reset_vcp(struct mtk_vcp_device *vcp)
{
	struct arm_smccc_res res;

	if (vcp->vcp_cluster->core_nums > MMUP_ID) {
		writel((u32)VCP_PACK_IOVA(vcp->vcp_cluster->share_mem_iova),
		       vcp->vcp_cluster->cfg + VCP_C1_GPR1_DRAM_RESV_ADDR);
		writel((u32)vcp->vcp_cluster->share_mem_size,
		       vcp->vcp_cluster->cfg + VCP_C1_GPR2_DRAM_RESV_SIZE);

		arm_smccc_smc(MTK_SIP_TINYSYS_VCP_CONTROL,
			      MTK_TINYSYS_MMUP_KERNEL_OP_RESET_RELEASE,
			      1, 0, 0, 0, 0, 0, &res);
		if (res.a0 != 1) {
			dev_err(vcp->dev, "MMUP reset release SMC failed: %ld\n", res.a0);
			return -EIO;
		}
	}

	writel((u32)VCP_PACK_IOVA(vcp->vcp_cluster->share_mem_iova),
	       vcp->vcp_cluster->cfg + VCP_C0_GPR1_DRAM_RESV_ADDR);
	writel((u32)vcp->vcp_cluster->share_mem_size,
	       vcp->vcp_cluster->cfg + VCP_C0_GPR2_DRAM_RESV_SIZE);

	arm_smccc_smc(MTK_SIP_TINYSYS_VCP_CONTROL,
		      MTK_TINYSYS_VCP_KERNEL_OP_RESET_RELEASE,
		      1, 0, 0, 0, 0, 0, &res);
	if (res.a0 != 1) {
		dev_err(vcp->dev, "VCP reset release SMC failed: %ld\n", res.a0);
		return -EIO;
	}

	return 0;
}

static size_t load_part_binary(void __iomem *image_buf,
			       const u8 *fw_src,
			       size_t size,
			       const char *part_bin_name)
{
	const u8 *fw_ptr = fw_src;
	u32 offset;
	u32 align_size;
	const struct mtk_vcp_img_hdr *img_hdr_info;

	if (!fw_src || !image_buf || size < VCP_IMAGE_HEADER_SIZE)
		return 0;

	offset = 0;
	while (offset < size) {
		img_hdr_info = (const struct mtk_vcp_img_hdr *)(fw_ptr + offset);
		align_size = round_up(img_hdr_info->dsz, ALIGN_16);
		offset += VCP_IMAGE_HEADER_SIZE;
		if (img_hdr_info->magic != VCM_IMAGE_MAGIC ||
		    strncmp(img_hdr_info->name, part_bin_name, VCM_IMAGE_NAME_MAXSZ - 1)) {
			offset += align_size;
		} else {
			memcpy_toio(image_buf, fw_ptr + offset, img_hdr_info->dsz);
			offset += align_size;
			return img_hdr_info->dsz;
		}
	}

	return 0;
}

static int load_vcp_bin(const u8 *fw_src,
			size_t size,
			void __iomem *img_buf_va,
			phys_addr_t img_buf_pa,
			dma_addr_t img_buf_iova,
			struct mtk_vcp_device *vcp)
{
	u32 fw_size;
	u32 dram_img_size;
	u32 dram_backup_img_offset;
	struct vcp_region_info_st vcp_region_info = {};
	struct arm_smccc_res res;

	fw_size = load_part_binary(vcp->vcp_cluster->sram_base +
				   vcp->vcp_cluster->sram_offset[VCP_ID],
				   fw_src, size, VCP_HFRP_PART_NAME);
	if (!fw_size) {
		dev_err(vcp->dev, "Failed to load %s\n", VCP_HFRP_PART_NAME);
		return -EINVAL;
	}

	dram_img_size = load_part_binary(img_buf_va + VCP_DRAM_IMG_OFFSET,
					 fw_src, size, VCP_HFRP_DRAM_PART_NAME);
	if (!dram_img_size) {
		dev_err(vcp->dev, "Failed to load %s\n", VCP_HFRP_DRAM_PART_NAME);
		return -EINVAL;
	}

	vcp_region_info.struct_size = sizeof(struct vcp_region_info_st);

	dram_backup_img_offset = VCP_DRAM_IMG_OFFSET + round_up(dram_img_size, ALIGN_1024);

	vcp_region_info.ap_dram_start = VCP_PACK_IOVA(img_buf_iova + VCP_DRAM_IMG_OFFSET);
	vcp_region_info.ap_dram_backup_start = VCP_PACK_IOVA(img_buf_iova + dram_backup_img_offset);
	vcp_region_info.ap_dram_size = dram_img_size;

	vcp_region_info.l2tcm_offset = vcp->vcp_cluster->sram_offset[MMUP_ID];

	memcpy_toio(vcp->vcp_cluster->sram_base +
		    vcp->vcp_cluster->sram_offset[VCP_ID] + REGION_OFFSET,
		    &vcp_region_info, sizeof(vcp_region_info));

	arm_smccc_smc(MTK_SIP_TINYSYS_VCP_CONTROL,
		      MTK_TINYSYS_MMUP_KERNEL_OP_SET_L2TCM_OFFSET,
		      vcp->vcp_cluster->sram_offset[MMUP_ID],
		      0, 0, 0, 0, 0, &res);
	if (res.a0 != 1) {
		dev_err(vcp->dev, "Set L2TCM offset SMC failed: %ld\n", res.a0);
		return -EIO;
	}

	return 0;
}

static int load_mmup_bin(const u8 *fw_src,
			 size_t size,
			 void __iomem *img_buf_va,
			 phys_addr_t img_buf_pa,
			 dma_addr_t img_buf_iova,
			 struct mtk_vcp_device *vcp)
{
	u32 fw_size;
	u32 dram_img_size;
	u32 dram_backup_img_offset;
	struct vcp_region_info_st vcp_region_info = {};
	struct arm_smccc_res res;

	fw_size = load_part_binary(vcp->vcp_cluster->sram_base +
				   vcp->vcp_cluster->sram_offset[MMUP_ID],
				   fw_src, size, VCP_MMUP_PART_NAME);
	if (!fw_size) {
		dev_err(vcp->dev, "Failed to load %s\n", VCP_MMUP_PART_NAME);
		return -EINVAL;
	}

	dram_img_size = load_part_binary(img_buf_va + MMUP_DRAM_IMG_OFFSET, fw_src, size,
					 VCP_MMUP_DRAM_PART_NAME);
	if (!dram_img_size) {
		dev_err(vcp->dev, "Failed to load %s\n", VCP_MMUP_DRAM_PART_NAME);
		return -EINVAL;
	}

	vcp_region_info.struct_size = sizeof(struct vcp_region_info_st);

	dram_backup_img_offset = MMUP_DRAM_IMG_OFFSET + round_up(dram_img_size, ALIGN_1024);
	vcp_region_info.ap_dram_start = VCP_PACK_IOVA(img_buf_iova + MMUP_DRAM_IMG_OFFSET);
	vcp_region_info.ap_dram_backup_start = VCP_PACK_IOVA(img_buf_iova + dram_backup_img_offset);
	vcp_region_info.ap_dram_size = dram_img_size;

	memcpy_toio(vcp->vcp_cluster->sram_base +
		    vcp->vcp_cluster->sram_offset[MMUP_ID] + REGION_OFFSET,
		    &vcp_region_info, sizeof(vcp_region_info));

	arm_smccc_smc(MTK_SIP_TINYSYS_VCP_CONTROL,
		      MTK_TINYSYS_MMUP_KERNEL_OP_SET_FW_SIZE,
		      fw_size, 0, 0, 0, 0, 0, &res);
	if (res.a0 != 1) {
		dev_err(vcp->dev, "Set firmware size SMC failed: %ld\n", res.a0);
		return -EIO;
	}

	return 0;
}

int mtk_vcp_load(struct rproc *rproc, const struct firmware *fw)
{
	struct arm_smccc_res res;
	struct mtk_vcp_device *vcp = rproc->priv;
	dma_addr_t img_buf_iova;
	phys_addr_t img_buf_phys;
	void __iomem *img_buf_va;
	int ret;

	if (!vcp)
		return -EINVAL;

	if (fw->size < VCP_IMAGE_HEADER_SIZE ||
	    fw->size > vcp->ops->get_mem_size(vcp, VCP_RTOS_MEM_ID)) {
		dev_err(vcp->dev, "Invalid firmware size\n");
		return -EINVAL;
	}

	writel(0x1, vcp->vcp_cluster->cfg_core + VCP_R_CORE0_SW_RSTN_SET);
	writel(0x1, vcp->vcp_cluster->cfg_core + VCP_R_CORE1_SW_RSTN_SET);

	memset_io(vcp->vcp_cluster->sram_base, 0, vcp->vcp_cluster->sram_size);

	img_buf_iova = vcp->ops->get_mem_iova(vcp, VCP_RTOS_MEM_ID);
	img_buf_phys = vcp->ops->get_mem_phys(vcp, VCP_RTOS_MEM_ID);
	img_buf_va = vcp->ops->get_mem_virt(vcp, VCP_RTOS_MEM_ID);

	arm_smccc_smc(MTK_SIP_TINYSYS_VCP_CONTROL,
		      MTK_TINYSYS_VCP_KERNEL_OP_COLD_BOOT_VCP,
		      0, 0, 0, 0, 0, 0, &res);
	if (res.a0 != 1) {
		dev_err(vcp->dev, "Cold boot SMC failed: %ld\n", res.a0);
		return -EIO;
	}

	ret = load_vcp_bin(fw->data, fw->size,
			   img_buf_va, img_buf_phys,
			   img_buf_iova, vcp);
	if (ret)
		return ret;

	ret = load_mmup_bin(fw->data, fw->size,
			    img_buf_va, img_buf_phys,
			    img_buf_iova, vcp);
	if (ret)
		return ret;

	return 0;
}

static irqreturn_t vcp_irq_handler(int irq, void *priv)
{
	u32 reg0, reg1;
	struct mtk_vcp_device *vcp = priv;

	reg0 = readl(vcp->vcp_cluster->cfg_core + R_CORE0_WDT_IRQ);
	reg1 = vcp->vcp_cluster->core_nums > VCP_ID ?
	       readl(vcp->vcp_cluster->cfg_core + R_CORE1_WDT_IRQ) : 0;

	if (!reg0 && !reg1)
		return IRQ_NONE;

	if (reg0) {
		writel(B_WDT_IRQ, vcp->vcp_cluster->cfg_core + R_CORE0_WDT_IRQ);
		dev_err(vcp->dev, "VCP core watchdog timeout\n");
	}

	if (reg1) {
		writel(B_WDT_IRQ, vcp->vcp_cluster->cfg_core + R_CORE1_WDT_IRQ);
		dev_err(vcp->dev, "MMUP core watchdog timeout\n");
	}

	return IRQ_HANDLED;
}

int vcp_wdt_irq_init(struct mtk_vcp_device *vcp)
{
	int ret;

	ret = devm_request_irq(vcp->dev, platform_get_irq(vcp->pdev, 0),
			       vcp_irq_handler, IRQF_ONESHOT,
			       vcp->pdev->name, vcp);
	if (ret)
		dev_err_probe(vcp->dev, ret, "failed to request wdt irq\n");

	return ret;
}

MODULE_AUTHOR("Xiangzhi Tang <xiangzhi.tang@mediatek.com>");
MODULE_DESCRIPTION("MTK VCP Controller");
MODULE_LICENSE("GPL");
