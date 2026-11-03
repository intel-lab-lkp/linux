/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_WRITE_STREAMS_H
#define _LINUX_WRITE_STREAMS_H

#include <linux/spinlock.h>
#include <linux/types.h>

struct file;
struct inode;

/* i_write_stream value of an inode that has no write stream */
#define WRITE_STREAM_NOT_SET	0

/*
 * Write stream slot pool, embedded in whatever filesystem object scopes
 * an id space.
 */
struct write_stream_pool {
	unsigned int		nr_streams;
	unsigned long		*streams_in_use;
	spinlock_t		lock;
};

static inline unsigned int
write_stream_pool_count(const struct write_stream_pool *pool)
{
	return pool->nr_streams;
}

int  write_stream_pool_init(struct write_stream_pool *pool, unsigned int nr);
void write_stream_pool_destroy(struct write_stream_pool *pool);

int  write_stream_alloc_fd(struct write_stream_pool *pool, struct file *file);
int  write_stream_get_id(struct file *file,
			 const struct write_stream_pool *pool);

int  inode_set_write_stream(struct inode *inode, u16 id);

#endif /* _LINUX_WRITE_STREAMS_H */
