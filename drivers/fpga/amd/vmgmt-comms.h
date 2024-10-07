/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Driver for Versal PCIe device
 *
 * Copyright (C) 2024 Advanced Micro Devices, Inc. All rights reserved.
 */

#ifndef __VMGMT_COMMS_H
#define __VMGMT_COMMS_H

struct comms_device *vmgmtm_comms_init(struct vmgmt_device *vdev);
void vmgmtm_comms_fini(struct comms_device *ccdev);

#endif	/* __VMGMT_COMMS_H */
