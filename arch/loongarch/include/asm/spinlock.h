/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020-2022 Loongson Technology Corporation Limited
 */
#ifndef _ASM_SPINLOCK_H
#define _ASM_SPINLOCK_H

#ifdef CONFIG_PARAVIRT
#define vcpu_is_preempted	vcpu_is_preempted
bool vcpu_is_preempted(int cpu);
#endif

#include <asm/processor.h>
#include <asm/qspinlock.h>
#include <asm/qrwlock.h>

#endif /* _ASM_SPINLOCK_H */
