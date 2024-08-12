/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2024 Google, Inc.
 */

#ifndef _LINUX_BINDER_GENL_H
#define _LINUX_BINDER_GENL_H

#include "binder_internal.h"

int binder_genl_init(void);

bool binder_genl_report_enabled(struct binder_proc *proc, u32 mask);

void binder_genl_send_report(struct binder_report *report, int len);

#endif /* _LINUX_BINDER_GENL_H */
