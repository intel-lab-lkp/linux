/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * OpenRISC trap codes used internally by the kernel
 *
 * Linux architectural port borrowing liberally from similar works of
 * others.  All original copyrights apply as per the original source
 * declaration.
 *
 * Modifications for the OpenRISC architecture:
 * Copyright (C) 2026 Sahil Siddiq <sahilcdq0@gmail.com>
 */

#ifndef __ASM_BREAK_H
#define __ASM_BREAK_H

#define BRK_KPROBE_BP       512     /* Kprobe break */
#define BRK_KPROBE_SSTEPBP  1024    /* Kprobe single-step software implementation */

#endif /* __ASM_BREAK_H */
