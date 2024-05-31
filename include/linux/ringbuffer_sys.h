/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_RINGBUFFER_SYS_H
#define _LINUX_RINGBUFFER_SYS_H

struct file;
void ringbuffer_file_exit(struct file *file);

struct mm_struct;
void ringbuffer_mm_exit(struct mm_struct *mm);

struct ringbuffer;
size_t ringbuffer_read(struct ringbuffer *rb, void *data, size_t len, bool nonblocking);
size_t ringbuffer_write(struct ringbuffer *rb, void *data, size_t len, bool nonblocking);

#endif /* _LINUX_RINGBUFFER_SYS_H */
