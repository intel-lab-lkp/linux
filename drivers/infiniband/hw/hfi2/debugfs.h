/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
/*
 * Copyright(c) 2015, 2016, 2018 Intel Corporation.
 * Copyright(c) 2025-2026 Cornelis Networks, Inc.
 */

#ifndef _HFI2_DEBUGFS_H
#define _HFI2_DEBUGFS_H

struct hfi2_ibdev;

#define DEBUGFS_SEQ_FILE_OPS(name) \
static const struct seq_operations _##name##_seq_ops = { \
	.start = _##name##_seq_start, \
	.next  = _##name##_seq_next, \
	.stop  = _##name##_seq_stop, \
	.show  = _##name##_seq_show \
}

#define DEBUGFS_SEQ_FILE_OPEN(name) \
static int _##name##_open(struct inode *inode, struct file *s) \
{ \
	struct seq_file *seq; \
	int ret; \
	ret =  seq_open(s, &_##name##_seq_ops); \
	if (ret) \
		return ret; \
	seq = s->private_data; \
	seq->private = inode->i_private; \
	return 0; \
}

#define DEBUGFS_FILE_OPS(name) \
static const struct file_operations _##name##_file_ops = { \
	.owner   = THIS_MODULE, \
	.open    = _##name##_open, \
	.read    = seq_read, \
	.llseek  = seq_lseek, \
	.release = seq_release \
}

#ifdef CONFIG_DEBUG_FS
void hfi2_dbg_ibdev_init(struct hfi2_ibdev *ibd);
void hfi2_dbg_ibdev_exit(struct hfi2_ibdev *ibd);
void hfi2_dbg_init(void);
void hfi2_dbg_exit(void);

#else
static inline void hfi2_dbg_ibdev_init(struct hfi2_ibdev *ibd)
{
}

static inline void hfi2_dbg_ibdev_exit(struct hfi2_ibdev *ibd)
{
}

static inline void hfi2_dbg_init(void)
{
}

static inline void hfi2_dbg_exit(void)
{
}
#endif

#endif                          /* _HFI2_DEBUGFS_H */
