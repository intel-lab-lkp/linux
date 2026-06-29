/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 ARM Ltd.
 * Author: Leonardo Bras <leo.bras@arm.com>
 */

#ifndef __ARM64_KVM_DIRTY_BIT_H__
#define __ARM64_KVM_DIRTY_BIT_H__

#include <asm/kvm_pgtable.h>

enum hacdbs_status {
	HACDBS_OFF,
	HACDBS_IDLE,
	HACDBS_RUNNING,
	HACDBS_ERROR
};

struct hacdbs {
	enum hacdbs_status status;
	int size;
};

DECLARE_PER_CPU(struct hacdbs, hacdbs_pcp);

void __init kvm_hacdbs_init(void);
void kvm_hacdbs_cpu_up(void);
void kvm_hacdbs_cpu_down(void);

#endif /* __ARM64_KVM_DIRTY_BIT_H__ */
