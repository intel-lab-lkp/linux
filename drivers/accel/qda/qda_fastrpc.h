/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __QDA_FASTRPC_H__
#define __QDA_FASTRPC_H__

#include <linux/completion.h>
#include <linux/list.h>
#include <linux/types.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>

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
#define FASTRPC_BUILD_SCALARS(attr, method, in, out, oin, oout)  \
				(((attr & 0x07) << 29) |		\
				((method & 0x1f) << 24) |	\
				((in & 0xff) << 16) |		\
				((out & 0xff) <<  8) |		\
				((oin & 0x0f) <<  4) |		\
				(oout & 0x0f))

#define FASTRPC_SCALARS(method, in, out) \
		FASTRPC_BUILD_SCALARS(0, method, in, out, 0, 0)

/**
 * struct fastrpc_buf_overlap - Buffer overlap tracking structure
 *
 * This structure tracks overlapping buffer regions to optimize memory
 * mapping and avoid redundant mappings of the same physical memory.
 */
struct fastrpc_buf_overlap {
	/* Start address of the buffer in user virtual address space */
	u64 start;
	/* End address of the buffer in user virtual address space */
	u64 end;
	/* Remote argument index associated with this overlap */
	int raix;
	/* Start address of the mapped region */
	u64 mstart;
	/* End address of the mapped region */
	u64 mend;
	/* Offset within the mapped region */
	u64 offset;
};

/**
 * struct fastrpc_remote_dmahandle - Structure to represent a remote DMA handle
 */
struct fastrpc_remote_dmahandle {
	/* DMA handle file descriptor */
	s32 fd;
	/* DMA handle offset */
	u32 offset;
	/* DMA handle length */
	u32 len;
};

/**
 * struct fastrpc_remote_buf - Structure to represent a remote buffer
 */
struct fastrpc_remote_buf {
	/* Buffer pointer */
	u64 pv;
	/* Length of buffer */
	u64 len;
};

/**
 * union fastrpc_remote_arg - Union to represent remote arguments
 */
union fastrpc_remote_arg {
	/* Remote buffer */
	struct fastrpc_remote_buf buf;
	/* Remote DMA handle */
	struct fastrpc_remote_dmahandle dma;
};

/**
 * struct fastrpc_phy_page - Structure to represent a physical page
 */
struct fastrpc_phy_page {
	/* Physical address */
	u64 addr;
	/* Size of contiguous region */
	u64 size;
};

/**
 * struct fastrpc_invoke_buf - Structure to represent an invoke buffer
 */
struct fastrpc_invoke_buf {
	/* Number of contiguous regions */
	u32 num;
	/* Page index */
	u32 pgidx;
};

/**
 * struct fastrpc_create_process_inbuf - Input buffer for process creation
 *
 * This structure defines the input buffer format for creating a new
 * process on the remote DSP.
 */
struct fastrpc_create_process_inbuf {
	/* Client identifier for the session */
	int client_id;
	/* Length of the process name string */
	u32 namelen;
	/* Length of the shell file */
	u32 filelen;
	/* Length of the pages list */
	u32 pageslen;
	/* Process attributes flags */
	u32 attrs;
	/* Length of the signature data */
	u32 siglen;
};

/**
 * struct qda_msg - Message structure for FastRPC communication
 *
 * This structure represents a message sent to or received from the remote
 * processor via FastRPC protocol.
 */
struct qda_msg {
	/* Process client ID */
	int client_id;
	/* Thread ID */
	int tid;
	/* Context identifier for matching responses */
	u64 ctx;
	/* Handle to invoke on remote processor */
	u32 handle;
	/* Scalars structure describing the data layout */
	u32 sc;
	/* Physical address of the message buffer */
	u64 addr;
	/* Size of contiguous region */
	u64 size;
	/* Kernel virtual address of the buffer */
	void *buf;
	/* Physical/DMA address of the buffer */
	u64 phys;
	/* Return value from remote processor */
	int ret;
	/* Pointer to qda_dev for context management */
	struct qda_dev *qdev;
	/* Back-pointer to FastRPC context */
	struct fastrpc_invoke_context *fastrpc_ctx;
	/* File private data for GEM object lookup */
	struct drm_file *file_priv;
};

/**
 * struct fastrpc_invoke_context - Remote procedure call invocation context
 *
 * This structure maintains all state for a single remote procedure call,
 * including buffer management, synchronization, and result handling.
 */
struct fastrpc_invoke_context {
	/* Unique context identifier for this invocation */
	u64 ctxid;
	/* Number of input buffers */
	int inbufs;
	/* Number of output buffers */
	int outbufs;
	/* Number of file descriptor handles */
	int handles;
	/* Number of scalar parameters */
	int nscalars;
	/* Total number of buffers (input + output) */
	int nbufs;
	/* Process ID of the calling process */
	int pid;
	/* Return value from the remote invocation */
	int retval;
	/* Length of metadata */
	int metalen;
	/* Client identifier for this session */
	int client_id;
	/* Protection domain identifier */
	int pd;
	/* Type of invocation request */
	int type;
	/* Scalars parameter encoding buffer information */
	u32 sc;
	/* Handle to the remote method being invoked */
	u32 handle;
	/* Pointer to CRC values for data integrity */
	u32 *crc;
	/* Pointer to array of file descriptors */
	u64 *fdlist;
	/* Size of the packet */
	u64 pkt_size;
	/* Aligned packet size for DMA transfers */
	u64 aligned_pkt_size;
	/* Array of invoke buffer descriptors */
	struct fastrpc_invoke_buf *list;
	/* Array of physical page descriptors for buffers */
	struct fastrpc_phy_page *pages;
	/* Array of physical page descriptors for input buffers */
	struct fastrpc_phy_page *input_pages;
	/* List node for linking contexts in a queue */
	struct list_head node;
	/* Completion object for synchronizing invocation */
	struct completion work;
	/* Pointer to the QDA message structure */
	struct qda_msg *msg;
	/* Array of remote procedure arguments */
	union fastrpc_remote_arg *rpra;
	/* Array of GEM objects for argument buffers */
	struct drm_gem_object **gem_objs;
	/* Pointer to user-space invoke arguments */
	struct fastrpc_invoke_args *args;
	/* Array of buffer overlap descriptors */
	struct fastrpc_buf_overlap *olaps;
	/* Reference counter for context lifetime management */
	struct kref refcount;
	/* GEM object for the main message buffer */
	struct qda_gem_obj *msg_gem_obj;
	/* DRM file private data */
	struct drm_file *file_priv;
	/* GEM object for PD initialization memory */
	struct qda_gem_obj *init_mem_gem_obj;
	/* Pointer to request buffer */
	void *req;
	/* Pointer to response buffer */
	void *rsp;
	/* Pointer to input buffer */
	void *inbuf;
};

/* Remote Method ID table - identifies initialization and control operations */
#define FASTRPC_RMID_INIT_ATTACH	0	/* Attach to DSP session */
#define FASTRPC_RMID_INIT_RELEASE	1	/* Release DSP session */
#define FASTRPC_RMID_INIT_CREATE	6	/* Create DSP process */
#define FASTRPC_RMID_INIT_CREATE_ATTR	7	/* Create DSP process with attributes */
#define FASTRPC_RMID_INVOKE_DYNAMIC	0xFFFFFFFF	/* Dynamic method invocation */

/* Common handle for initialization operations */
#define FASTRPC_INIT_HANDLE		0x1

/* Protection Domain(PD) ids */
#define ROOT_PD		(0)
#define USER_PD		(1)

/* Number of arguments for process creation */
#define FASTRPC_CREATE_PROCESS_NARGS	6
/* Maximum initialization file size (4MB) */
#define INIT_FILELEN_MAX		(4 * 1024 * 1024)

/**
 * fastrpc_context_free - Free an invocation context
 * @ref: Reference counter for the context
 *
 * This function is called when the reference count reaches zero,
 * releasing all resources associated with the invocation context.
 */
void fastrpc_context_free(struct kref *ref);

/*
 * FastRPC context and invocation management functions
 */

/**
 * fastrpc_context_alloc - Allocate a new FastRPC invocation context
 *
 * Returns: Pointer to allocated context, or NULL on failure
 */
struct fastrpc_invoke_context *fastrpc_context_alloc(void);

/**
 * fastrpc_prepare_args - Prepare arguments for FastRPC invocation
 * @ctx: FastRPC invocation context
 * @argp: User-space pointer to invocation arguments
 *
 * Returns: 0 on success, negative error code on failure
 */
int fastrpc_prepare_args(struct fastrpc_invoke_context *ctx, char __user *argp);

/**
 * fastrpc_get_header_size - Get the size of the FastRPC message header
 * @ctx: FastRPC invocation context
 * @out_size: Pointer to store the header size in bytes
 *
 * Returns: 0 on success, negative error code on failure
 */
int fastrpc_get_header_size(struct fastrpc_invoke_context *ctx, size_t *out_size);

/**
 * fastrpc_internal_invoke_pack - Pack invocation context into message
 * @ctx: FastRPC invocation context
 * @msg: QDA message structure to pack into
 *
 * Returns: 0 on success, negative error code on failure
 */
int fastrpc_internal_invoke_pack(struct fastrpc_invoke_context *ctx, struct qda_msg *msg);

/**
 * fastrpc_internal_invoke_unpack - Unpack response message into context
 * @ctx: FastRPC invocation context
 * @msg: QDA message structure to unpack from
 *
 * Returns: 0 on success, negative error code on failure
 */
int fastrpc_internal_invoke_unpack(struct fastrpc_invoke_context *ctx, struct qda_msg *msg);

#endif /* __QDA_FASTRPC_H__ */
