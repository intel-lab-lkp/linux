/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Author: Jianmin Lv <lvjianmin@loongson.cn>
 *         Huacai Chen <chenhuacai@loongson.cn>
 *
 * Copyright (C) 2020-2022 Loongson Technology Corporation Limited
 */

#ifndef _ASM_LOONGARCH_NUMA_H
#define _ASM_LOONGARCH_NUMA_H

#include <linux/nodemask.h>

#define NODE_ADDRSPACE_SHIFT 44

#define pa_to_nid(addr)		(((addr) & 0xf00000000000) >> NODE_ADDRSPACE_SHIFT)
#define nid_to_addrbase(nid)	(_ULCAST_(nid) << NODE_ADDRSPACE_SHIFT)

#include <asm/topology.h>
#include <asm-generic/numa.h>

#endif	/* _ASM_LOONGARCH_NUMA_H */
