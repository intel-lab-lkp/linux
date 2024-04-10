/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright © 2024 Apple Inc. All rights reserved.
 */

#ifndef __ASM_TSO_H
#define __ASM_TSO_H

#ifdef CONFIG_ARM64_TSO

#include <linux/sched.h>
#include <linux/types.h>

int modify_tso_enable(bool tso_enable);
void tso_thread_switch(struct task_struct *next);

#endif /* CONFIG_ARM64_TSO */
#endif /* __ASM_TSO_H */
