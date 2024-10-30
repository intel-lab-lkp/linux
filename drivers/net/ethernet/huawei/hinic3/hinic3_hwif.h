/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved. */

#ifndef HINIC3_HWIF_H
#define HINIC3_HWIF_H

#include "hinic3_common.h"

struct hinic3_hwdev;

enum func_type {
	TYPE_PF = 0,
	TYPE_VF = 1,
};

enum hinic3_fault_err_level {
	FAULT_LEVEL_FATAL,
	FAULT_LEVEL_SERIOUS_RESET,
	FAULT_LEVEL_HOST,
	FAULT_LEVEL_SERIOUS_FLR,
	FAULT_LEVEL_GENERAL,
	FAULT_LEVEL_SUGGESTION,
	FAULT_LEVEL_MAX,
};

struct hinic3_db_area {
	unsigned long *db_bitmap_array;
	u32           db_max_areas;
	/* protect doorbell area alloc and free */
	spinlock_t    idx_lock;
};

struct hinic3_func_attr {
	enum func_type func_type;
	u16            func_global_idx;
	u16            global_vf_id_of_pf;
	/* max: 2 ^ 15 */
	u16            num_irqs;
	/* max: 2 ^ 8 */
	u16            num_sq;
	u8             port_to_port_idx;
	u8             pci_intf_idx;
	u8             ppf_idx;
	/* max: 2 ^ 3 */
	u8             num_aeqs;
	/* max: 2 ^ 7 */
	u8             num_ceqs;
	u8             msix_flex_en;
};

static_assert(sizeof(struct hinic3_func_attr) == 20);

struct hinic3_hwif {
	u8 __iomem              *cfg_regs_base;
	u8 __iomem              *intr_regs_base;
	u8 __iomem              *mgmt_regs_base;
	u64                     db_base_phy;
	u64                     db_dwqe_len;
	u8 __iomem              *db_base;
	struct hinic3_db_area   db_area;
	struct hinic3_func_attr attr;
};

enum hinic3_outbound_ctrl {
	ENABLE_OUTBOUND  = 0x0,
	DISABLE_OUTBOUND = 0x1,
};

enum hinic3_doorbell_ctrl {
	ENABLE_DOORBELL  = 0x0,
	DISABLE_DOORBELL = 0x1,
};

enum hinic3_pf_status {
	HINIC3_PF_STATUS_INIT            = 0x0,
	HINIC3_PF_STATUS_ACTIVE_FLAG     = 0x11,
	HINIC3_PF_STATUS_FLR_START_FLAG  = 0x12,
	HINIC3_PF_STATUS_FLR_FINISH_FLAG = 0x13,
};

enum hinic3_msix_state {
	HINIC3_MSIX_ENABLE,
	HINIC3_MSIX_DISABLE,
};

enum hinic3_msix_auto_mask {
	HINIC3_CLR_MSIX_AUTO_MASK,
	HINIC3_SET_MSIX_AUTO_MASK,
};

#define HINIC3_FUNC_TYPE(hwdev)  ((hwdev)->hwif->attr.func_type)
#define HINIC3_IS_PF(hwdev)      (HINIC3_FUNC_TYPE(hwdev) == TYPE_PF)
#define HINIC3_IS_VF(hwdev)      (HINIC3_FUNC_TYPE(hwdev) == TYPE_VF)

u32 hinic3_hwif_read_reg(struct hinic3_hwif *hwif, u32 reg);
void hinic3_hwif_write_reg(struct hinic3_hwif *hwif, u32 reg, u32 val);

void hinic3_disable_doorbell(struct hinic3_hwif *hwif);
void hinic3_enable_doorbell(struct hinic3_hwif *hwif);

int hinic3_alloc_db_addr(struct hinic3_hwdev *hwdev, void __iomem **db_base,
			 void __iomem **dwqe_base);
void hinic3_free_db_addr(struct hinic3_hwdev *hwdev, const void __iomem *db_base,
			 void __iomem *dwqe_base);

void hinic3_set_pf_status(struct hinic3_hwif *hwif,
			  enum hinic3_pf_status status);
enum hinic3_pf_status hinic3_get_pf_status(struct hinic3_hwif *hwif);

int hinic3_init_hwif(struct hinic3_hwdev *hwdev);
void hinic3_free_hwif(struct hinic3_hwdev *hwdev);

void hinic3_set_msix_state(struct hinic3_hwdev *hwdev, u16 msix_idx,
			   enum hinic3_msix_state flag);
void hinic3_misx_intr_clear_resend_bit(struct hinic3_hwdev *hwdev, u16 msix_idx,
				       u8 clear_resend_en);
void hinic3_set_msix_auto_mask_state(struct hinic3_hwdev *hwdev, u16 msix_idx,
				     enum hinic3_msix_auto_mask flag);

u16 hinic3_global_func_id(struct hinic3_hwdev *hwdev);
u8 hinic3_pf_id_of_vf(struct hinic3_hwdev *hwdev);
u16 hinic3_glb_pf_vf_offset(struct hinic3_hwdev *hwdev);
u8 hinic3_ppf_idx(struct hinic3_hwdev *hwdev);

#endif
