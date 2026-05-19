/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __QDA_FASTRPC_H__
#define __QDA_FASTRPC_H__

#include <linux/completion.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/types.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/qda_accel.h>

/* Forward declarations */
struct qda_gem_obj;

/*
 * FastRPC scalar extraction macros
 *
 * These macros extract different fields from the scalar value that describes
 * the arguments passed in a FastRPC invocation.
 */
#define REMOTE_SCALARS_INBUFS(sc)	(((sc) >> 16) & 0x0ff)
#define REMOTE_SCALARS_OUTBUFS(sc)	(((sc) >> 8) & 0x0ff)
#define REMOTE_SCALARS_INHANDLES(sc)	(((sc) >> 4) & 0x0f)
#define REMOTE_SCALARS_OUTHANDLES(sc)	((sc) & 0x0f)
#define REMOTE_SCALARS_LENGTH(sc)	(REMOTE_SCALARS_INBUFS(sc) +   \
					 REMOTE_SCALARS_OUTBUFS(sc) +  \
					 REMOTE_SCALARS_INHANDLES(sc) + \
					 REMOTE_SCALARS_OUTHANDLES(sc))

/* FastRPC configuration constants */
#define FASTRPC_ALIGN		128		/* Alignment requirement */
#define FASTRPC_MAX_FDLIST	16		/* Maximum file descriptors */
#define FASTRPC_MAX_CRCLIST	64		/* Maximum CRC list entries */

/*
 * FastRPC scalar construction macros
 *
 * These macros build the scalar value that describes the arguments
 * for a FastRPC invocation.
 */
#define FASTRPC_BUILD_SCALARS(attr, method, in, out, oin, oout)		\
				(((attr & 0x07) << 29) |		\
				((method & 0x1f) << 24) |		\
				((in & 0xff) << 16) |			\
				((out & 0xff) <<  8) |			\
				((oin & 0x0f) <<  4) |			\
				(oout & 0x0f))

#define FASTRPC_SCALARS(method, in, out) \
		FASTRPC_BUILD_SCALARS(0, method, in, out, 0, 0)

/**
 * struct fastrpc_buf_overlap - Buffer overlap tracking structure
 *
 * Tracks overlapping buffer regions to optimise memory mapping and avoid
 * redundant mappings of the same physical memory.
 */
struct fastrpc_buf_overlap {
	/** @start: Start address of the buffer in user virtual address space */
	u64 start;
	/** @end: End address of the buffer in user virtual address space */
	u64 end;
	/** @raix: Remote argument index associated with this overlap */
	int raix;
	/** @mstart: Start address of the mapped region */
	u64 mstart;
	/** @mend: End address of the mapped region */
	u64 mend;
	/** @offset: Offset within the mapped region */
	u64 offset;
};

/**
 * struct fastrpc_remote_dmahandle - Remote DMA handle descriptor
 */
struct fastrpc_remote_dmahandle {
	/** @fd: DMA-BUF file descriptor */
	s32 fd;
	/** @offset: Byte offset within the DMA-BUF */
	u32 offset;
	/** @len: Length of the region in bytes */
	u32 len;
};

/**
 * struct fastrpc_remote_buf - Remote buffer descriptor
 */
struct fastrpc_remote_buf {
	/** @pv: Buffer pointer (user virtual address) */
	u64 pv;
	/** @len: Length of the buffer in bytes */
	u64 len;
};

/**
 * union fastrpc_remote_arg - Remote argument (buffer or DMA handle)
 */
union fastrpc_remote_arg {
	/** @buf: Inline buffer descriptor */
	struct fastrpc_remote_buf buf;
	/** @dma: DMA-BUF handle descriptor */
	struct fastrpc_remote_dmahandle dma;
};

/**
 * struct fastrpc_phy_page - Physical page descriptor
 */
struct fastrpc_phy_page {
	/** @addr: Physical (IOMMU) address of the page */
	u64 addr;
	/** @size: Size of the contiguous region in bytes */
	u64 size;
};

/**
 * struct fastrpc_invoke_buf - Invoke buffer descriptor
 */
struct fastrpc_invoke_buf {
	/** @num: Number of contiguous physical regions */
	u32 num;
	/** @pgidx: Index into the physical page array */
	u32 pgidx;
};

/**
 * struct fastrpc_create_process_inbuf - Input buffer for process creation
 *
 * This structure defines the input buffer format for creating a new
 * process on the remote DSP.
 */
struct fastrpc_create_process_inbuf {
	/** @remote_session_id: Client identifier for the session */
	int remote_session_id;
	/** @namelen: Length of the process name string including NUL terminator */
	u32 namelen;
	/** @filelen: Length of the ELF shell file in bytes */
	u32 filelen;
	/** @pageslen: Number of physical page descriptors */
	u32 pageslen;
	/** @attrs: Process attribute flags */
	u32 attrs;
	/** @siglen: Length of the signature data in bytes */
	u32 siglen;
};

/**
 * struct fastrpc_msg - FastRPC wire message for remote invocations
 *
 * Sent to the remote processor via RPMsg. This is the exact layout
 * the DSP expects; do not reorder or add fields without DSP firmware
 * coordination.
 */
struct fastrpc_msg {
	/** @remote_session_id: Session identifier on the remote processor */
	int remote_session_id;
	/** @tid: Thread ID of the invoking thread */
	int tid;
	/** @ctx: Context identifier for matching request/response */
	u64 ctx;
	/** @handle: Handle of the remote method to invoke */
	u32 handle;
	/** @sc: Scalars value encoding in/out buffer counts */
	u32 sc;
	/** @addr: Physical address of the message payload buffer */
	u64 addr;
	/** @size: Size of the message payload in bytes */
	u64 size;
};

/**
 * struct qda_msg - FastRPC message with kernel-internal bookkeeping
 */
struct qda_msg {
	/**
	 * @fastrpc: Wire-format message sent to the DSP via RPMsg.
	 * Must be the first member.
	 */
	struct fastrpc_msg fastrpc;
	/** @buf: Kernel virtual address of the payload buffer */
	void *buf;
	/** @phys: Physical/DMA address of the payload buffer */
	u64 phys;
	/** @ret: Return value from the remote processor */
	int ret;
	/** @fastrpc_ctx: Back-pointer to the owning invocation context */
	struct fastrpc_invoke_context *fastrpc_ctx;
	/** @file_priv: DRM file private data for GEM object lookup */
	struct drm_file *file_priv;
};

/**
 * struct fastrpc_invoke_context - Remote procedure call invocation context
 *
 * Maintains all state for a single remote procedure call, including buffer
 * management, synchronisation, and result handling.
 */
struct fastrpc_invoke_context {
	/** @node: List node for linking contexts in a queue */
	struct list_head node;
	/** @ctxid: Unique context identifier (XArray key shifted left by 4) */
	u64 ctxid;
	/** @inbufs: Number of input buffers */
	int inbufs;
	/** @outbufs: Number of output buffers */
	int outbufs;
	/** @handles: Number of DMA-BUF handle arguments */
	int handles;
	/** @nscalars: Total number of scalar arguments */
	int nscalars;
	/** @nbufs: Total number of buffer arguments (inbufs + outbufs) */
	int nbufs;
	/** @pid: Process ID of the calling process */
	int pid;
	/** @retval: Return value from the remote invocation */
	int retval;
	/** @metalen: Length of the FastRPC metadata header in bytes */
	int metalen;
	/** @remote_session_id: Session identifier on the remote processor */
	int remote_session_id;
	/** @pd: Protection domain identifier encoded into the context ID */
	int pd;
	/** @type: Invocation type (e.g. FASTRPC_RMID_INVOKE_DYNAMIC) */
	int type;
	/** @sc: Scalars value encoding in/out buffer counts */
	u32 sc;
	/** @handle: Handle of the remote method being invoked */
	u32 handle;
	/** @crc: Pointer to CRC values for data integrity checking */
	u32 *crc;
	/** @fdlist: Pointer to array of DMA-BUF file descriptors */
	u64 *fdlist;
	/** @pkt_size: Total payload size in bytes */
	u64 pkt_size;
	/** @aligned_pkt_size: Page-aligned payload size for GEM allocation */
	u64 aligned_pkt_size;
	/** @list: Array of invoke buffer descriptors */
	struct fastrpc_invoke_buf *list;
	/** @pages: Array of physical page descriptors for all arguments */
	struct fastrpc_phy_page *pages;
	/** @input_pages: Array of physical page descriptors for input buffers */
	struct fastrpc_phy_page *input_pages;
	/** @work: Completion used to synchronise with the DSP response */
	struct completion work;
	/** @msg: Pointer to the QDA message structure for this invocation */
	struct qda_msg *msg;
	/** @rpra: Array of remote procedure arguments */
	union fastrpc_remote_arg *rpra;
	/** @gem_objs: Array of GEM objects imported for argument buffers */
	struct drm_gem_object **gem_objs;
	/** @args: User-space invoke argument descriptors */
	struct drm_qda_fastrpc_invoke_args *args;
	/** @olaps: Array of buffer overlap descriptors for deduplication */
	struct fastrpc_buf_overlap *olaps;
	/** @refcount: Reference counter for context lifetime management */
	struct kref refcount;
	/** @msg_gem_obj: GEM object backing the message payload buffer */
	struct qda_gem_obj *msg_gem_obj;
	/** @file_priv: DRM file private data */
	struct drm_file *file_priv;
	/** @init_mem_gem_obj: GEM object for PD initialization memory */
	struct qda_gem_obj *init_mem_gem_obj;
	/** @req: Pointer to kernel-internal request buffer */
	void *req;
	/** @rsp: Pointer to kernel-internal response buffer */
	void *rsp;
	/** @inbuf: Pointer to kernel-internal input buffer */
	void *inbuf;
};

/* Remote Method ID table - identifies initialization and control operations */
#define FASTRPC_RMID_INIT_RELEASE	1	/* Release DSP process */
#define FASTRPC_RMID_INIT_MMAP		4	/* Map memory region to DSP */
#define FASTRPC_RMID_INIT_CREATE	6	/* Create DSP process */
#define FASTRPC_RMID_INIT_CREATE_ATTR	7	/* Create DSP process with attributes */
#define FASTRPC_RMID_INIT_MEM_MAP	10	/* Map DMA buffer with attributes to DSP */
#define FASTRPC_RMID_INVOKE_DYNAMIC	0xFFFFFFFF	/* Dynamic method invocation */

/* Common handle for initialization operations */
#define FASTRPC_INIT_HANDLE		0x1

/* Protection Domain (PD) identifiers */
#define QDA_ROOT_PD		(0)
#define QDA_USER_PD		(1)

/* Number of arguments for process creation */
#define FASTRPC_CREATE_PROCESS_NARGS	6
/* Maximum initialization file size (4 MB) */
#define FASTRPC_INIT_FILELEN_MAX	(4 * 1024 * 1024)

/* Message structures for internal FastRPC calls */

/**
 * struct fastrpc_mem_map_req_msg - Memory map request message with attributes
 *
 * This message structure is sent to the DSP to request mapping
 * of a DMA buffer with custom attributes (ATTR request).
 */
struct fastrpc_mem_map_req_msg {
	/** @remote_session_id: Client identifier for the session */
	s32 remote_session_id;
	/** @fd: DMA-BUF file descriptor of the buffer to map */
	s32 fd;
	/** @offset: Byte offset within the buffer */
	s32 offset;
	/** @flags: Mapping flags (cache attributes, permissions) */
	u32 flags;
	/** @vaddrin: Virtual address hint for the DSP mapping */
	u64 vaddrin;
	/** @num: Size of the physical page descriptor array in bytes */
	s32 num;
	/** @data_len: Length of additional inline data */
	s32 data_len;
};

/**
 * struct fastrpc_map_req_msg - Legacy memory map request message
 *
 * This message structure is sent to the DSP to request mapping
 * of a DMA buffer into the DSP's virtual address space.
 */
struct fastrpc_map_req_msg {
	/** @remote_session_id: Client identifier for the session */
	s32 remote_session_id;
	/** @flags: Mapping flags (cache attributes, permissions) */
	u32 flags;
	/** @vaddr: Virtual address hint for the DSP mapping */
	u64 vaddr;
	/** @num: Size of the physical page descriptor array in bytes */
	s32 num;
};

/**
 * struct fastrpc_map_rsp_msg - Memory map response message
 *
 * This message structure is returned by the DSP after successfully
 * mapping a buffer, providing the virtual address for future access.
 */
struct fastrpc_map_rsp_msg {
	/** @vaddrout: DSP virtual address assigned to the mapped buffer */
	u64 vaddrout;
};

void qda_fastrpc_context_free(struct kref *ref);
struct fastrpc_invoke_context *qda_fastrpc_context_alloc(void);
int qda_fastrpc_prepare_args(struct fastrpc_invoke_context *ctx, char __user *argp);
int qda_fastrpc_get_header_size(struct fastrpc_invoke_context *ctx, size_t *out_size);
int qda_fastrpc_invoke_pack(struct fastrpc_invoke_context *ctx, struct qda_msg *msg);
int qda_fastrpc_invoke_unpack(struct fastrpc_invoke_context *ctx, struct qda_msg *msg);
int qda_fastrpc_return_result(struct fastrpc_invoke_context *ctx, char __user *argp);

#endif /* __QDA_FASTRPC_H__ */
