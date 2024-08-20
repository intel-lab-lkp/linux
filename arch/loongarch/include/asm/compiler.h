/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2024 Loongson Technology Corporation Limited
 */
#ifndef _ASM_COMPILER_H
#define _ASM_COMPILER_H

#ifdef barrier_before_unreachable
#undef barrier_before_unreachable
#define barrier_before_unreachable() do { } while (0)
#endif

#endif /* _ASM_COMPILER_H */
