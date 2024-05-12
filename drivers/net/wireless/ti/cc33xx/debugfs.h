/* SPDX-License-Identifier: GPL-2.0-only */

/* Copyright (C) 2022-2024 Texas Instruments Inc.*/

#ifndef __DEBUGFS_H__
#define __DEBUGFS_H__

#include "cc33xx.h"

__printf(4, 5) int cc33xx_format_buffer(char __user *userbuf, size_t count,
					loff_t *ppos, char *fmt, ...);

int cc33xx_debugfs_init(struct cc33xx *cc);
void cc33xx_debugfs_exit(struct cc33xx *cc);
void cc33xx_debugfs_reset(struct cc33xx *cc);
void cc33xx_debugfs_update_stats(struct cc33xx *cc);

#define DEBUGFS_FORMAT_BUFFER_SIZE 256

#define DEBUGFS_READONLY_FILE(name, fmt, value...)			\
static ssize_t name## _read(struct file *file, char __user *userbuf,	\
			    size_t count, loff_t *ppos)			\
{									\
	struct cc33xx *cc = file->private_data;				\
	return cc33xx_format_buffer(userbuf, count, ppos,		\
				    fmt "\n", ##value);			\
}									\
									\
static const struct file_operations name## _ops = {			\
	.read = name## _read,						\
	.open = simple_open,						\
	.llseek	= generic_file_llseek,					\
}

#define DEBUGFS_ADD(name, parent)					\
		debugfs_create_file(#name, 0400, parent,		\
				    cc, &name## _ops)

#define DEBUGFS_ADD_PREFIX(prefix, name, parent)			\
		debugfs_create_file(#name, 0400, parent,		\
				    cc, &prefix## _## name## _ops)

#define DEBUGFS_FWSTATS_FILE(sub, name, fmt, struct_type)		\
static ssize_t sub## _ ##name## _read(struct file *file,		\
				      char __user *userbuf,		\
				      size_t count, loff_t *ppos)	\
{									\
	struct cc33xx *cc = file->private_data;				\
	struct struct_type *stats = cc->stats.fw_stats;			\
									\
	cc33xx_debugfs_update_stats(cc);				\
									\
	return cc33xx_format_buffer(userbuf, count, ppos, fmt "\n",	\
				    stats->sub.name);			\
}									\
									\
static const struct file_operations sub## _ ##name## _ops = {		\
	.read = sub## _ ##name## _read,					\
	.open = simple_open,						\
	.llseek	= generic_file_llseek,					\
}

#define DEBUGFS_FWSTATS_FILE_ARRAY(sub, name, len, struct_type)		\
static ssize_t sub## _ ##name## _read(struct file *file,		\
				      char __user *userbuf,		\
				      size_t count, loff_t *ppos)	\
{									\
	struct cc33xx *cc = file->private_data;				\
	struct struct_type *stats = cc->stats.fw_stats;			\
	char buf[DEBUGFS_FORMAT_BUFFER_SIZE] = "";			\
	int res, i;							\
									\
	cc33xx_debugfs_update_stats(cc);				\
									\
	for (i = 0; i < (len); i++)					\
		res = snprintf(buf, sizeof(buf), "%s[%d] = %d\n",	\
			       buf, i, stats->sub.name[i]);		\
									\
	return cc33xx_format_buffer(userbuf, count, ppos, "%s", buf);	\
}									\
									\
static const struct file_operations sub## _ ##name## _ops = {		\
	.read = sub## _ ##name## _read,					\
	.open = simple_open,						\
	.llseek	= generic_file_llseek,					\
}

#define DEBUGFS_FWSTATS_ADD(sub, name)					\
	DEBUGFS_ADD(sub## _ ##name, stats)

#endif /* CC33XX_DEBUGFS_H */
