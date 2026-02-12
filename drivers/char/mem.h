/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2025 Red Hat inc Alessandro Carminati <acarmina@redhat.com>
 */

#ifndef _LINUX_CHAR_MEM_H
#define _LINUX_CHAR_MEM_H

#if IS_ENABLED(CONFIG_KUNIT)
ssize_t read_mem(struct file *file, char __user *buf,
			size_t count, loff_t *ppos);
ssize_t write_mem(struct file *file, const char __user *buf,
			 size_t count, loff_t *ppos);
int page_is_allowed(unsigned long pfn);
#endif

#endif /* _LINUX_CHAR_MEM_H */
