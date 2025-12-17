/* SPDX-License-Identifier: GPL-2.0-or-later WITH Linux-syscall-note */
/* Copyright (c) 2024 HiSilicon Technologies Co., Ltd. */
#ifndef _UAPI_HISI_SOC_L3C_H
#define _UAPI_HISI_SOC_L3C_H

#include <linux/types.h>

/* HISI_L3C_INFO: cache lock info for HiSilicon SoC */
#define HISI_L3C_LOCK_INFO	_IOW(0xBB, 1, unsigned long)

/**
 * struct hisi_l3c_info - User data for hisi cache operates.
 * @lock_region_num: available locked memory region on a L3C instance
 * @lock_size: available size to be locked of the L3C instance.
 * @address_alignment: if the L3C lock requires locked region physical start
 *		       address to be aligned with the memory region size.
 * @max_lock_size: maximum locked memory size on a L3C instance.
 * @min_lock_size: minimum locked memory size on a L3C instance.
 */
struct hisi_l3c_lock_info {
	__u32 lock_region_num;
	__u64 lock_size;
	__u8 address_alignment;
	__u64 max_lock_size;
	__u64 min_lock_size;
};

#endif
