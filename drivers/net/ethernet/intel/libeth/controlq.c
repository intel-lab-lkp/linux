// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2025 Intel Corporation */

#include <linux/bitfield.h>

#include <net/libeth/controlq.h>
#include <net/libeth/rx.h>

#define LIBETH_CTLQ_DESC_QWORD0(sz)			\
	(LIBETH_CTLQ_DESC_FLAG_BUF |			\
	 LIBETH_CTLQ_DESC_FLAG_RD |			\
	 FIELD_PREP(LIBETH_CTLQ_DESC_DATA_LEN, sz))

/**
 * libeth_ctlq_free_fq - free fill queue resources, including buffers
 * @ctlq: Rx control queue whose resources need to be freed
 */
static void libeth_ctlq_free_fq(struct libeth_ctlq_info *ctlq)
{
	struct libeth_fq fq = {
		.fqes		= ctlq->rx_fqes,
		.pp		= ctlq->pp,
	};

	for (u32 ntc = ctlq->next_to_clean; ntc != ctlq->next_to_post; ) {
		page_pool_put_full_page(fq.fqes[ntc].page->pp,
					fq.fqes[ntc].page, false);

		if (++ntc >= ctlq->ring_len)
			ntc = 0;
	}

	libeth_rx_fq_destroy(&fq);
}

/**
 * libeth_ctlq_init_fq - initialize fill queue for an Rx controlq
 * @ctlq: control queue that needs a Rx buffer allocation
 *
 * Return: %0 on success, -%errno on failure
 */
static int libeth_ctlq_init_fq(struct libeth_ctlq_info *ctlq)
{
	struct libeth_fq fq = {
		.count		= ctlq->ring_len,
		.truesize	= LIBETH_CTLQ_MAX_BUF_LEN,
		.nid		= NUMA_NO_NODE,
		.type		= LIBETH_FQE_SHORT,
		.hsplit		= true,
		.no_napi	= true,
	};
	int err;

	err = libeth_rx_fq_create(&fq, ctlq->dev);
	if (err)
		return err;

	ctlq->pp = fq.pp;
	ctlq->rx_fqes = fq.fqes;
	ctlq->truesize = fq.truesize;

	return 0;
}

/**
 * libeth_ctlq_reset_rx_desc - reset the descriptor with a new address
 * @desc: descriptor to (re)initialize
 * @addr: physical address to put into descriptor
 * @mem_truesize: size of the accessible memory
 */
static void libeth_ctlq_reset_rx_desc(struct libeth_ctlq_desc *desc,
				      dma_addr_t addr, u32 mem_truesize)
{
	u64 qword;

	*desc = (struct libeth_ctlq_desc) {};
	qword = LIBETH_CTLQ_DESC_QWORD0(mem_truesize);
	desc->qword0 = cpu_to_le64(qword);

	qword = FIELD_PREP(LIBETH_CTLQ_DESC_DATA_ADDR_HIGH,
			   upper_32_bits(addr)) |
		FIELD_PREP(LIBETH_CTLQ_DESC_DATA_ADDR_LOW,
			   lower_32_bits(addr));
	desc->qword3 = cpu_to_le64(qword);
}

/**
 * libeth_ctlq_post_rx_buffs - post buffers to descriptor ring
 * @ctlq: control queue that requires Rx descriptor ring to be initialized with
 *	  new Rx buffers
 *
 * The caller must make sure that calls to libeth_ctlq_post_rx_buffs()
 * and libeth_ctlq_recv() for separate queues are either serialized
 * or used under ctlq->lock.
 *
 * Return: %0 on success, -%ENOMEM if any buffer could not be allocated
 */
int libeth_ctlq_post_rx_buffs(struct libeth_ctlq_info *ctlq)
{
	u32 ntp = ctlq->next_to_post, ntc = ctlq->next_to_clean, num_to_post;
	const struct libeth_fq_fp fq = {
		.pp		= ctlq->pp,
		.fqes		= ctlq->rx_fqes,
		.truesize	= ctlq->truesize,
		.count		= ctlq->ring_len,
	};
	int ret = 0;

	num_to_post = (ntc > ntp ? 0 : ctlq->ring_len) + ntc - ntp - 1;

	while (num_to_post--) {
		dma_addr_t addr;

		addr = libeth_rx_alloc(&fq, ntp);
		if (unlikely(addr == DMA_MAPPING_ERROR)) {
			ret = -ENOMEM;
			goto post_bufs;
		}

		libeth_ctlq_reset_rx_desc(&ctlq->descs[ntp], addr, fq.truesize);

		if (unlikely(++ntp == ctlq->ring_len))
			ntp = 0;
	}

post_bufs:
	if (likely(ctlq->next_to_post != ntp)) {
		ctlq->next_to_post = ntp;

		writel(ntp, ctlq->reg.tail);
	}

	return ret;
}
EXPORT_SYMBOL_NS_GPL(libeth_ctlq_post_rx_buffs, "LIBETH_CP");

/**
 * libeth_ctlq_free_tx_msgs - Free Tx control queue messages
 * @ctlq: Tx control queue being destroyed
 * @num_msgs: number of messages allocated so far
 */
static void libeth_ctlq_free_tx_msgs(struct libeth_ctlq_info *ctlq,
				     u32 num_msgs)
{
	for (u32 i = 0; i < num_msgs; i++)
		kfree(ctlq->tx_msg[i]);

	kvfree(ctlq->tx_msg);
}

/**
 * libeth_ctlq_alloc_tx_msgs - Allocate Tx control queue messages
 * @ctlq: Tx control queue being created
 *
 * Return: %0 on success, -%ENOMEM on allocation error
 */
static int libeth_ctlq_alloc_tx_msgs(struct libeth_ctlq_info *ctlq)
{
	ctlq->tx_msg = kvcalloc(ctlq->ring_len,
				sizeof(*ctlq->tx_msg), GFP_KERNEL);
	if (!ctlq->tx_msg)
		return -ENOMEM;

	for (u32 i = 0; i < ctlq->ring_len; i++) {
		ctlq->tx_msg[i] = kzalloc(sizeof(*ctlq->tx_msg[i]), GFP_KERNEL);

		if (!ctlq->tx_msg[i]) {
			libeth_ctlq_free_tx_msgs(ctlq, i);
			return -ENOMEM;
		}
	}

	return 0;
}

/**
 * libeth_cp_free_dma_mem - Free the previously allocated DMA memory
 * @dev: device information
 * @mem: DMA memory information
 */
static void libeth_cp_free_dma_mem(struct device *dev,
				   struct libeth_cp_dma_mem *mem)
{
	dma_free_coherent(dev, mem->size, mem->va, mem->pa);
	mem->va = NULL;
}

/**
 * libeth_ctlq_dealloc_ring_res - Free memory allocated for control queue
 * @ctlq: control queue that requires its ring memory to be freed
 *
 * Free the memory used by the ring, buffers and other related structures.
 */
static void libeth_ctlq_dealloc_ring_res(struct libeth_ctlq_info *ctlq)
{
	struct libeth_cp_dma_mem *dma = &ctlq->ring_mem;

	if (ctlq->type == LIBETH_CTLQ_TYPE_TX)
		libeth_ctlq_free_tx_msgs(ctlq, ctlq->ring_len);
	else
		libeth_ctlq_free_fq(ctlq);

	libeth_cp_free_dma_mem(ctlq->dev, dma);
}

/**
 * libeth_cp_alloc_dma_mem - Allocate a DMA memory
 * @dev: device information
 * @mem: memory for DMA information to be stored
 * @size: size of the memory to allocate
 *
 * Return: virtual address of DMA memory or NULL.
 */
static void *libeth_cp_alloc_dma_mem(struct device *dev,
				     struct libeth_cp_dma_mem *mem, u32 size)
{
	size = ALIGN(size, SZ_4K);

	mem->va = dma_alloc_coherent(dev, size, &mem->pa, GFP_KERNEL);
	mem->size = size;

	return mem->va;
}

/**
 * libeth_ctlq_alloc_queue_res - allocate memory for descriptor ring and bufs
 * @ctlq: control queue that requires its ring resources to be allocated
 *
 * Return: %0 on success, -%errno on failure
 */
static int libeth_ctlq_alloc_queue_res(struct libeth_ctlq_info *ctlq)
{
	size_t size = array_size(ctlq->ring_len, sizeof(*ctlq->descs));
	struct libeth_cp_dma_mem *dma = &ctlq->ring_mem;
	int err = -ENOMEM;

	if (!libeth_cp_alloc_dma_mem(ctlq->dev, dma, size))
		return -ENOMEM;

	ctlq->descs = dma->va;

	if (ctlq->type == LIBETH_CTLQ_TYPE_TX) {
		if (libeth_ctlq_alloc_tx_msgs(ctlq))
			goto free_dma_mem;
	} else {
		err = libeth_ctlq_init_fq(ctlq);
		if (err)
			goto free_dma_mem;

		err = libeth_ctlq_post_rx_buffs(ctlq);
		if (err) {
			libeth_ctlq_free_fq(ctlq);
			goto free_dma_mem;
		}
	}

	return 0;

free_dma_mem:
	libeth_cp_free_dma_mem(ctlq->dev, dma);

	return err;
}

/**
 * libeth_ctlq_init_regs - Initialize control queue registers
 * @ctlq: control queue that needs to be initialized
 *
 * Initialize registers. The caller is expected to have already initialized the
 * descriptor ring memory and buffer memory.
 */
static void libeth_ctlq_init_regs(struct libeth_ctlq_info *ctlq)
{
	u32 dword;

	if (ctlq->type == VIRTCHNL2_QUEUE_TYPE_RX)
		writel(ctlq->ring_len - 1, ctlq->reg.tail);

	writel(0, ctlq->reg.head);
	writel(lower_32_bits(ctlq->ring_mem.pa), ctlq->reg.addr_low);
	writel(upper_32_bits(ctlq->ring_mem.pa), ctlq->reg.addr_high);

	dword = FIELD_PREP(LIBETH_CTLQ_MBX_ATQ_LEN, ctlq->ring_len) |
		ctlq->reg.len_ena_mask;
	writel(dword, ctlq->reg.len);
}

/**
 * libeth_find_ctlq - find the controlq for the given id and type
 * @ctx: controlq context structure
 * @type: type of controlq to find
 * @id: controlq id to find
 *
 * Return: control queue info pointer on success, NULL on failure
 */
struct libeth_ctlq_info *libeth_find_ctlq(struct libeth_ctlq_ctx *ctx,
					  enum virtchnl2_queue_type type,
					  int id)
{
	struct libeth_ctlq_info *cq;

	guard(spinlock)(&ctx->ctlqs_lock);

	list_for_each_entry(cq, &ctx->ctlqs, list)
		if (cq->qid == id && cq->type == type)
			return cq;

	return NULL;
}
EXPORT_SYMBOL_NS_GPL(libeth_find_ctlq, "LIBETH_CP");

/**
 * libeth_ctlq_add - add one control queue
 * @ctx: controlq context information
 * @qinfo: information that requires for queue creation
 *
 * Allocate and initialize a control queue and add it to the control queue list.
 * The ctlq parameter will be allocated/initialized and passed back to the
 * caller if no errors occur.
 *
 * Note: libeth_ctlq_init must be called prior to any calls to libeth_ctlq_add.
 *
 * Return: added control queue info pointer on success, error pointer on failure
 */
static struct libeth_ctlq_info *
libeth_ctlq_add(struct libeth_ctlq_ctx *ctx,
		const struct libeth_ctlq_create_info *qinfo)
{
	struct libeth_ctlq_info *ctlq;

	if (qinfo->id != LIBETH_CTLQ_MBX_ID)
		return ERR_PTR(-EOPNOTSUPP);

	/* libeth_ctlq_init was not called */
	scoped_guard(spinlock, &ctx->ctlqs_lock)
		if (!ctx->ctlqs.next)
			return ERR_PTR(-EINVAL);

	ctlq = kvzalloc(sizeof(*ctlq), GFP_KERNEL);
	if (!ctlq)
		return ERR_PTR(-ENOMEM);

	ctlq->type = qinfo->type;
	ctlq->qid = qinfo->id;
	ctlq->ring_len = qinfo->len;
	ctlq->dev = &ctx->mmio_info.pdev->dev;
	ctlq->reg = qinfo->reg;

	if (libeth_ctlq_alloc_queue_res(ctlq)) {
		kvfree(ctlq);
		return ERR_PTR(-ENOMEM);
	}

	libeth_ctlq_init_regs(ctlq);

	spin_lock_init(&ctlq->lock);

	scoped_guard(spinlock, &ctx->ctlqs_lock)
		list_add(&ctlq->list, &ctx->ctlqs);

	return ctlq;
}

/**
 * libeth_ctlq_remove - deallocate and remove specified control queue
 * @ctx: libeth context information
 * @ctlq: specific control queue that needs to be removed
 */
static void libeth_ctlq_remove(struct libeth_ctlq_ctx *ctx,
			       struct libeth_ctlq_info *ctlq)
{
	scoped_guard(spinlock, &ctx->ctlqs_lock)
		list_del(&ctlq->list);

	libeth_ctlq_dealloc_ring_res(ctlq);
	kvfree(ctlq);
}

/**
 * libeth_ctlq_init - main initialization routine for all control queues
 * @ctx: libeth context information
 * @qinfo: array of structs containing info for each queue to be initialized
 * @numq: number of queues to initialize
 *
 * This initializes queue list and adds any number and any type of control
 * queues. This is an all or nothing routine; if one fails, all previously
 * allocated queues will be destroyed. This must be called prior to using
 * the individual add/remove APIs.
 *
 * Return: %0 on success, -%errno on failure
 */
int libeth_ctlq_init(struct libeth_ctlq_ctx *ctx,
		     const struct libeth_ctlq_create_info *qinfo,
		     u32 numq)
{
	INIT_LIST_HEAD(&ctx->ctlqs);
	spin_lock_init(&ctx->ctlqs_lock);

	for (u32 i = 0; i < numq; i++) {
		struct libeth_ctlq_info *ctlq;

		ctlq = libeth_ctlq_add(ctx, &qinfo[i]);
		if (IS_ERR(ctlq)) {
			libeth_ctlq_deinit(ctx);
			return PTR_ERR(ctlq);
		}
	}

	return 0;
}
EXPORT_SYMBOL_NS_GPL(libeth_ctlq_init, "LIBETH_CP");

/**
 * libeth_ctlq_deinit - destroy all control queues
 * @ctx: libeth context information
 */
void libeth_ctlq_deinit(struct libeth_ctlq_ctx *ctx)
{
	struct libeth_ctlq_info *ctlq, *tmp;

	list_for_each_entry_safe(ctlq, tmp, &ctx->ctlqs, list)
		libeth_ctlq_remove(ctx, ctlq);
}
EXPORT_SYMBOL_NS_GPL(libeth_ctlq_deinit, "LIBETH_CP");

/**
 * libeth_ctlq_tx_desc_from_msg - initialize a Tx descriptor from a message
 * @desc: descriptor to be initialized
 * @msg: filled control queue message
 */
static void libeth_ctlq_tx_desc_from_msg(struct libeth_ctlq_desc *desc,
					 const struct libeth_ctlq_msg *msg)
{
	const struct libeth_cp_dma_mem *dma = &msg->send_mem;
	u64 qword;

	qword = FIELD_PREP(LIBETH_CTLQ_DESC_FLAGS, msg->flags) |
		FIELD_PREP(LIBETH_CTLQ_DESC_INFRA_OPCODE, msg->opcode) |
		FIELD_PREP(LIBETH_CTLQ_DESC_PFID_VFID, msg->func_id);
	desc->qword0 = cpu_to_le64(qword);

	qword = FIELD_PREP(LIBETH_CTLQ_DESC_VIRTCHNL_OPCODE,
			   msg->chnl_opcode) |
		FIELD_PREP(LIBETH_CTLQ_DESC_VIRTCHNL_MSG_RET_VAL,
			   msg->chnl_retval);
	desc->qword1 = cpu_to_le64(qword);

	qword = FIELD_PREP(LIBETH_CTLQ_DESC_MSG_PARAM0, msg->param0) |
		FIELD_PREP(LIBETH_CTLQ_DESC_SW_COOKIE,
			   msg->sw_cookie) |
		FIELD_PREP(LIBETH_CTLQ_DESC_VIRTCHNL_FLAGS,
			   msg->virt_flags);
	desc->qword2 = cpu_to_le64(qword);

	if (likely(msg->data_len)) {
		desc->qword0 |=
			cpu_to_le64(LIBETH_CTLQ_DESC_QWORD0(msg->data_len));
		qword = FIELD_PREP(LIBETH_CTLQ_DESC_DATA_ADDR_HIGH,
				   upper_32_bits(dma->pa)) |
			FIELD_PREP(LIBETH_CTLQ_DESC_DATA_ADDR_LOW,
				   lower_32_bits(dma->pa));
	} else {
		qword = msg->addr_param;
	}

	desc->qword3 = cpu_to_le64(qword);
}

/**
 * libeth_ctlq_send - send a message to Control Plane or Peer
 * @ctlq: specific control queue which is used for sending a message
 * @q_msg: array of queue messages to be sent
 * @num_q_msg: number of messages to send on control queue
 *
 * The control queue will hold a reference to each send message until
 * the completion for that message has been cleaned.
 *
 * The caller must hold ctlq->lock.
 *
 * Return: %0 on success, -%errno on failure.
 */
int libeth_ctlq_send(struct libeth_ctlq_info *ctlq,
		     struct libeth_ctlq_msg *q_msg, u32 num_q_msg)
{
	u32 num_desc_avail, ntu;

	ntu = ctlq->next_to_use;

	num_desc_avail = (ctlq->next_to_clean > ntu ? 0 : ctlq->ring_len) +
			  ctlq->next_to_clean - ntu - 1;

	if (num_desc_avail < num_q_msg)
		return -EBUSY;

	for (int i = 0; i < num_q_msg; i++) {
		struct libeth_ctlq_msg *msg = &q_msg[i];
		struct libeth_ctlq_desc *desc;

		desc = &ctlq->descs[ntu];
		libeth_ctlq_tx_desc_from_msg(desc, msg);

		if (unlikely(++ntu == ctlq->ring_len))
			ntu = 0;
	}
	writel(ntu, ctlq->reg.tail);
	ctlq->next_to_use = ntu;

	return 0;
}
EXPORT_SYMBOL_NS_GPL(libeth_ctlq_send, "LIBETH_CP");

/**
 * libeth_ctlq_fill_rx_msg - fill in a message from Rx descriptor and buffer
 * @msg: message to be filled in
 * @desc: received descriptor
 * @rx_buf: fill queue buffer associated with the descriptor
 */
static void libeth_ctlq_fill_rx_msg(struct libeth_ctlq_msg *msg,
				    const struct libeth_ctlq_desc *desc,
				    struct libeth_fqe *rx_buf)
{
	u64 qword = le64_to_cpu(desc->qword0);

	msg->flags = FIELD_GET(LIBETH_CTLQ_DESC_FLAGS, qword);
	msg->opcode = FIELD_GET(LIBETH_CTLQ_DESC_INFRA_OPCODE, qword);
	msg->data_len = FIELD_GET(LIBETH_CTLQ_DESC_DATA_LEN, qword);
	msg->hw_retval = FIELD_GET(LIBETH_CTLQ_DESC_HW_RETVAL, qword);

	qword = le64_to_cpu(desc->qword1);
	msg->chnl_opcode =
		FIELD_GET(LIBETH_CTLQ_DESC_VIRTCHNL_OPCODE, qword);
	msg->chnl_retval =
		FIELD_GET(LIBETH_CTLQ_DESC_VIRTCHNL_MSG_RET_VAL, qword);

	qword = le64_to_cpu(desc->qword2);
	msg->param0 =
		FIELD_GET(LIBETH_CTLQ_DESC_MSG_PARAM0, qword);
	msg->sw_cookie =
		FIELD_GET(LIBETH_CTLQ_DESC_SW_COOKIE, qword);
	msg->virt_flags =
		FIELD_GET(LIBETH_CTLQ_DESC_VIRTCHNL_FLAGS, qword);

	if (likely(msg->data_len)) {
		msg->recv_mem = (struct kvec) {
			.iov_base = page_address(rx_buf->page),
			.iov_len = msg->data_len,
		};
		libeth_rx_sync_for_cpu(rx_buf, msg->data_len);
	} else {
		msg->recv_mem = (struct kvec) {};
		msg->addr_param = le64_to_cpu(desc->qword3);
		page_pool_put_full_page(rx_buf->page->pp, rx_buf->page,
					false);
	}
}

/**
 * libeth_ctlq_recv - receive control queue message call back
 * @ctlq: control queue that needs to processed for receive
 * @msg: array of received control queue messages on this q;
 * needs to be pre-allocated by caller for as many messages as requested
 * @num_q_msg: number of messages that can be stored in msg buffer
 *
 * Called by interrupt handler or polling mechanism. Caller is expected
 * to free buffers.
 *
 * The caller must make sure that calls to libeth_ctlq_post_rx_buffs()
 * and libeth_ctlq_recv() for separate queues are either serialized
 * or used under ctlq->lock.
 *
 * Return: number of messages received
 */
u32 libeth_ctlq_recv(struct libeth_ctlq_info *ctlq, struct libeth_ctlq_msg *msg,
		     u32 num_q_msg)
{
	u32 ntc, i;

	ntc = ctlq->next_to_clean;

	for (i = 0; i < num_q_msg; i++) {
		const struct libeth_ctlq_desc *desc = &ctlq->descs[ntc];
		struct libeth_fqe *rx_buf = &ctlq->rx_fqes[ntc];
		u64 qword;

		qword = le64_to_cpu(desc->qword0);
		if (!FIELD_GET(LIBETH_CTLQ_DESC_FLAG_DD, qword))
			break;

		dma_rmb();

		if (unlikely(FIELD_GET(LIBETH_CTLQ_DESC_FLAG_ERR, qword)))
			break;

		libeth_ctlq_fill_rx_msg(&msg[i], desc, rx_buf);

		if (unlikely(++ntc == ctlq->ring_len))
			ntc = 0;
	}

	ctlq->next_to_clean = ntc;

	return i;
}
EXPORT_SYMBOL_NS_GPL(libeth_ctlq_recv, "LIBETH_CP");

/**
 * libeth_ctlq_xn_pop_free - get a free Xn entry from the free list
 * @xnm: Xn transaction manager
 *
 * Retrieve a free Xn entry from the free list.
 *
 * Return: valid Xn entry pointer or NULL if there are no free Xn entries.
 */
static struct libeth_ctlq_xn *
libeth_ctlq_xn_pop_free(struct libeth_ctlq_xn_manager *xnm)
{
	struct libeth_ctlq_xn *xn;
	u32 free_idx;

	guard(spinlock)(&xnm->free_xns_bm_lock);

	if (unlikely(xnm->shutdown))
		return NULL;

	free_idx = find_next_bit(xnm->free_xns_bm, LIBETH_CTLQ_MAX_XN_ENTRIES,
				 0);
	if (free_idx == LIBETH_CTLQ_MAX_XN_ENTRIES)
		return NULL;

	__clear_bit(free_idx, xnm->free_xns_bm);
	xn = &xnm->ring[free_idx];
	xn->cookie = xnm->cookie++;

	return xn;
}

/**
 * __libeth_ctlq_xn_push_free - unsafely push a Xn entry into the free list
 * @xnm: Xn transaction manager
 * @xn: xn entry to be added into the free list
 */
static void __libeth_ctlq_xn_push_free(struct libeth_ctlq_xn_manager *xnm,
				       struct libeth_ctlq_xn *xn)
{
	__set_bit(xn->index, xnm->free_xns_bm);

	if (likely(!xnm->shutdown))
		return;

	if (bitmap_full(xnm->free_xns_bm, LIBETH_CTLQ_MAX_XN_ENTRIES))
		complete(&xnm->can_destroy);
}

/**
 * libeth_ctlq_xn_push_free - push a Xn entry into the free list
 * @xnm: Xn transaction manager
 * @xn: xn entry to be added into the free list, not locked
 *
 * Safely add a used Xn entry back to the free list.
 */
static void libeth_ctlq_xn_push_free(struct libeth_ctlq_xn_manager *xnm,
				     struct libeth_ctlq_xn *xn)
{
	guard(spinlock)(&xnm->free_xns_bm_lock);

	__libeth_ctlq_xn_push_free(xnm, xn);
}

/**
 * libeth_ctlq_xn_put - put an Xn that will not be used in the current thread
 * @xnm: Xn transaction manager
 * @xn: async xn entry to be put for now, not locked
 *
 * If the Xn manager is being shutdown, nothing will handle the related
 * async request.
 */
static void libeth_ctlq_xn_put(struct libeth_ctlq_xn_manager *xnm,
			       struct libeth_ctlq_xn *xn)
{
	guard(spinlock)(&xnm->free_xns_bm_lock);

	if (unlikely(xnm->shutdown))
		__libeth_ctlq_xn_push_free(xnm, xn);
}

/**
 * libeth_ctlq_xn_deinit_dma - free the DMA memory allocated for send messages
 * @dev: device pointer
 * @xnm: pointer to the transaction manager
 * @num_entries: number of Xn entries to free the DMA for
 */
static void libeth_ctlq_xn_deinit_dma(struct device *dev,
				      struct libeth_ctlq_xn_manager *xnm,
				      u32 num_entries)
{
	for (u32 i = 0; i < num_entries; i++) {
		struct libeth_ctlq_xn *xn = &xnm->ring[i];

		libeth_cp_free_dma_mem(dev, xn->dma_mem);
		kfree(xn->dma_mem);
	}
}

/**
 * libeth_ctlq_xn_init_dma - pre-allocate DMA memory for send messages that use
 * stack variables
 * @dev: device pointer
 * @xnm: pointer to transaction manager
 *
 * Return: %0 on success or error if memory allocation fails
 */
static int libeth_ctlq_xn_init_dma(struct device *dev,
				   struct libeth_ctlq_xn_manager *xnm)
{
	u32 i;

	for (i = 0; i < LIBETH_CTLQ_MAX_XN_ENTRIES; i++) {
		struct libeth_ctlq_xn *xn = &xnm->ring[i];
		struct libeth_cp_dma_mem *dma_mem;

		dma_mem = kzalloc(sizeof(*dma_mem), GFP_KERNEL);
		if (!dma_mem)
			goto dealloc_dma;

		dma_mem->va = libeth_cp_alloc_dma_mem(dev, dma_mem,
						      LIBETH_CTLQ_MAX_BUF_LEN);
		if (!dma_mem->va) {
			kfree(dma_mem);
			goto dealloc_dma;
		}

		xn->dma_mem = dma_mem;
	}

	return 0;

dealloc_dma:
	libeth_ctlq_xn_deinit_dma(dev, xnm, i);

	return -ENOMEM;
}

/**
 * libeth_ctlq_xn_process_recv - process Xn data in receive message
 * @params: Xn receive param information to handle a receive message
 * @ctlq_msg: received control queue message
 *
 * Process a control queue receive message and send a complete event
 * notification.
 *
 * Return: true if a message has been processed, false otherwise.
 */
static bool
libeth_ctlq_xn_process_recv(struct libeth_ctlq_xn_recv_params *params,
			    struct libeth_ctlq_msg *ctlq_msg)
{
	struct libeth_ctlq_xn_manager *xnm = params->xnm;
	struct libeth_ctlq_xn *xn;
	u16 msg_cookie, xn_index;
	struct kvec *response;
	int status;
	u16 data;

	data = ctlq_msg->sw_cookie;
	xn_index = FIELD_GET(LIBETH_CTLQ_XN_INDEX_M, data);
	msg_cookie = FIELD_GET(LIBETH_CTLQ_XN_COOKIE_M, data);
	status = ctlq_msg->chnl_retval ? -EFAULT : 0;

	xn = &xnm->ring[xn_index];
	if (ctlq_msg->chnl_opcode != xn->virtchnl_opcode ||
	    msg_cookie != xn->cookie)
		return false;

	spin_lock(&xn->xn_lock);
	if (xn->state != LIBETH_CTLQ_XN_ASYNC &&
	    xn->state != LIBETH_CTLQ_XN_WAITING) {
		spin_unlock(&xn->xn_lock);
		return false;
	}

	response = &ctlq_msg->recv_mem;
	if (xn->state == LIBETH_CTLQ_XN_ASYNC) {
		xn->resp_cb(xn->send_ctx, response, status);
		xn->state = LIBETH_CTLQ_XN_IDLE;
		spin_unlock(&xn->xn_lock);
		libeth_ctlq_xn_push_free(xnm, xn);

		return true;
	}

	xn->recv_mem = *response;
	xn->state = status ? LIBETH_CTLQ_XN_COMPLETED_FAILED :
			     LIBETH_CTLQ_XN_COMPLETED_SUCCESS;

	complete(&xn->cmd_completion_event);
	spin_unlock(&xn->xn_lock);

	return true;
}

/**
 * libeth_xn_check_async_timeout - Check for asynchronous message timeouts
 * @xnm: Xn transaction manager
 *
 * Call the corresponding callback to notify the caller about the timeout.
 */
static void libeth_xn_check_async_timeout(struct libeth_ctlq_xn_manager *xnm)
{
	u32 idx;

	for_each_clear_bit(idx, xnm->free_xns_bm, LIBETH_CTLQ_MAX_XN_ENTRIES) {
		struct libeth_ctlq_xn *xn = &xnm->ring[idx];
		u64 timeout_ms;

		spin_lock(&xn->xn_lock);

		timeout_ms = ktime_ms_delta(ktime_get(), xn->timestamp);
		if (xn->state != LIBETH_CTLQ_XN_ASYNC ||
		    timeout_ms < xn->timeout_ms) {
			spin_unlock(&xn->xn_lock);
			continue;
		}

		xn->resp_cb(xn->send_ctx, NULL, -ETIMEDOUT);
		xn->state = LIBETH_CTLQ_XN_IDLE;
		spin_unlock(&xn->xn_lock);
		libeth_ctlq_xn_push_free(xnm, xn);
	}
}

/**
 * libeth_ctlq_xn_recv - process control queue receive message
 * @params: Xn receive param information to handle a receive message
 *
 * Process a receive message and update the receive queue buffer.
 *
 * Return: true if a message has been processed, false otherwise.
 */
bool libeth_ctlq_xn_recv(struct libeth_ctlq_xn_recv_params *params)
{
	struct libeth_ctlq_msg ctlq_msg;
	u32 num_msgs;

	num_msgs = libeth_ctlq_recv(params->ctlq, &ctlq_msg, 1);

	if (num_msgs && !libeth_ctlq_xn_process_recv(params, &ctlq_msg))
		params->ctlq_msg_handler(params->xnm->ctx, &ctlq_msg);

	libeth_ctlq_post_rx_buffs(params->ctlq);
	libeth_xn_check_async_timeout(params->xnm);

	return !!num_msgs;
}
EXPORT_SYMBOL_NS_GPL(libeth_ctlq_xn_recv, "LIBETH_CP");

/**
 * libeth_cp_map_dma_mem - map a given virtual address for DMA
 * @dev: device information
 * @va: virtual address to be mapped
 * @size: size of the memory
 * @direction: DMA direction either from/to device
 * @dma_mem: memory for DMA information to be stored
 *
 * Return: true on success, false on DMA map failure.
 */
static bool libeth_cp_map_dma_mem(struct device *dev, void *va, size_t size,
				  int direction,
				  struct libeth_cp_dma_mem *dma_mem)
{
	dma_mem->pa = dma_map_single(dev, va, size, direction);

	return dma_mapping_error(dev, dma_mem->pa) ? false : true;
}

/**
 * libeth_cp_unmap_dma_mem - unmap previously mapped DMA address
 * @dev: device information
 * @dma_mem: DMA memory information
 */
static void libeth_cp_unmap_dma_mem(struct device *dev,
				    const struct libeth_cp_dma_mem *dma_mem)
{
	dma_unmap_single(dev, dma_mem->pa, dma_mem->size,
			 dma_mem->direction);
}

/**
 * libeth_ctlq_xn_process_send - process and send a control queue message
 * @params: Xn send param information for sending a control queue message
 * @xn: Assigned Xn entry for tracking the control queue message
 *
 * Return: %0 on success, -%errno on failure.
 */
static
int libeth_ctlq_xn_process_send(struct libeth_ctlq_xn_send_params *params,
				struct libeth_ctlq_xn *xn)
{
	size_t buf_len = params->send_buf.iov_len;
	struct device *dev = params->ctlq->dev;
	void *buf = params->send_buf.iov_base;
	struct libeth_cp_dma_mem *dma_mem;
	u16 cookie;
	int ret;

	if (!buf || !buf_len)
		return -EOPNOTSUPP;

	if (libeth_cp_can_send_onstack(buf_len)) {
		dma_mem = xn->dma_mem;
		memcpy(dma_mem->va, buf, buf_len);
	} else {
		dma_mem = &xn->send_dma_mem;
		dma_mem->va = buf;
		dma_mem->size = buf_len;
		dma_mem->direction = DMA_TO_DEVICE;

		if (!libeth_cp_map_dma_mem(dev, buf, buf_len, DMA_TO_DEVICE,
					   dma_mem))
			return -ENOMEM;
	}

	cookie = FIELD_PREP(LIBETH_CTLQ_XN_COOKIE_M, xn->cookie) |
		 FIELD_PREP(LIBETH_CTLQ_XN_INDEX_M, xn->index);

	scoped_guard(spinlock, &params->ctlq->lock) {
		if (!params->ctlq_msg || params->resp_cb) {
			struct libeth_ctlq_info *ctlq = params->ctlq;

			*ctlq->tx_msg[ctlq->next_to_use] =
				params->ctlq_msg ? *params->ctlq_msg :
				(struct libeth_ctlq_msg) {
					.opcode = LIBETH_CTLQ_SEND_MSG_TO_CP
				};
			params->ctlq_msg = ctlq->tx_msg[ctlq->next_to_use];
		}

		params->ctlq_msg->sw_cookie = cookie;
		params->ctlq_msg->send_mem = *dma_mem;
		params->ctlq_msg->data_len = buf_len;
		params->ctlq_msg->chnl_opcode = params->chnl_opcode;
		ret = libeth_ctlq_send(params->ctlq, params->ctlq_msg, 1);
	}

	if (ret && !libeth_cp_can_send_onstack(buf_len))
		libeth_cp_unmap_dma_mem(dev, dma_mem);

	return ret;
}

/**
 * libeth_ctlq_xn_send - Function to send a control queue message
 * @params: Xn send param information for sending a control queue message
 *
 * Send a control queue (mailbox or config) message.
 * Based on the params value, the call can be completed synchronously or
 * asynchronously.
 *
 * Return: %0 on success, -%errno on failure.
 */
int libeth_ctlq_xn_send(struct libeth_ctlq_xn_send_params *params)
{
	bool free_send = !libeth_cp_can_send_onstack(params->send_buf.iov_len);
	struct libeth_ctlq_xn *xn;
	int ret;

	if (params->send_buf.iov_len > LIBETH_CTLQ_MAX_BUF_LEN) {
		ret = -EINVAL;
		goto free_buf;
	}

	xn = libeth_ctlq_xn_pop_free(params->xnm);
	/* no free transactions available */
	if (unlikely(!xn)) {
		ret = -EAGAIN;
		goto free_buf;
	}

	spin_lock(&xn->xn_lock);

	xn->state = params->resp_cb ? LIBETH_CTLQ_XN_ASYNC :
				      LIBETH_CTLQ_XN_WAITING;
	xn->ctlq = params->ctlq;
	xn->virtchnl_opcode = params->chnl_opcode;

	if (params->resp_cb) {
		xn->send_ctx = params->send_ctx;
		xn->resp_cb = params->resp_cb;
		xn->timeout_ms = params->timeout_ms;
		xn->timestamp = ktime_get();
	}

	ret = libeth_ctlq_xn_process_send(params, xn);
	if (ret)
		goto release_xn;
	else
		free_send = false;

	spin_unlock(&xn->xn_lock);

	if (params->resp_cb) {
		libeth_ctlq_xn_put(params->xnm, xn);
		return 0;
	}

	wait_for_completion_timeout(&xn->cmd_completion_event,
				    msecs_to_jiffies(params->timeout_ms));

	spin_lock(&xn->xn_lock);
	switch (xn->state) {
	case LIBETH_CTLQ_XN_WAITING:
		ret = -ETIMEDOUT;
		break;
	case LIBETH_CTLQ_XN_COMPLETED_SUCCESS:
		params->recv_mem = xn->recv_mem;
		break;
	default:
		ret = -EBADMSG;
		break;
	}

	/* Free the receive buffer in case of failure. On timeout, receive
	 * buffer is not allocated.
	 */
	if (ret && ret != -ETIMEDOUT)
		libeth_ctlq_release_rx_buf(xn->ctlq, &xn->recv_mem);

release_xn:
	xn->state = LIBETH_CTLQ_XN_IDLE;
	reinit_completion(&xn->cmd_completion_event);
	spin_unlock(&xn->xn_lock);
	libeth_ctlq_xn_push_free(params->xnm, xn);
free_buf:
	if (free_send)
		params->rel_tx_buf(params->send_buf.iov_base);

	return ret;
}
EXPORT_SYMBOL_NS_GPL(libeth_ctlq_xn_send, "LIBETH_CP");

/**
 * libeth_ctlq_xn_send_clean - cleanup the send control queue message buffers
 * @params: Xn clean param information for send complete handling
 *
 * Cleanup the send buffers for the given control queue, if force is set, then
 * clear all the outstanding send messages irrrespective their send status.
 * Force should be used during deinit or reset.
 *
 * Return: number of send buffers cleaned.
 */
u32 libeth_ctlq_xn_send_clean(const struct libeth_ctlq_xn_clean_params *params)
{
	struct libeth_ctlq_info *ctlq = params->ctlq;
	struct device *dev = ctlq->dev;
	u32 ntc, i;

	spin_lock(&ctlq->lock);
	ntc = ctlq->next_to_clean;

	for (i = 0; i < params->num_msgs; i++) {
		struct libeth_ctlq_msg *msg = ctlq->tx_msg[ntc];
		struct libeth_ctlq_desc *desc;
		u64 qword;

		desc = &ctlq->descs[ntc];
		qword = le64_to_cpu(desc->qword0);

		if (!FIELD_GET(LIBETH_CTLQ_DESC_FLAG_DD, qword))
			break;

		dma_rmb();

		if (!libeth_cp_can_send_onstack(msg->data_len)) {
			libeth_cp_unmap_dma_mem(dev, &msg->send_mem);
			params->rel_tx_buf(msg->send_mem.va);
		}

		memset(msg, 0, sizeof(*msg));
		desc->qword0 = 0;

		if (unlikely(++ntc == ctlq->ring_len))
			ntc = 0;
	}

	ctlq->next_to_clean = ntc;
	spin_unlock(&ctlq->lock);

	return i;
}
EXPORT_SYMBOL_NS_GPL(libeth_ctlq_xn_send_clean, "LIBETH_CP");

/**
 * libeth_ctlq_xn_deinit - deallocate and free the transaction manager resources
 * @xnm: pointer to the transaction manager
 * @ctx: controlq context structure
 *
 * All Rx processing must be stopped beforehand.
 */
void libeth_ctlq_xn_deinit(struct libeth_ctlq_xn_manager *xnm,
			   struct libeth_ctlq_ctx *ctx)
{
	bool must_wait = false;
	u32 i;

	/* Should be no new clear bits after this */
	spin_lock(&xnm->free_xns_bm_lock);
		xnm->shutdown = true;

	for_each_clear_bit(i, xnm->free_xns_bm, LIBETH_CTLQ_MAX_XN_ENTRIES) {
		struct libeth_ctlq_xn *xn = &xnm->ring[i];

		spin_lock(&xn->xn_lock);

		if (xn->state == LIBETH_CTLQ_XN_WAITING ||
		    xn->state == LIBETH_CTLQ_XN_IDLE) {
			complete(&xn->cmd_completion_event);
			must_wait = true;
		} else if (xn->state == LIBETH_CTLQ_XN_ASYNC) {
			__libeth_ctlq_xn_push_free(xnm, xn);
		}

		spin_unlock(&xn->xn_lock);
	}

	spin_unlock(&xnm->free_xns_bm_lock);

	if (must_wait)
		wait_for_completion(&xnm->can_destroy);

	libeth_ctlq_xn_deinit_dma(&ctx->mmio_info.pdev->dev, xnm,
				  LIBETH_CTLQ_MAX_XN_ENTRIES);
	kfree(xnm);
	libeth_ctlq_deinit(ctx);
}
EXPORT_SYMBOL_NS_GPL(libeth_ctlq_xn_deinit, "LIBETH_CP");

/**
 * libeth_ctlq_xn_init - initialize the Xn transaction manager
 * @params: Xn init param information for allocating Xn manager resources
 *
 * Return: %0 on success, -%errno on failure.
 */
int libeth_ctlq_xn_init(struct libeth_ctlq_xn_init_params *params)
{
	struct libeth_ctlq_xn_manager *xnm;
	int ret;

	ret = libeth_ctlq_init(params->ctx, params->cctlq_info, params->num_qs);
	if (ret)
		return ret;

	xnm = kzalloc(sizeof(*xnm), GFP_KERNEL);
	if (!xnm)
		goto ctlq_deinit;

	ret = libeth_ctlq_xn_init_dma(&params->ctx->mmio_info.pdev->dev, xnm);
	if (ret)
		goto free_xnm;

	spin_lock_init(&xnm->free_xns_bm_lock);
	init_completion(&xnm->can_destroy);
	bitmap_fill(xnm->free_xns_bm, LIBETH_CTLQ_MAX_XN_ENTRIES);

	for (u32 i = 0; i < LIBETH_CTLQ_MAX_XN_ENTRIES; i++) {
		struct libeth_ctlq_xn *xn = &xnm->ring[i];

		xn->index = i;
		init_completion(&xn->cmd_completion_event);
		spin_lock_init(&xn->xn_lock);
	}
	xnm->ctx = params->ctx;
	params->xnm = xnm;

	return 0;

free_xnm:
	kfree(xnm);
ctlq_deinit:
	libeth_ctlq_deinit(params->ctx);

	return -ENOMEM;
}
EXPORT_SYMBOL_NS_GPL(libeth_ctlq_xn_init, "LIBETH_CP");

MODULE_DESCRIPTION("Control Plane communication API");
MODULE_IMPORT_NS("LIBETH");
MODULE_LICENSE("GPL");
