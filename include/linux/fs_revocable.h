/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright 2025 Google LLC
 */

#ifndef __LINUX_FS_REVOCABLE_H
#define __LINUX_FS_REVOCABLE_H

#include <linux/fs.h>
#include <linux/revocable.h>

int fs_revocable_replace(struct revocable_provider *rp, struct file *filp);

#endif /* __LINUX_FS_REVOCABLE_H */
