/* SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause) */
/*
 * Wave6 series multi-standard codec IP - debug interface
 *
 * Copyright (C) 2025 CHIPS&MEDIA INC
 */

#ifndef __WAVE6_VPU_DBG_H__
#define __WAVE6_VPU_DBG_H__

int wave6_vpu_create_dbgfs_file(struct vpu_instance *inst);
void wave6_vpu_remove_dbgfs_file(struct vpu_instance *inst);

#endif /* __WAVE6_VPU_DBG_H__ */
