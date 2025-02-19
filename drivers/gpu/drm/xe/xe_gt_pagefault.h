/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_GT_PAGEFAULT_H_
#define _XE_GT_PAGEFAULT_H_

#include <linux/types.h>

struct xe_gt;
struct xe_guc;

struct pagefault {
	u64 page_addr;
	u32 asid;
	u16 pdata;
	u8 vfid;
	u8 access_type;
	u8 fault_type;
	u8 fault_level;
	u8 engine_class;
	u8 engine_instance;
	u8 fault_unsuccessful;
	bool prefetch;
	bool trva_fault;
};

enum access_type {
	ACCESS_TYPE_READ = 0,
	ACCESS_TYPE_WRITE = 1,
	ACCESS_TYPE_ATOMIC = 2,
	ACCESS_TYPE_RESERVED = 3,
};

enum fault_type {
	NOT_PRESENT = 0,
	WRITE_ACCESS_VIOLATION = 1,
	ATOMIC_ACCESS_VIOLATION = 2,
};

int xe_gt_pagefault_init(struct xe_gt *gt);
void xe_gt_pagefault_reset(struct xe_gt *gt);
int xe_guc_pagefault_handler(struct xe_guc *guc, u32 *msg, u32 len);
int xe_guc_access_counter_notify_handler(struct xe_guc *guc, u32 *msg, u32 len);

#endif	/* _XE_GT_PAGEFAULT_ */
