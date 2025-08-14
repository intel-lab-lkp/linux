/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2025 HiSilicon Limited
 * Author: Qinxin Xia <xiaqinxin@huawei.com>
 *
 */

#ifndef __ASM_IO_PTDUMP_H
#define __ASM_IO_PTDUMP_H

#ifdef CONFIG_IO_PTDUMP_DEBUGFS
void __init io_ptdump_debugfs_register(const char *name);
#else
void __init io_ptdump_debugfs_register(const char *name) { }
#endif /* CONFIG_IO_PTDUMP_DEBUGFS  */

#endif
