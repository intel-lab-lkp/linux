/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM dma_buf

#if !defined(_TRACE_DMA_BUF_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_DMA_BUF_H

#include <linux/dma-buf.h>
#include <linux/tracepoint.h>

DECLARE_EVENT_CLASS(dma_buf,

	TP_PROTO(struct dma_buf *dmabuf),

	TP_ARGS(dmabuf),

	TP_STRUCT__entry(
		__string(exp_name, dmabuf->exp_name)
		__string(name, dmabuf->name)
		__field(size_t, size)
		__field(ino_t, ino)
		__field(long, f_refcnt)
	),

	TP_fast_assign(
		__assign_str(exp_name);
		__assign_str(name);
		__entry->size = dmabuf->size;
		__entry->ino = dmabuf->file->f_inode->i_ino;
		__entry->f_refcnt = file_count(dmabuf->file);
	),

	TP_printk("exp_name=%s name=%s size=%zu ino=%lu f_refcnt=%ld",
		  __get_str(exp_name),
		  __get_str(name),
		  __entry->size,
		  __entry->ino,
		  __entry->f_refcnt)
);

DECLARE_EVENT_CLASS(dma_buf_attach_dev,

	TP_PROTO(struct dma_buf *dmabuf, struct device *dev),

	TP_ARGS(dmabuf, dev),

	TP_STRUCT__entry(
		__string(dname, dev_name(dev))
		__string(exp_name, dmabuf->exp_name)
		__string(name, dmabuf->name)
		__field(size_t, size)
		__field(ino_t, ino)
		__field(long, f_refcnt)
	),

	TP_fast_assign(
		__assign_str(dname);
		__assign_str(exp_name);
		__assign_str(name);
		__entry->size = dmabuf->size;
		__entry->ino = dmabuf->file->f_inode->i_ino;
		__entry->f_refcnt = file_count(dmabuf->file);
	),

	TP_printk("dev_name=%s exp_name=%s name=%s size=%zu ino=%lu f_refcnt=%ld",
		  __get_str(dname),
		  __get_str(exp_name),
		  __get_str(name),
		  __entry->size,
		  __entry->ino,
		  __entry->f_refcnt)
);

DECLARE_EVENT_CLASS(dma_buf_fd,

	TP_PROTO(struct dma_buf *dmabuf, int fd),

	TP_ARGS(dmabuf, fd),

	TP_STRUCT__entry(
		__string(exp_name, dmabuf->exp_name)
		__string(name, dmabuf->name)
		__field(size_t, size)
		__field(ino_t, ino)
		__field(int, fd)
		__field(long, f_refcnt)
	),

	TP_fast_assign(
		__assign_str(exp_name);
		__assign_str(name);
		__entry->size = dmabuf->size;
		__entry->ino = dmabuf->file->f_inode->i_ino;
		__entry->fd = fd;
		__entry->f_refcnt = file_count(dmabuf->file);
	),

	TP_printk("exp_name=%s name=%s size=%zu ino=%lu fd=%d f_refcnt=%ld",
		  __get_str(exp_name),
		  __get_str(name),
		  __entry->size,
		  __entry->ino,
		  __entry->fd,
		  __entry->f_refcnt)
);

DEFINE_EVENT(dma_buf, dma_buf_export,

	TP_PROTO(struct dma_buf *dmabuf),

	TP_ARGS(dmabuf)
);

DEFINE_EVENT(dma_buf, dma_buf_mmap_internal,

	TP_PROTO(struct dma_buf *dmabuf),

	TP_ARGS(dmabuf)
);

DEFINE_EVENT(dma_buf, dma_buf_mmap,

	TP_PROTO(struct dma_buf *dmabuf),

	TP_ARGS(dmabuf)
);

DEFINE_EVENT(dma_buf, dma_buf_put,

	TP_PROTO(struct dma_buf *dmabuf),

	TP_ARGS(dmabuf)
);

DEFINE_EVENT(dma_buf_attach_dev, dma_buf_attach,

	TP_PROTO(struct dma_buf *dmabuf, struct device *dev),

	TP_ARGS(dmabuf, dev)
);

DEFINE_EVENT(dma_buf_attach_dev, dma_buf_detach,

	TP_PROTO(struct dma_buf *dmabuf, struct device *dev),

	TP_ARGS(dmabuf, dev)
);

DEFINE_EVENT(dma_buf_fd, dma_buf_fd,

	TP_PROTO(struct dma_buf *dmabuf, int fd),

	TP_ARGS(dmabuf, fd)
);

DEFINE_EVENT(dma_buf_fd, dma_buf_get,

	TP_PROTO(struct dma_buf *dmabuf, int fd),

	TP_ARGS(dmabuf, fd)
);

#endif /* _TRACE_DMA_BUF_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
