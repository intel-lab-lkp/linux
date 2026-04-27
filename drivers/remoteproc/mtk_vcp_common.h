/* SPDX-License-Identifier: (GPL-2.0 OR MIT) */
/*
 * Copyright (c) 2025 MediaTek Inc.
 */

#ifndef __MTK_VCP_COMMON_H
#define __MTK_VCP_COMMON_H

#include <linux/arm-smccc.h>
#include <linux/bitops.h>
#include <linux/firmware.h>
#include <linux/soc/mediatek/mtk_sip_svc.h>
#include <linux/remoteproc/mtk_vcp_public.h>

/* VCP timeout definition */
#define VCP_READY_TIMEOUT_MS 3000
#define VCP_IPI_DEV_READY_TIMEOUT 1000
#define CORE_HART_SHUTDOWN_TIMEOUT_MS 10
#define SUSPEND_WAIT_TIMEOUT_MS 100

/* VCP platform definition */
#define DMA_MAX_MASK_BIT 33
#define RESUME_MAGIC 0x12345678
#define SUSPEND_MAGIC 0x87654321
#define PIN_OUT_C_SIZE_SLEEP_0 2

/* VCP load image definition */
#define VCM_IMAGE_MAGIC             (0x58881688)
#define VCM_IMAGE_NAME_MAXSZ        (32)
#define VCP_IMAGE_HEADER_SIZE       (0x200)

#define VCP_DRAM_IMG_OFFSET         (0x200000)
#define MMUP_DRAM_IMG_OFFSET        (0x1200000)

#define REGION_OFFSET               (0x4)
#define ALIGN_1024                  (1024)
#define ALIGN_16                    (16)
#define VCP_HFRP_PART_NAME          "tinysys-vcp-RV55_A"
#define VCP_MMUP_PART_NAME          "tinysys-mmup-RV33_A"
#define VCP_HFRP_DRAM_PART_NAME     "tinysys-vcp-RV55_A_dram"
#define VCP_MMUP_DRAM_PART_NAME     "tinysys-mmup-RV33_A_dram"

/* VCP memory iova pack convert definition */
#define VCP_PACK_IOVA(addr)     ((u32)((addr) | (((u64)(addr) >> 32) & 0xF)))
#define VCP_UNPACK_IOVA(addr)   \
	((u64)((addr) & 0xFFFFFFF0) | (((u64)(addr) & 0xF) << 32))

/* VCP cfg_core register offset definition */
#define VCP_R_CORE0_SW_RSTN_SET         (0x0004)
#define VCP_R_CORE1_SW_RSTN_SET         (0x000C)
#define R_GIPC_IN_SET                   (0x0028)
#define R_GIPC_IN_CLR                   (0x002C)
#define GIPC_MMUP_SHUT                  BIT(10)
#define GIPC_VCP_HART0_SHUT             BIT(14)
#define B_GIPC4_SETCLR_3                BIT(19)
#define R_CORE0_WDT_IRQ                 (0x0050)
#define R_CORE1_WDT_IRQ                 (0x0054)
#define B_WDT_IRQ                       BIT(0)
#define AP_R_GPR2                       (0x0068)
#define B_CORE0_SUSPEND                 BIT(0)
#define B_CORE0_RESUME                  BIT(1)
#define AP_R_GPR3                       (0x006C)
#define B_CORE1_SUSPEND                 BIT(0)
#define B_CORE1_RESUME                  BIT(1)

/* VCP cfg register offset definition */
#define R_CORE0_STATUS                  (0x6070)
#define B_CORE_GATED                    BIT(0)
#define B_HART0_HALT                    BIT(1)
#define B_HART1_HALT                    BIT(2)
#define B_CORE_AXIS_BUSY                BIT(4)
#define R_CORE1_STATUS                  (0x9070)
#define VCP_C0_GPR0_SUSPEND_RESUME      (0x6040)
#define VCP_C0_GPR1_DRAM_RESV_ADDR      (0x6044)
#define VCP_C0_GPR2_DRAM_RESV_SIZE      (0x6048)
#define VCP_C0_GPR3_DRAM_RESV_LOGGER    (0x604C)
#define VCP_C0_GPR5_H0_REBOOT           (0x6054)
#define CORE_RDY_TO_REBOOT              (0x0034)
#define VCP_C0_GPR6_H1_REBOOT           (0x6058)
#define VCP_C1_GPR0_SUSPEND_RESUME      (0x9040)
#define VCP_C1_GPR1_DRAM_RESV_ADDR      (0x9044)
#define VCP_C1_GPR2_DRAM_RESV_SIZE      (0x9048)
#define VCP_C1_GPR3_DRAM_RESV_LOGGER    (0x904C)
#define VCP_C1_GPR5_H0_REBOOT           (0x9054)
#define VCP_C1_GPR6_H1_REBOOT           (0x9058)

/* VCP cfg_sec register offset definition */
#define R_GPR2_SEC                      (0x0008)
#define MMUP_AP_SUSPEND                 BIT(0)
#define R_GPR3_SEC                      (0x000C)
#define VCP_AP_SUSPEND                  BIT(0)

enum vcp_core_id {
	VCP_ID = 0,
	MMUP_ID,
	VCP_CORE_TOTAL,
};

enum vcp_slp_cmd {
	SLP_WAKE_LOCK = 0,
	SLP_WAKE_UNLOCK,
	SLP_STATUS_DBG,
	SLP_SUSPEND,
	SLP_RESUME,
};

enum mtk_tinysys_vcp_kernel_op {
	MTK_TINYSYS_VCP_KERNEL_OP_RESET_SET = 0,
	MTK_TINYSYS_VCP_KERNEL_OP_RESET_RELEASE,
	MTK_TINYSYS_VCP_KERNEL_OP_COLD_BOOT_VCP,
	MTK_TINYSYS_MMUP_KERNEL_OP_RESET_SET,
	MTK_TINYSYS_MMUP_KERNEL_OP_RESET_RELEASE,
	MTK_TINYSYS_MMUP_KERNEL_OP_SET_L2TCM_OFFSET,
	MTK_TINYSYS_MMUP_KERNEL_OP_SET_FW_SIZE,
	MTK_TINYSYS_MMUP_KERNEL_OP_COLD_BOOT_MMUP,
	MTK_TINYSYS_VCP_KERNEL_OP_NUM,
};

/**
 * struct mtk_vcp_img_hdr - mtk image header format.
 *
 * @magic: mtk vcp image magic id
 * @dsz: mtk vcp image part binary size
 * @name: mtk vcp image part binary parttion name
 */
struct mtk_vcp_img_hdr {
	u32 magic;
	u32 dsz;
	char name[VCM_IMAGE_NAME_MAXSZ];
};

/**
 * struct mtk_vcp_feature_table - feature table structure definition.
 *
 * @feature_id: feature id
 * @core_id: feature using vcp core id
 */
struct mtk_vcp_feature_table {
	enum vcp_feature_id feature_id;
	enum vcp_core_id core_id;
};

/**
 * struct mtk_vcp_reserved_mem_table - memory table structure definition.
 *
 * @memory_id: memory_id id
 * @size: predistribution memory size
 */
struct mtk_vcp_reserved_mem_table {
	enum vcp_reserve_mem_id memory_id;
	size_t size;
};

/**
 * struct vcp_reserve_mblock - vcp reserved memory structure.
 *
 * @vcp_reserve_mem_id: reserved memory id
 * @phys: reserved memory phy addr
 * @iova: reserved memory dma map addr
 * @virt: reserved memory CPU virt addr
 * @size: reserved memory size
 */
struct vcp_reserve_mblock {
	enum vcp_reserve_mem_id num;
	phys_addr_t phys;
	dma_addr_t iova;
	void __iomem *virt;
	size_t size;
};

/**
 * struct vcp_slp_ctrl - sleep ctrl data sync with AP and VCP
 *
 * @feature: Feature id
 * @cmd: sleep cmd flag.
 */
struct vcp_slp_ctrl {
	u32 feature;
	u32 cmd;
};

/**
 * struct vcp_work_struct - vcp notify work structure.
 *
 * @work: struct work_struct member
 * @dev: struct device member
 * @u32 flags: vcp notify work flag
 * @id: vcp core id
 */
struct vcp_work_struct {
	struct work_struct work;
	struct device *dev;
	u32 flags;
	u32 id;
};

/**
 * struct vcp_region_info_st - config vcp image info sync to vcp bootloader.
 *
 * @ap_loader_start: config vcp bootloader to copy loader start addr
 * @ap_loader_size: config vcp bootloader to copy loader size
 * @ap_firmware_start: config vcp bootloader to copy firmware start addr
 * @ap_firmware_size: config vcp bootloader to copy firmware size
 * @ap_dram_start: config vcp run dram binary start addr
 * @ap_dram_size: config vcp run dram binary size
 * @ap_dram_backup_start: config vcp backup dram binary start addr
 * @struct_size: vcp image region info structure size
 * @l2tcm_offset: vcp two core using l2sram layout
 * @TaskContext_ptr: vcp task context ptr for debug
 * @vcpctl: - vcp control info
 * @regdump_start: regdump start addr for debug
 * @regdump_size: regdump size for debug
 * @ap_params_start: params start addr
 * @sramlog_buf_offset: sramlog_buf_offset for debug
 * @sramlog_end_idx_offset: sramlog_end_idx_offset for debug
 * @sramlog_buf_maxlen: sramlog_buf_maxlen for debug
 * @ap_loader_start_pa: config vcp bootloader for loader start pa
 * @coredump_offset: coredump_offset offset for debug
 * @coredump_dram_offset: coredump_dram_offset offset for debug
 *
 * This structure is shared with VCP firmware and must be kept in sync.
 */
struct vcp_region_info_st {
	u32 ap_loader_start;
	u32 ap_loader_size;
	u32 ap_firmware_start;
	u32 ap_firmware_size;
	u32 ap_dram_start;
	u32 ap_dram_size;
	u32 ap_dram_backup_start;
	u32 struct_size;
	u32 l2tcm_offset;
	u32 TaskContext_ptr;
	u32 vcpctl;
	u32 regdump_start;
	u32 regdump_size;
	u32 ap_params_start;
	u32 sramlog_buf_offset;
	u32 sramlog_end_idx_offset;
	u32 sramlog_buf_maxlen;
	u32 ap_loader_start_pa;
	u32 coredump_offset;
	u32 coredump_dram_offset;
} __packed;

int vcp_ready_ipi_handler(u32 id, void *prdata,
			  void *data, u32 len);
bool is_vcp_ready(struct mtk_vcp_device *vcp,
		  enum vcp_feature_id id);
int vcp_notify_work_init(struct mtk_vcp_device *vcp);
void vcp_extern_notify(enum vcp_core_id core_id,
		       enum vcp_notify_event notify_status);
void vcp_register_notify(struct mtk_vcp_device *vcp,
			 enum vcp_feature_id id,
			 struct notifier_block *nb);
void vcp_unregister_notify(struct mtk_vcp_device *vcp,
			   enum vcp_feature_id id,
			   struct notifier_block *nb);

int vcp_reserve_memory_init(struct mtk_vcp_device *vcp);
phys_addr_t vcp_get_reserve_mem_phys(struct mtk_vcp_device *vcp, enum vcp_reserve_mem_id id);
dma_addr_t vcp_get_reserve_mem_iova(struct mtk_vcp_device *vcp, enum vcp_reserve_mem_id id);
size_t vcp_get_reserve_mem_size(struct mtk_vcp_device *vcp, enum vcp_reserve_mem_id id);
void __iomem *vcp_get_reserve_mem_virt(struct mtk_vcp_device *vcp, enum vcp_reserve_mem_id id);
void __iomem *vcp_get_internal_sram_virt(struct mtk_vcp_device *vcp);

int reset_vcp(struct mtk_vcp_device *vcp);
int mtk_vcp_load(struct rproc *rproc, const struct firmware *fw);

int vcp_wdt_irq_init(struct mtk_vcp_device *vcp);

int vcp_register_feature(struct mtk_vcp_device *vcp,
			 enum vcp_feature_id id);
int vcp_deregister_feature(struct mtk_vcp_device *vcp,
			   enum vcp_feature_id id);

bool is_vcp_suspending(struct mtk_vcp_device *vcp);
int wait_core_hart_shutdown(struct mtk_vcp_device *vcp, enum vcp_core_id core_id);
void vcp_wait_core_stop(struct mtk_vcp_device *vcp, enum vcp_core_id core_id);
void vcp_wait_suspend_resume(struct mtk_vcp_device *vcp, bool suspend);
#endif
