/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022-2025 Intel Corporation
 */

#ifndef _XE_GT_PAGEFAULT_TYPES_H_
#define _XE_GT_PAGEFAULT_TYPES_H_

#include <linux/types.h>

/**
 * struct xe_gt_pagefault - Structure of pagefaults returned by the
 * pagefault handler
 */
struct xe_gt_pagefault {
	/** @page_addr: faulted address of this pagefault */
	u64 page_addr;
	/** @asid: ASID of this pagefault */
	u32 asid;
	/** @pdata: PDATA of this pagefault */
	u16 pdata;
	/** @vfid: VFID of this pagefault */
	u8 vfid;
	/** @access_type: access type of this pagefault */
	u8 access_type;
	/** @fault_type: fault type of this pagefault */
	u8 fault_type;
	/** @fault_level: fault level of this pagefault */
	u8 fault_level;
	/** @engine_class: engine class this pagefault was reported on */
	u8 engine_class;
	/** @engine_instance: engine instance this pagefault was reported on */
	u8 engine_instance;
	/** @fault_unsuccessful: flag for if the pagefault recovered or not */
	u8 fault_unsuccessful;
	/** @prefetch: unused */
	bool prefetch;
	/** @trva_fault: is set if this is a TRTT fault */
	bool trva_fault;
};

#endif
