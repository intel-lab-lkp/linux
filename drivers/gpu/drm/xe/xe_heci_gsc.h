/* SPDX-License-Identifier: MIT */
/*
 * Copyright(c) 2023, Intel Corporation. All rights reserved.
 */
#ifndef _XE_HECI_GSC_H_
#define _XE_HECI_GSC_H_

#include <linux/types.h>

struct xe_bo;
struct xe_device;
struct mei_aux_device;

/*
 * GSC HECI1 bit corresponds to bit15 and HECI2 to bit14.
 * The reason for this is to allow growth for more interfaces in the future.
 */
#define GSC_IRQ_INTF(_x) BIT(15 - (_x))

/*
 * CSC HECI1 bit corresponds to bit9 and HECI2 to bit10.
 */
#define CSC_IRQ_INTF(_x) BIT(9 + (_x))

enum xe_heci_intf {
	XE_HECI1,
	XE_HECI2,
	XE_HECI_COUNT,
};

/**
 * struct xe_heci_gsc_intf - one HECI interface of the GSC
 *
 * @adev: mei auxiliary device of the interface
 * @irq: irq number, -1 when polled
 * @ext_op_mem: VRAM the GSC uses as extended operational memory, if any
 */
struct xe_heci_gsc_intf {
	struct mei_aux_device *adev;
	int irq;
	struct xe_bo *ext_op_mem;
};

/**
 * struct xe_heci_gsc - graphics security controller for xe, HECI interfaces
 *
 * @intf: HECI1 (DG2 PXP and HuC) and HECI2 (firmware interface)
 */
struct xe_heci_gsc {
	struct xe_heci_gsc_intf intf[XE_HECI_COUNT];
};

int xe_heci_gsc_init(struct xe_device *xe);
void xe_heci_gsc_irq_handler(struct xe_device *xe, u32 iir);
void xe_heci_csc_irq_handler(struct xe_device *xe, u32 iir);

#endif /* _XE_HECI_GSC_H_ */
