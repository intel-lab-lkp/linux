/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM dma_buf

#if !defined(_TRACE_DMA_BUF_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_DMA_BUF_H

#include <linux/dma-buf.h>
#include <linux/tracepoint.h>

TRACE_EVENT(dma_buf_export,

	TP_PROTO(struct dma_buf *dmabuf),

	TP_ARGS(dmabuf),

	TP_STRUCT__entry(
		__string(exp_name, dmabuf->exp_name)
		__string(name, dmabuf->name)
		__field(size_t, size)
		__field(ino_t, ino)
	),

	TP_fast_assign(
		__assign_str(exp_name);
		__assign_str(name);
		__entry->size = dmabuf->size;
		__entry->ino = dmabuf->file->f_inode->i_ino;
	),

	TP_printk("exp_name=%s name=%s size=%zu ino=%lu",
		  __get_str(exp_name),
		  __get_str(name),
		  __entry->size,
		  __entry->ino)
);

TRACE_EVENT(dma_buf_fd,

	TP_PROTO(struct dma_buf *dmabuf, int fd),

	TP_ARGS(dmabuf, fd),

	TP_STRUCT__entry(
		__string(exp_name, dmabuf->exp_name)
		__string(name, dmabuf->name)
		__field(size_t, size)
		__field(ino_t, ino)
		__field(int, fd)
	),

	TP_fast_assign(
		__assign_str(exp_name);
		__assign_str(name);
		__entry->size = dmabuf->size;
		__entry->ino = dmabuf->file->f_inode->i_ino;
		__entry->fd = fd;
	),

	TP_printk("exp_name=%s name=%s size=%zu ino=%lu fd=%d",
		  __get_str(exp_name),
		  __get_str(name),
		  __entry->size,
		  __entry->ino,
		  __entry->fd)
);

TRACE_EVENT(dma_buf_mmap_internal,

	TP_PROTO(struct dma_buf *dmabuf),

	TP_ARGS(dmabuf),

	TP_STRUCT__entry(
		__string(exp_name, dmabuf->exp_name)
		__string(name, dmabuf->name)
		__field(size_t, size)
		__field(ino_t, ino)
	),

	TP_fast_assign(
		__assign_str(exp_name);
		__assign_str(name);
		__entry->size = dmabuf->size;
		__entry->ino = dmabuf->file->f_inode->i_ino;
	),

	TP_printk("exp_name=%s name=%s size=%zu ino=%lu",
		  __get_str(exp_name),
		  __get_str(name),
		  __entry->size,
		  __entry->ino)
);

TRACE_EVENT(dma_buf_mmap,

	TP_PROTO(struct dma_buf *dmabuf),

	TP_ARGS(dmabuf),

	TP_STRUCT__entry(
		__string(exp_name, dmabuf->exp_name)
		__string(name, dmabuf->name)
		__field(size_t, size)
		__field(ino_t, ino)
	),

	TP_fast_assign(
		__assign_str(exp_name);
		__assign_str(name);
		__entry->size = dmabuf->size;
		__entry->ino = dmabuf->file->f_inode->i_ino;
	),

	TP_printk("exp_name=%s name=%s size=%zu ino=%lu",
		  __get_str(exp_name),
		  __get_str(name),
		  __entry->size,
		  __entry->ino)
);

TRACE_EVENT(dma_buf_attach,

	TP_PROTO(struct dma_buf *dmabuf, struct device *dev),

	TP_ARGS(dmabuf, dev),

	TP_STRUCT__entry(
		__string(dname, dev_name(dev))
		__string(exp_name, dmabuf->exp_name)
		__string(name, dmabuf->name)
		__field(size_t, size)
		__field(ino_t, ino)
	),

	TP_fast_assign(
		__assign_str(dname);
		__assign_str(exp_name);
		__assign_str(name);
		__entry->size = dmabuf->size;
		__entry->ino = dmabuf->file->f_inode->i_ino;
	),

	TP_printk("dev_name=%s exp_name=%s name=%s size=%zu ino=%lu",
		  __get_str(dname),
		  __get_str(exp_name),
		  __get_str(name),
		  __entry->size,
		  __entry->ino)
);

TRACE_EVENT(dma_buf_detach,

	TP_PROTO(struct dma_buf *dmabuf),

	TP_ARGS(dmabuf),

	TP_STRUCT__entry(
		__string(exp_name, dmabuf->exp_name)
		__string(name, dmabuf->name)
		__field(size_t, size)
		__field(ino_t, ino)
	),

	TP_fast_assign(
		__assign_str(exp_name);
		__assign_str(name);
		__entry->size = dmabuf->size;
		__entry->ino = dmabuf->file->f_inode->i_ino;
	),

	TP_printk("exp_name=%s name=%s size=%zu ino=%lu",
		  __get_str(exp_name),
		  __get_str(name),
		  __entry->size,
		  __entry->ino)
);

TRACE_EVENT(dma_buf_get,

	TP_PROTO(int fd, struct file *file),

	TP_ARGS(fd, file),

	TP_STRUCT__entry(
		__string(exp_name, ((struct dma_buf *)file->private_data)->exp_name)
		__string(name, ((struct dma_buf *)file->private_data)->name)
		__field(size_t, size)
		__field(ino_t, ino)
		__field(int, fd)
		__field(long, f_ref)
	),

	TP_fast_assign(
		__assign_str(exp_name);
		__assign_str(name);
		__entry->size = ((struct dma_buf *)file->private_data)->size;
		__entry->ino = ((struct dma_buf *)file->private_data)->file->f_inode->i_ino;
		__entry->fd = fd;
		__entry->f_ref = file_ref_get(&file->f_ref);
	),

	TP_printk("exp_name=%s name=%s size=%zu ino=%lu fd=%d f_ref=%ld",
		  __get_str(exp_name),
		  __get_str(name),
		  __entry->size,
		  __entry->ino,
		  __entry->fd,
		  __entry->f_ref)
);

TRACE_EVENT(dma_buf_put,

	TP_PROTO(struct dma_buf *dmabuf),

	TP_ARGS(dmabuf),

	TP_STRUCT__entry(
		__string(exp_name, dmabuf->exp_name)
		__string(name, dmabuf->name)
		__field(size_t, size)
		__field(ino_t, ino)
		__field(long, f_ref)
	),

	TP_fast_assign(
		__assign_str(exp_name);
		__assign_str(name);
		__entry->size = dmabuf->size;
		__entry->ino = dmabuf->file->f_inode->i_ino;
		__entry->f_ref = file_ref_get(&dmabuf->file->f_ref);
	),

	TP_printk("exp_name=%s name=%s size=%zu ino=%lu f_ref=%ld",
		  __get_str(exp_name),
		  __get_str(name),
		  __entry->size,
		  __entry->ino,
		  __entry->f_ref)
);

#endif /* _TRACE_DMA_BUF_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
