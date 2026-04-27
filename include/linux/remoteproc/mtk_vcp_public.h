/* SPDX-License-Identifier: (GPL-2.0 OR MIT) */
/*
 * Copyright (c) 2025 MediaTek Inc.
 */

#ifndef __MTK_VCP_PUBLIC_H__
#define __MTK_VCP_PUBLIC_H__

#include <linux/platform_device.h>
#include <linux/firmware/mediatek/mtk-vcp-ipc.h>
#include <linux/remoteproc.h>

#define VCP_SYNC_TIMEOUT_MS             (50)

enum vcp_notify_event {
	VCP_EVENT_READY = 0,
	VCP_EVENT_STOP,
	VCP_EVENT_SUSPEND,
	VCP_EVENT_RESUME,
};

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

enum {
	IPI_OUT_VDEC_1                 =  0,
	IPI_IN_VDEC_1                  =  1,
	IPI_OUT_C_SLEEP_0              =  2,
	IPI_OUT_TEST_0                 =  3,
	IPI_IN_VCP_READY_0             =  5,
	IPI_OUT_MMDVFS_VCP             =  9,
	IPI_IN_MMDVFS_VCP              = 10,
	IPI_OUT_MMQOS                  = 11,
	IPI_IN_MMQOS                   = 12,
	IPI_OUT_MMDEBUG                = 13,
	IPI_IN_MMDEBUG                 = 14,
	IPI_OUT_C_VCP_HWVOTER_DEBUG    = 15,
	IPI_OUT_VENC_0                 = 16,
	IPI_IN_VENC_0                  = 17,
	IPI_OUT_C_SLEEP_1              = 20,
	IPI_OUT_TEST_1                 = 21,
	IPI_OUT_LOGGER_CTRL_0          = 22,
	IPI_OUT_VCPCTL_1               = 23,
	IPI_IN_LOGGER_CTRL_0           = 25,
	IPI_IN_VCP_READY_1             = 26,
	IPI_OUT_LOGGER_CTRL_1          = 30,
	IPI_IN_LOGGER_CTRL_1           = 31,
	IPI_OUT_VCPCTL_0               = 32,
	IPI_OUT_MMDVFS_MMUP            = 33,
	IPI_IN_MMDVFS_MMUP             = 34,
	IPI_OUT_VDISP                  = 35,
	VCP_IPI_COUNT,
	VCP_IPI_NS_SERVICE             = 0xff,
	VCP_IPI_NS_SERVICE_COUNT       = 0x100,
};

struct mtk_vcp_device {
	struct platform_device *pdev;
	struct device *dev;
	struct rproc *rproc;
	struct mtk_ipi_device *ipi_dev;
	struct mtk_vcp_of_cluster *vcp_cluster;
	const struct mtk_vcp_ipi_ops *ipi_ops;
	const struct mtk_vcp_ops *ops;
	const struct mtk_vcp_platdata *platdata;
};

struct mtk_vcp_ipi_ops {
	int (*ipi_send)(struct mtk_ipi_device *ipidev, u32 ipi_id,
			void *data, u32 len);
	int (*ipi_send_compl)(struct mtk_ipi_device *ipidev, u32 ipi_id,
			      void *data, u32 len, u32 timeout_ms);
	int (*ipi_register)(struct mtk_ipi_device *ipidev, int ipi_id,
			    mbox_pin_cb_t cb, void *prdata, void *msg);
	int (*ipi_unregister)(struct mtk_ipi_device *ipidev, int ipi_id);
};

struct mtk_vcp_ops {
	bool (*vcp_is_suspending)(struct mtk_vcp_device *vcp);
	bool (*vcp_is_ready)(struct mtk_vcp_device *vcp,
			     enum vcp_feature_id id);
	int (*register_feature)(struct mtk_vcp_device *vcp,
				enum vcp_feature_id id);
	int (*deregister_feature)(struct mtk_vcp_device *vcp,
				  enum vcp_feature_id id);
	void (*register_notify)(struct mtk_vcp_device *vcp,
				enum vcp_feature_id id,
				struct notifier_block *nb);
	void (*unregister_notify)(struct mtk_vcp_device *vcp,
				  enum vcp_feature_id id,
				  struct notifier_block *nb);
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
struct mtk_ipi_device *vcp_get_ipidev(struct mtk_vcp_device *vcp);

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
