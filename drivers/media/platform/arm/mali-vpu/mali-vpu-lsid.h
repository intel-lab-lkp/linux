/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 ARM Limited.
 */

#ifndef __MALI_VPU_LSID_H__
#define __MALI_VPU_LSID_H__

#include <linux/types.h>

struct mali_vpu_dev;

/*
 * A local session identifier (LSID) represents a hardware execution
 * context. A firmware session must be mapped to an LSID before it can be
 * scheduled on the VPU cores.
 */

/**
 * struct mali_vpu_lsid_pcb - LSID process control block.
 * @ctrl: Saved LSID control register value.
 * @mmu_ctrl: Saved LSID MMU control register value.
 * @irqhost: Saved pending host IRQ state.
 * @nprot: Saved non-protected/protected allocation state.
 *
 * This structure is used to store the register map when a session is unmapped
 * from a LSID, so it can be restored again when the session is remapped.
 */
struct mali_vpu_lsid_pcb {
	u32 ctrl;
	u32 mmu_ctrl;
	u32 irqhost;
	u32 nprot;
};

/**
 * struct mali_vpu_lsid - Hardware LSID instance.
 * @vpu_dev: Back pointer to parent Mali VPU device.
 * @id: Hardware LSID instance.
 */
struct mali_vpu_lsid {
	struct mali_vpu_dev *vpu_dev;
	u32 id;
};

void mali_vpu_lsid_init(struct mali_vpu_lsid *lsid, struct mali_vpu_dev *vpu_dev, u32 id);
int mali_vpu_lsid_map(struct mali_vpu_lsid *lsid, struct mali_vpu_lsid_pcb *pcb);
void mali_vpu_lsid_unmap(struct mali_vpu_lsid *lsid, struct mali_vpu_lsid_pcb *pcb);
int mali_vpu_lsid_jobqueue_add(struct mali_vpu_lsid *lsid, u32 ncores);
void mali_vpu_lsid_jobqueue_remove(struct mali_vpu_lsid *lsid);
void mali_vpu_lsid_send_irq(struct mali_vpu_lsid *lsid);
void mali_vpu_lsid_flush_mmu(struct mali_vpu_lsid *lsid);
int mali_vpu_lsid_terminate(struct mali_vpu_lsid *lsid);

#endif /* __MALI_VPU_LSID_H__ */
