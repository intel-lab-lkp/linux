/* SPDX-License-Identifier: (GPL-2.0 OR MIT) */
/*
 * Copyright (c) 2025 MediaTek Inc.
 */

#ifndef __MTK_VCP_PUBLIC_H__
#define __MTK_VCP_PUBLIC_H__

#include <linux/platform_device.h>
#include <linux/remoteproc.h>

enum vcp_reserve_mem_id {
	VCP_RTOS_MEM_ID,
	VDEC_MEM_ID,
	VENC_MEM_ID,
	MMDVFS_VCP_MEM_ID,
	MMDVFS_MMUP_MEM_ID,
	MMQOS_MEM_ID,
	NUMS_MEM_ID,
};

enum vcp_feature_id {
	RTOS_FEATURE_ID,
	VDEC_FEATURE_ID,
	VENC_FEATURE_ID,
	GCE_FEATURE_ID,
	MMDVFS_MMUP_FEATURE_ID,
	MMDVFS_VCP_FEATURE_ID,
	MMQOS_FEATURE_ID,
	MMDEBUG_FEATURE_ID,
	HWCCF_FEATURE_ID,
	HWCCF_DEBUG_FEATURE_ID,
	IMGSYS_FEATURE_ID,
	VDISP_FEATURE_ID,
	VMM_FEATURE_ID,
	NUM_FEATURE_ID,
};

struct mtk_vcp_device {
	struct platform_device *pdev;
	struct device *dev;
	struct rproc *rproc;
	struct mtk_vcp_of_cluster *vcp_cluster;
	const struct mtk_vcp_ops *ops;
	const struct mtk_vcp_platdata *platdata;
};

struct mtk_vcp_ops {
	phys_addr_t (*get_mem_phys)(struct mtk_vcp_device *vcp,
				    enum vcp_reserve_mem_id id);
	dma_addr_t (*get_mem_iova)(struct mtk_vcp_device *vcp,
				   enum vcp_reserve_mem_id id);
	void __iomem *(*get_mem_virt)(struct mtk_vcp_device *vcp,
				      enum vcp_reserve_mem_id id);
	size_t (*get_mem_size)(struct mtk_vcp_device *vcp,
			       enum vcp_reserve_mem_id id);
	void __iomem *(*get_vcp_sram_virt)(struct mtk_vcp_device *vcp);
};

struct mtk_vcp_device *vcp_get(struct platform_device *pdev);
void vcp_put(struct mtk_vcp_device *vcp);

/*
 * These inline functions are intended for user drivers that are loaded
 * earlier than the VCP driver, or for built-in drivers that cannot access
 * the symbols of VCP module.
 */
static inline struct mtk_vcp_device *mtk_vcp_get_by_phandle(phandle phandle)
{
	struct rproc *rproc = NULL;

	rproc = rproc_get_by_phandle(phandle);
	if (IS_ERR_OR_NULL(rproc))
		return NULL;

	return rproc->priv;
}
#endif
