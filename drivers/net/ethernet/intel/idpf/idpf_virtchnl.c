// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2023 Intel Corporation */

#include <net/libeth/rx.h>

#include "idpf.h"
#include "idpf_virtchnl.h"
#include "idpf_ptp.h"

/**
 * idpf_vid_to_vport - Translate vport id to vport pointer
 * @adapter: private data struct
 * @v_id: vport id to translate
 *
 * Returns vport matching v_id, NULL if not found.
 */
static
struct idpf_vport *idpf_vid_to_vport(struct idpf_adapter *adapter, u32 v_id)
{
	u16 num_max_vports = idpf_get_max_vports(adapter);
	int i;

	for (i = 0; i < num_max_vports; i++)
		if (adapter->vport_ids[i] == v_id)
			return adapter->vports[i];

	return NULL;
}

/**
 * idpf_handle_event_link - Handle link event message
 * @adapter: private data struct
 * @v2e: virtchnl event message
 */
static void idpf_handle_event_link(struct idpf_adapter *adapter,
				   const struct virtchnl2_event *v2e)
{
	struct idpf_netdev_priv *np;
	struct idpf_vport *vport;

	vport = idpf_vid_to_vport(adapter, le32_to_cpu(v2e->vport_id));
	if (!vport) {
		dev_err_ratelimited(&adapter->pdev->dev, "Failed to find vport_id %d for link event\n",
				    v2e->vport_id);
		return;
	}
	np = netdev_priv(vport->netdev);

	np->link_speed_mbps = le32_to_cpu(v2e->link_speed);

	if (vport->link_up == v2e->link_status)
		return;

	vport->link_up = v2e->link_status;

	if (np->state != __IDPF_VPORT_UP)
		return;

	if (vport->link_up) {
		netif_tx_start_all_queues(vport->netdev);
		netif_carrier_on(vport->netdev);
	} else {
		netif_tx_stop_all_queues(vport->netdev);
		netif_carrier_off(vport->netdev);
	}
}

/**
 * idpf_recv_event_msg - Receive virtchnl event message
 * @ctx: control queue context
 * @ctlq_msg: message to copy from
 *
 * Receive virtchnl event message
 */
void idpf_recv_event_msg(struct libie_ctlq_ctx *ctx,
			 struct libie_ctlq_msg *ctlq_msg)
{
	struct kvec *buff = &ctlq_msg->recv_mem;
	int payload_size = buff->iov_len;
	struct idpf_adapter *adapter;
	struct virtchnl2_event *v2e;
	u32 event;

	adapter = container_of(ctx, struct idpf_adapter, ctlq_ctx);
	if (payload_size < sizeof(*v2e)) {
		dev_err_ratelimited(&adapter->pdev->dev, "Failed to receive valid payload for event msg (op %d len %d)\n",
				    ctlq_msg->chnl_opcode,
				    payload_size);
		goto free_rx_buf;
	}

	v2e = (struct virtchnl2_event *)buff->iov_base;
	event = le32_to_cpu(v2e->event);

	switch (event) {
	case VIRTCHNL2_EVENT_LINK_CHANGE:
		idpf_handle_event_link(adapter, v2e);
		break;
	default:
		dev_err(&adapter->pdev->dev,
			"Unknown event %d from PF\n", event);
		break;
	}

free_rx_buf:
	libie_ctlq_release_rx_buf(buff);
}

/**
 * idpf_mb_clean - cleanup the send mailbox queue entries
 * @adapter: driver specific private structure
 * @asq: send control queue info
 *
 * This is a helper function to clean the send mailbox queue entries.
 */
static void idpf_mb_clean(struct idpf_adapter *adapter,
			  struct libie_ctlq_info *asq)
{
	struct libie_ctlq_xn_clean_params clean_params = {
		.ctx		= &adapter->ctlq_ctx,
		.ctlq		= asq,
		.rel_tx_buf	= kfree,
		.num_msgs	= IDPF_DFLT_MBX_Q_LEN,
	};

	libie_ctlq_xn_send_clean(&clean_params);
}

#if IS_ENABLED(CONFIG_PTP_1588_CLOCK)
/**
 * idpf_ptp_is_mb_msg - Check if the message is PTP-related
 * @op: virtchnl opcode
 *
 * Return: true if msg is PTP-related, false otherwise.
 */
static bool idpf_ptp_is_mb_msg(u32 op)
{
	switch (op) {
	case VIRTCHNL2_OP_PTP_GET_DEV_CLK_TIME:
	case VIRTCHNL2_OP_PTP_GET_CROSS_TIME:
	case VIRTCHNL2_OP_PTP_SET_DEV_CLK_TIME:
	case VIRTCHNL2_OP_PTP_ADJ_DEV_CLK_FINE:
	case VIRTCHNL2_OP_PTP_ADJ_DEV_CLK_TIME:
	case VIRTCHNL2_OP_PTP_GET_VPORT_TX_TSTAMP_CAPS:
	case VIRTCHNL2_OP_PTP_GET_VPORT_TX_TSTAMP:
		return true;
	default:
		return false;
	}
}

/**
 * idpf_prepare_ptp_mb_msg - Prepare PTP related message
 *
 * @adapter: Driver specific private structure
 * @op: virtchnl opcode
 * @ctlq_msg: Corresponding control queue message
 */
static void idpf_prepare_ptp_mb_msg(struct idpf_adapter *adapter, u32 op,
				    struct libie_ctlq_msg *ctlq_msg)
{
	/* If the message is PTP-related and the secondary mailbox is available,
	 * send the message through the secondary mailbox.
	 */
	if (!idpf_ptp_is_mb_msg(op) || !adapter->ptp->secondary_mbx.valid)
		return;

	ctlq_msg->opcode = LIBIE_CTLQ_SEND_MSG_TO_PEER;
	ctlq_msg->func_id = adapter->ptp->secondary_mbx.peer_mbx_q_id;
	ctlq_msg->flags = FIELD_PREP(LIBIE_CTLQ_DESC_FLAG_HOST_ID,
				     adapter->ptp->secondary_mbx.peer_id);
}
#else /* !CONFIG_PTP_1588_CLOCK */
static void idpf_prepare_ptp_mb_msg(struct idpf_adapter *adapter, u32 op,
				    struct libie_ctlq_msg *ctlq_msg)
{ }
#endif /* CONFIG_PTP_1588_CLOCK */

/**
 * idpf_send_mb_msg - send mailbox message to the device control plane
 * @adapter: driver specific private structure
 * @xn_params: Xn send parameters to fill
 * @send_buf: buffer to send
 * @send_buf_size: size of the send buffer
 *
 * Fill the Xn parameters with the required info to send a virtchnl message.
 * The send buffer is DMA mapped in the libie to avoid memcpy.
 *
 * Cleanup the mailbox queue entries of the previously sent message to
 * unmap and release the buffer.
 *
 * Return: 0 if sending was successful or reset in detected,
 *	   negative error code on failure.
 */
int idpf_send_mb_msg(struct idpf_adapter *adapter,
		     struct libie_ctlq_xn_send_params *xn_params,
		     void *send_buf, size_t send_buf_size)
{
	struct libie_ctlq_msg ctlq_msg = {};

	if (idpf_is_reset_detected(adapter)) {
		if (!libie_cp_can_send_onstack(send_buf_size))
			kfree(send_buf);

		return -EBUSY;
	}

	idpf_prepare_ptp_mb_msg(adapter, xn_params->chnl_opcode, &ctlq_msg);
	xn_params->ctlq_msg = ctlq_msg.opcode ? &ctlq_msg : NULL;

	xn_params->send_buf.iov_base = send_buf;
	xn_params->send_buf.iov_len = send_buf_size;
	xn_params->xnm = adapter->xn_init_params.xnm;
	xn_params->ctlq = xn_params->ctlq ? xn_params->ctlq : adapter->asq;
	xn_params->rel_tx_buf = kfree;

	idpf_mb_clean(adapter, xn_params->ctlq);

	return libie_ctlq_xn_send(xn_params);
}

/**
 * idpf_wait_for_marker_event - wait for software marker response
 * @vport: virtual port data structure
 *
 * Returns 0 success, negative on failure.
 **/
static int idpf_wait_for_marker_event(struct idpf_vport *vport)
{
	int event;
	int i;

	for (i = 0; i < vport->num_txq; i++)
		idpf_queue_set(SW_MARKER, vport->txqs[i]);

	event = wait_event_timeout(vport->sw_marker_wq,
				   test_and_clear_bit(IDPF_VPORT_SW_MARKER,
						      vport->flags),
				   msecs_to_jiffies(500));

	for (i = 0; i < vport->num_txq; i++)
		idpf_queue_clear(POLL_MODE, vport->txqs[i]);

	if (event)
		return 0;

	dev_warn(&vport->adapter->pdev->dev, "Failed to receive marker packets\n");

	return -ETIMEDOUT;
}

/**
 * idpf_send_ver_msg - send virtchnl version message
 * @adapter: Driver specific private structure
 *
 * Send virtchnl version message.  Returns 0 on success, negative on failure.
 */
static int idpf_send_ver_msg(struct idpf_adapter *adapter)
{
	struct libie_ctlq_xn_send_params xn_params = {
		.timeout_ms	= IDPF_VC_XN_DEFAULT_TIMEOUT_MSEC,
		.chnl_opcode	= VIRTCHNL2_OP_VERSION,
	};
	struct virtchnl2_version_info *vvi_recv;
	struct virtchnl2_version_info vvi;
	u32 major, minor;
	int err;

	if (adapter->virt_ver_maj) {
		vvi.major = cpu_to_le32(adapter->virt_ver_maj);
		vvi.minor = cpu_to_le32(adapter->virt_ver_min);
	} else {
		vvi.major = cpu_to_le32(IDPF_VIRTCHNL_VERSION_MAJOR);
		vvi.minor = cpu_to_le32(IDPF_VIRTCHNL_VERSION_MINOR);
	}

	err = idpf_send_mb_msg(adapter, &xn_params, &vvi, sizeof(vvi));
	if (err)
		return err;

	vvi_recv = xn_params.recv_mem.iov_base;
	major = le32_to_cpu(vvi_recv->major);
	minor = le32_to_cpu(vvi_recv->minor);

	if (major > IDPF_VIRTCHNL_VERSION_MAJOR) {
		dev_warn(&adapter->pdev->dev, "Virtchnl major version greater than supported\n");
		err = -EINVAL;
		goto free_rx_buf;
	}

	if (major == IDPF_VIRTCHNL_VERSION_MAJOR &&
	    minor > IDPF_VIRTCHNL_VERSION_MINOR)
		dev_warn(&adapter->pdev->dev, "Virtchnl minor version didn't match\n");

	/* If we have a mismatch, resend version to update receiver on what
	 * version we will use.
	 */
	if (!adapter->virt_ver_maj &&
	    major != IDPF_VIRTCHNL_VERSION_MAJOR &&
	    minor != IDPF_VIRTCHNL_VERSION_MINOR)
		err = -EAGAIN;

	adapter->virt_ver_maj = major;
	adapter->virt_ver_min = minor;

free_rx_buf:
	libie_ctlq_release_rx_buf(&xn_params.recv_mem);

	return err;
}

/**
 * idpf_send_get_caps_msg - Send virtchnl get capabilities message
 * @adapter: Driver specific private structure
 *
 * Send virtchl get capabilities message. Returns 0 on success, negative on
 * failure.
 */
static int idpf_send_get_caps_msg(struct idpf_adapter *adapter)
{
	struct libie_ctlq_xn_send_params xn_params = {
		.timeout_ms	= IDPF_VC_XN_DEFAULT_TIMEOUT_MSEC,
		.chnl_opcode	= VIRTCHNL2_OP_GET_CAPS,
	};
	struct virtchnl2_get_capabilities caps = {};
	int err;

	caps.csum_caps =
		cpu_to_le32(VIRTCHNL2_CAP_TX_CSUM_L3_IPV4	|
			    VIRTCHNL2_CAP_TX_CSUM_L4_IPV4_TCP	|
			    VIRTCHNL2_CAP_TX_CSUM_L4_IPV4_UDP	|
			    VIRTCHNL2_CAP_TX_CSUM_L4_IPV4_SCTP	|
			    VIRTCHNL2_CAP_TX_CSUM_L4_IPV6_TCP	|
			    VIRTCHNL2_CAP_TX_CSUM_L4_IPV6_UDP	|
			    VIRTCHNL2_CAP_TX_CSUM_L4_IPV6_SCTP	|
			    VIRTCHNL2_CAP_RX_CSUM_L3_IPV4	|
			    VIRTCHNL2_CAP_RX_CSUM_L4_IPV4_TCP	|
			    VIRTCHNL2_CAP_RX_CSUM_L4_IPV4_UDP	|
			    VIRTCHNL2_CAP_RX_CSUM_L4_IPV4_SCTP	|
			    VIRTCHNL2_CAP_RX_CSUM_L4_IPV6_TCP	|
			    VIRTCHNL2_CAP_RX_CSUM_L4_IPV6_UDP	|
			    VIRTCHNL2_CAP_RX_CSUM_L4_IPV6_SCTP	|
			    VIRTCHNL2_CAP_TX_CSUM_L3_SINGLE_TUNNEL |
			    VIRTCHNL2_CAP_RX_CSUM_L3_SINGLE_TUNNEL |
			    VIRTCHNL2_CAP_TX_CSUM_L4_SINGLE_TUNNEL |
			    VIRTCHNL2_CAP_RX_CSUM_L4_SINGLE_TUNNEL |
			    VIRTCHNL2_CAP_RX_CSUM_GENERIC);

	caps.seg_caps =
		cpu_to_le32(VIRTCHNL2_CAP_SEG_IPV4_TCP		|
			    VIRTCHNL2_CAP_SEG_IPV4_UDP		|
			    VIRTCHNL2_CAP_SEG_IPV4_SCTP		|
			    VIRTCHNL2_CAP_SEG_IPV6_TCP		|
			    VIRTCHNL2_CAP_SEG_IPV6_UDP		|
			    VIRTCHNL2_CAP_SEG_IPV6_SCTP		|
			    VIRTCHNL2_CAP_SEG_TX_SINGLE_TUNNEL);

	caps.rss_caps =
		cpu_to_le64(VIRTCHNL2_FLOW_IPV4_TCP		|
			    VIRTCHNL2_FLOW_IPV4_UDP		|
			    VIRTCHNL2_FLOW_IPV4_SCTP		|
			    VIRTCHNL2_FLOW_IPV4_OTHER		|
			    VIRTCHNL2_FLOW_IPV6_TCP		|
			    VIRTCHNL2_FLOW_IPV6_UDP		|
			    VIRTCHNL2_FLOW_IPV6_SCTP		|
			    VIRTCHNL2_FLOW_IPV6_OTHER);

	caps.hsplit_caps =
		cpu_to_le32(VIRTCHNL2_CAP_RX_HSPLIT_AT_L4V4	|
			    VIRTCHNL2_CAP_RX_HSPLIT_AT_L4V6);

	caps.rsc_caps =
		cpu_to_le32(VIRTCHNL2_CAP_RSC_IPV4_TCP		|
			    VIRTCHNL2_CAP_RSC_IPV6_TCP);

	caps.other_caps =
		cpu_to_le64(VIRTCHNL2_CAP_SRIOV			|
			    VIRTCHNL2_CAP_MACFILTER		|
			    VIRTCHNL2_CAP_SPLITQ_QSCHED		|
			    VIRTCHNL2_CAP_PROMISC		|
			    VIRTCHNL2_CAP_LOOPBACK		|
			    VIRTCHNL2_CAP_PTP);

	err = idpf_send_mb_msg(adapter, &xn_params, &caps, sizeof(caps));
	if (err)
		return err;

	memcpy(&adapter->caps, xn_params.recv_mem.iov_base,
	       sizeof(adapter->caps));

	libie_ctlq_release_rx_buf(&xn_params.recv_mem);

	return 0;
}

/**
 * idpf_add_del_fsteer_filters - Send virtchnl add/del Flow Steering message
 * @adapter: adapter info struct
 * @rule: Flow steering rule to add/delete
 * @opcode: VIRTCHNL2_OP_ADD_FLOW_RULE to add filter, or
 *          VIRTCHNL2_OP_DEL_FLOW_RULE to delete. All other values are invalid.
 *
 * Send ADD/DELETE flow steering virtchnl message and receive the result.
 *
 * Return: 0 on success, negative on failure.
 */
int idpf_add_del_fsteer_filters(struct idpf_adapter *adapter,
				struct virtchnl2_flow_rule_add_del *rule,
				enum virtchnl2_op opcode)
{
	int rule_count = le32_to_cpu(rule->count);
	struct libie_ctlq_xn_send_params xn_params = {
		.chnl_opcode = opcode,
		.timeout_ms = IDPF_VC_XN_DEFAULT_TIMEOUT_MSEC,
	};
	int ret;

	if (opcode != VIRTCHNL2_OP_ADD_FLOW_RULE &&
	    opcode != VIRTCHNL2_OP_DEL_FLOW_RULE) {
		kfree(rule);
		return -EINVAL;
	}

	ret = idpf_send_mb_msg(adapter, &xn_params, rule,
			       struct_size(rule, rule_info, rule_count));
	if (ret)
		return ret;

	libie_ctlq_release_rx_buf(&xn_params.recv_mem);
	return 0;
}

/**
 * idpf_vport_alloc_max_qs - Allocate max queues for a vport
 * @adapter: Driver specific private structure
 * @max_q: vport max queue structure
 */
int idpf_vport_alloc_max_qs(struct idpf_adapter *adapter,
			    struct idpf_vport_max_q *max_q)
{
	struct idpf_avail_queue_info *avail_queues = &adapter->avail_queues;
	struct virtchnl2_get_capabilities *caps = &adapter->caps;
	u16 default_vports = idpf_get_default_vports(adapter);
	int max_rx_q, max_tx_q;

	mutex_lock(&adapter->queue_lock);

	max_rx_q = le16_to_cpu(caps->max_rx_q) / default_vports;
	max_tx_q = le16_to_cpu(caps->max_tx_q) / default_vports;
	if (adapter->num_alloc_vports < default_vports) {
		max_q->max_rxq = min_t(u16, max_rx_q, IDPF_MAX_Q);
		max_q->max_txq = min_t(u16, max_tx_q, IDPF_MAX_Q);
	} else {
		max_q->max_rxq = IDPF_MIN_Q;
		max_q->max_txq = IDPF_MIN_Q;
	}
	max_q->max_bufq = max_q->max_rxq * IDPF_MAX_BUFQS_PER_RXQ_GRP;
	max_q->max_complq = max_q->max_txq;

	if (avail_queues->avail_rxq < max_q->max_rxq ||
	    avail_queues->avail_txq < max_q->max_txq ||
	    avail_queues->avail_bufq < max_q->max_bufq ||
	    avail_queues->avail_complq < max_q->max_complq) {
		mutex_unlock(&adapter->queue_lock);

		return -EINVAL;
	}

	avail_queues->avail_rxq -= max_q->max_rxq;
	avail_queues->avail_txq -= max_q->max_txq;
	avail_queues->avail_bufq -= max_q->max_bufq;
	avail_queues->avail_complq -= max_q->max_complq;

	mutex_unlock(&adapter->queue_lock);

	return 0;
}

/**
 * idpf_vport_dealloc_max_qs - Deallocate max queues of a vport
 * @adapter: Driver specific private structure
 * @max_q: vport max queue structure
 */
void idpf_vport_dealloc_max_qs(struct idpf_adapter *adapter,
			       struct idpf_vport_max_q *max_q)
{
	struct idpf_avail_queue_info *avail_queues;

	mutex_lock(&adapter->queue_lock);
	avail_queues = &adapter->avail_queues;

	avail_queues->avail_rxq += max_q->max_rxq;
	avail_queues->avail_txq += max_q->max_txq;
	avail_queues->avail_bufq += max_q->max_bufq;
	avail_queues->avail_complq += max_q->max_complq;

	mutex_unlock(&adapter->queue_lock);
}

/**
 * idpf_init_avail_queues - Initialize available queues on the device
 * @adapter: Driver specific private structure
 */
static void idpf_init_avail_queues(struct idpf_adapter *adapter)
{
	struct idpf_avail_queue_info *avail_queues = &adapter->avail_queues;
	struct virtchnl2_get_capabilities *caps = &adapter->caps;

	avail_queues->avail_rxq = le16_to_cpu(caps->max_rx_q);
	avail_queues->avail_txq = le16_to_cpu(caps->max_tx_q);
	avail_queues->avail_bufq = le16_to_cpu(caps->max_rx_bufq);
	avail_queues->avail_complq = le16_to_cpu(caps->max_tx_complq);
}

/**
 * idpf_convert_reg_to_queue_chunks - copy queue chunk information to the right
 * structure
 * @dchunks: destination chunks to store data to
 * @schunks: source chunks to copy data from
 * @num_chunks: number of chunks to copy
 */
static void idpf_convert_reg_to_queue_chunks(struct virtchnl2_queue_chunk *dchunks,
					     struct idpf_queue_id_reg_chunk *schunks,
					     u16 num_chunks)
{
	for (u16 i = 0; i < num_chunks; i++) {
		dchunks[i].type = cpu_to_le32(schunks[i].type);
		dchunks[i].start_queue_id = cpu_to_le32(schunks[i].start_queue_id);
		dchunks[i].num_queues = cpu_to_le32(schunks[i].num_queues);
	}
}

/**
 * idpf_vport_init_queue_reg_chunks - initialize queue register chunks
 * @vport_config: persistent vport structure to store the queue register info
 * @schunks: source chunks to copy data from
 *
 * Return: %0 on success, -%errno on failure.
 */
static int
idpf_vport_init_queue_reg_chunks(struct idpf_vport_config *vport_config,
				 struct virtchnl2_queue_reg_chunks *schunks)
{
	struct idpf_queue_id_reg_info *q_info = &vport_config->qid_reg_info;
	u16 num_chunks = le16_to_cpu(schunks->num_chunks);

	kfree(q_info->queue_chunks);

	q_info->num_chunks = num_chunks;
	q_info->queue_chunks = kcalloc(num_chunks, sizeof(*q_info->queue_chunks),
				       GFP_KERNEL);
	if (!q_info->queue_chunks)
		return -ENOMEM;

	for (u16 i = 0; i < num_chunks; i++) {
		struct idpf_queue_id_reg_chunk *dchunk = &q_info->queue_chunks[i];
		struct virtchnl2_queue_reg_chunk *schunk = &schunks->chunks[i];

		dchunk->qtail_reg_start = le64_to_cpu(schunk->qtail_reg_start);
		dchunk->qtail_reg_spacing = le32_to_cpu(schunk->qtail_reg_spacing);
		dchunk->type = le32_to_cpu(schunk->type);
		dchunk->start_queue_id = le32_to_cpu(schunk->start_queue_id);
		dchunk->num_queues = le32_to_cpu(schunk->num_queues);
	}

	return 0;
}

/**
 * idpf_get_reg_intr_vecs - Get vector queue register offset
 * @adapter: adapter structure to get the vector chunks
 * @reg_vals: Register offsets to store in
 *
 * Returns number of registers that got populated
 */
int idpf_get_reg_intr_vecs(struct idpf_adapter *adapter,
			   struct idpf_vec_regs *reg_vals)
{
	struct virtchnl2_vector_chunks *chunks;
	struct idpf_vec_regs reg_val;
	u16 num_vchunks, num_vec;
	int num_regs = 0, i, j;

	chunks = &adapter->req_vec_chunks->vchunks;
	num_vchunks = le16_to_cpu(chunks->num_vchunks);

	for (j = 0; j < num_vchunks; j++) {
		struct virtchnl2_vector_chunk *chunk;
		u32 dynctl_reg_spacing;
		u32 itrn_reg_spacing;

		chunk = &chunks->vchunks[j];
		num_vec = le16_to_cpu(chunk->num_vectors);
		reg_val.dyn_ctl_reg = le32_to_cpu(chunk->dynctl_reg_start);
		reg_val.itrn_reg = le32_to_cpu(chunk->itrn_reg_start);
		reg_val.itrn_index_spacing = le32_to_cpu(chunk->itrn_index_spacing);

		dynctl_reg_spacing = le32_to_cpu(chunk->dynctl_reg_spacing);
		itrn_reg_spacing = le32_to_cpu(chunk->itrn_reg_spacing);

		for (i = 0; i < num_vec; i++) {
			reg_vals[num_regs].dyn_ctl_reg = reg_val.dyn_ctl_reg;
			reg_vals[num_regs].itrn_reg = reg_val.itrn_reg;
			reg_vals[num_regs].itrn_index_spacing =
						reg_val.itrn_index_spacing;

			reg_val.dyn_ctl_reg += dynctl_reg_spacing;
			reg_val.itrn_reg += itrn_reg_spacing;
			num_regs++;
		}
	}

	return num_regs;
}

/**
 * idpf_vport_get_q_reg - Get the queue registers for the vport
 * @reg_vals: register values needing to be set
 * @num_regs: amount we expect to fill
 * @q_type: queue model
 * @chunks: queue regs received over mailbox
 *
 * This function parses the queue register offsets from the queue register
 * chunk information, with a specific queue type and stores it into the array
 * passed as an argument. It returns the actual number of queue registers that
 * are filled.
 */
static int idpf_vport_get_q_reg(u32 *reg_vals, int num_regs, u32 q_type,
				struct idpf_queue_id_reg_info *chunks)
{
	u16 num_chunks = chunks->num_chunks;
	int reg_filled = 0, i;
	u32 reg_val;

	while (num_chunks--) {
		struct idpf_queue_id_reg_chunk *chunk;
		u16 num_q;

		chunk = &chunks->queue_chunks[num_chunks];
		if (chunk->type != q_type)
			continue;

		num_q = chunk->num_queues;
		reg_val = chunk->qtail_reg_start;
		for (i = 0; i < num_q && reg_filled < num_regs ; i++) {
			reg_vals[reg_filled++] = reg_val;
			reg_val += chunk->qtail_reg_spacing;
		}
	}

	return reg_filled;
}

/**
 * __idpf_queue_reg_init - initialize queue registers
 * @vport: virtual port structure
 * @rsrc: pointer to queue and vector resources
 * @reg_vals: registers we are initializing
 * @num_regs: how many registers there are in total
 * @q_type: queue model
 *
 * Return number of queues that are initialized
 */
static int __idpf_queue_reg_init(struct idpf_vport *vport,
				 struct idpf_q_vec_rsrc *rsrc, u32 *reg_vals,
				 int num_regs, u32 q_type)
{
	struct libie_mmio_info *mmio = &vport->adapter->ctlq_ctx.mmio_info;
	int i, j, k = 0;

	switch (q_type) {
	case VIRTCHNL2_QUEUE_TYPE_TX:
		for (i = 0; i < rsrc->num_txq_grp; i++) {
			struct idpf_txq_group *tx_qgrp = &rsrc->txq_grps[i];

			for (j = 0; j < tx_qgrp->num_txq && k < num_regs; j++, k++)
				tx_qgrp->txqs[j]->tail =
					libie_pci_get_mmio_addr(mmio,
								reg_vals[k]);
		}
		break;
	case VIRTCHNL2_QUEUE_TYPE_RX:
		for (i = 0; i < rsrc->num_rxq_grp; i++) {
			struct idpf_rxq_group *rx_qgrp = &rsrc->rxq_grps[i];
			u16 num_rxq = rx_qgrp->singleq.num_rxq;

			for (j = 0; j < num_rxq && k < num_regs; j++, k++) {
				struct idpf_rx_queue *q;

				q = rx_qgrp->singleq.rxqs[j];
				q->tail = libie_pci_get_mmio_addr(mmio,
								  reg_vals[k]);
			}
		}
		break;
	case VIRTCHNL2_QUEUE_TYPE_RX_BUFFER:
		for (i = 0; i < rsrc->num_rxq_grp; i++) {
			struct idpf_rxq_group *rx_qgrp = &rsrc->rxq_grps[i];
			u8 num_bufqs = rsrc->num_bufqs_per_qgrp;

			for (j = 0; j < num_bufqs && k < num_regs; j++, k++) {
				struct idpf_buf_queue *q;

				q = &rx_qgrp->splitq.bufq_sets[j].bufq;
				q->tail = libie_pci_get_mmio_addr(mmio,
								  reg_vals[k]);
			}
		}
		break;
	default:
		break;
	}

	return k;
}

/**
 * idpf_queue_reg_init - initialize queue registers
 * @vport: virtual port structure
 * @rsrc: pointer to queue and vector resources
 * @chunks: queue registers received over mailbox
 *
 * Return 0 on success, negative on failure
 */
int idpf_queue_reg_init(struct idpf_vport *vport,
			struct idpf_q_vec_rsrc *rsrc,
			struct idpf_queue_id_reg_info *chunks)
{
	int num_regs, ret = 0;
	u32 *reg_vals;

	/* We may never deal with more than 256 same type of queues */
	reg_vals = kzalloc(sizeof(void *) * IDPF_LARGE_MAX_Q, GFP_KERNEL);
	if (!reg_vals)
		return -ENOMEM;

	/* Initialize Tx queue tail register address */
	num_regs = idpf_vport_get_q_reg(reg_vals, IDPF_LARGE_MAX_Q,
					VIRTCHNL2_QUEUE_TYPE_TX,
					chunks);
	if (num_regs < rsrc->num_txq) {
		ret = -EINVAL;
		goto free_reg_vals;
	}

	num_regs = __idpf_queue_reg_init(vport, rsrc, reg_vals, num_regs,
					 VIRTCHNL2_QUEUE_TYPE_TX);
	if (num_regs < rsrc->num_txq) {
		ret = -EINVAL;
		goto free_reg_vals;
	}

	/* Initialize Rx/buffer queue tail register address based on Rx queue
	 * model
	 */
	if (idpf_is_queue_model_split(rsrc->rxq_model)) {
		num_regs = idpf_vport_get_q_reg(reg_vals, IDPF_LARGE_MAX_Q,
						VIRTCHNL2_QUEUE_TYPE_RX_BUFFER,
						chunks);
		if (num_regs < rsrc->num_bufq) {
			ret = -EINVAL;
			goto free_reg_vals;
		}

		num_regs = __idpf_queue_reg_init(vport, rsrc, reg_vals, num_regs,
						 VIRTCHNL2_QUEUE_TYPE_RX_BUFFER);
		if (num_regs < rsrc->num_bufq) {
			ret = -EINVAL;
			goto free_reg_vals;
		}
	} else {
		num_regs = idpf_vport_get_q_reg(reg_vals, IDPF_LARGE_MAX_Q,
						VIRTCHNL2_QUEUE_TYPE_RX,
						chunks);
		if (num_regs < rsrc->num_rxq) {
			ret = -EINVAL;
			goto free_reg_vals;
		}

		num_regs = __idpf_queue_reg_init(vport, rsrc, reg_vals, num_regs,
						 VIRTCHNL2_QUEUE_TYPE_RX);
		if (num_regs < rsrc->num_rxq) {
			ret = -EINVAL;
			goto free_reg_vals;
		}
	}

free_reg_vals:
	kfree(reg_vals);

	return ret;
}

/**
 * idpf_send_create_vport_msg - Send virtchnl create vport message
 * @adapter: Driver specific private structure
 * @max_q: vport max queue info
 *
 * send virtchnl creae vport message
 *
 * Returns 0 on success, negative on failure
 */
int idpf_send_create_vport_msg(struct idpf_adapter *adapter,
			       struct idpf_vport_max_q *max_q)
{
	struct libie_ctlq_xn_send_params xn_params = {
		.timeout_ms	= IDPF_VC_XN_DEFAULT_TIMEOUT_MSEC,
		.chnl_opcode	= VIRTCHNL2_OP_CREATE_VPORT,
	};
	struct virtchnl2_create_vport *vport_msg;
	u16 idx = adapter->next_vport;
	int err, buf_size;

	buf_size = sizeof(struct virtchnl2_create_vport);
	vport_msg = kzalloc(buf_size, GFP_KERNEL);
	if (!vport_msg)
		return -ENOMEM;

	vport_msg->vport_type = cpu_to_le16(VIRTCHNL2_VPORT_TYPE_DEFAULT);
	vport_msg->vport_index = cpu_to_le16(idx);

	if (adapter->req_tx_splitq || !IS_ENABLED(CONFIG_IDPF_SINGLEQ))
		vport_msg->txq_model = cpu_to_le16(VIRTCHNL2_QUEUE_MODEL_SPLIT);
	else
		vport_msg->txq_model = cpu_to_le16(VIRTCHNL2_QUEUE_MODEL_SINGLE);

	if (adapter->req_rx_splitq || !IS_ENABLED(CONFIG_IDPF_SINGLEQ))
		vport_msg->rxq_model = cpu_to_le16(VIRTCHNL2_QUEUE_MODEL_SPLIT);
	else
		vport_msg->rxq_model = cpu_to_le16(VIRTCHNL2_QUEUE_MODEL_SINGLE);

	err = idpf_vport_calc_total_qs(adapter, idx, vport_msg, max_q);
	if (err) {
		dev_err(&adapter->pdev->dev, "Enough queues are not available");
		goto rel_buf;
	}

	if (!adapter->vport_params_recvd[idx]) {
		adapter->vport_params_recvd[idx] =
			kzalloc(LIBIE_CTLQ_MAX_BUF_LEN, GFP_KERNEL);
		if (!adapter->vport_params_recvd[idx]) {
			err = -ENOMEM;
			goto rel_buf;
		}
	}

	err = idpf_send_mb_msg(adapter, &xn_params, vport_msg,
			       sizeof(*vport_msg));
	if (err) {
		kfree(adapter->vport_params_recvd[idx]);
		adapter->vport_params_recvd[idx] = NULL;
		return err;
	}

	memcpy(adapter->vport_params_recvd[idx], xn_params.recv_mem.iov_base,
	       xn_params.recv_mem.iov_len);

	libie_ctlq_release_rx_buf(&xn_params.recv_mem);

	return 0;

rel_buf:
	kfree(vport_msg);

	return err;
}

/**
 * idpf_check_supported_desc_ids - Verify we have required descriptor support
 * @vport: virtual port structure
 *
 * Return 0 on success, error on failure
 */
int idpf_check_supported_desc_ids(struct idpf_vport *vport)
{
	struct idpf_q_vec_rsrc *rsrc = &vport->dflt_qv_rsrc;
	struct idpf_adapter *adapter = vport->adapter;
	struct virtchnl2_create_vport *vport_msg;
	u64 rx_desc_ids, tx_desc_ids;

	vport_msg = adapter->vport_params_recvd[vport->idx];

	if (!IS_ENABLED(CONFIG_IDPF_SINGLEQ) &&
	    (vport_msg->rxq_model == VIRTCHNL2_QUEUE_MODEL_SINGLE ||
	     vport_msg->txq_model == VIRTCHNL2_QUEUE_MODEL_SINGLE)) {
		pci_err(adapter->pdev, "singleq mode requested, but not compiled-in\n");
		return -EOPNOTSUPP;
	}

	rx_desc_ids = le64_to_cpu(vport_msg->rx_desc_ids);
	tx_desc_ids = le64_to_cpu(vport_msg->tx_desc_ids);

	if (idpf_is_queue_model_split(rsrc->rxq_model)) {
		if (!(rx_desc_ids & VIRTCHNL2_RXDID_2_FLEX_SPLITQ_M)) {
			dev_info(&adapter->pdev->dev, "Minimum RX descriptor support not provided, using the default\n");
			vport_msg->rx_desc_ids = cpu_to_le64(VIRTCHNL2_RXDID_2_FLEX_SPLITQ_M);
		}
	} else {
		if (!(rx_desc_ids & VIRTCHNL2_RXDID_2_FLEX_SQ_NIC_M))
			rsrc->base_rxd = true;
	}

	if (!idpf_is_queue_model_split(rsrc->txq_model))
		return 0;

	if ((tx_desc_ids & MIN_SUPPORT_TXDID) != MIN_SUPPORT_TXDID) {
		dev_info(&adapter->pdev->dev, "Minimum TX descriptor support not provided, using the default\n");
		vport_msg->tx_desc_ids = cpu_to_le64(MIN_SUPPORT_TXDID);
	}

	return 0;
}

/**
 * idpf_send_destroy_vport_msg - Send virtchnl destroy vport message
 * @adapter: adapter pointer used to send virtchnl message
 * @vport_id: vport identifier used while preparing the virtchnl message
 *
 * Send virtchnl destroy vport message.  Returns 0 on success, negative on
 * failure.
 */
int idpf_send_destroy_vport_msg(struct idpf_adapter *adapter, u32 vport_id)
{
	struct libie_ctlq_xn_send_params xn_params = {
		.timeout_ms	= IDPF_VC_XN_MIN_TIMEOUT_MSEC,
		.chnl_opcode	= VIRTCHNL2_OP_DESTROY_VPORT,
	};
	struct virtchnl2_vport v_id;
	int err;

	v_id.vport_id = cpu_to_le32(vport_id);

	err = idpf_send_mb_msg(adapter, &xn_params, &v_id, sizeof(v_id));
	if (err)
		return err;

	libie_ctlq_release_rx_buf(&xn_params.recv_mem);

	return 0;
}

/**
 * idpf_send_enable_vport_msg - Send virtchnl enable vport message
 * @adapter: adapter pointer used to send virtchnl message
 * @vport_id: vport identifier used while preparing the virtchnl message
 *
 * Send enable vport virtchnl message.  Returns 0 on success, negative on
 * failure.
 */
int idpf_send_enable_vport_msg(struct idpf_adapter *adapter, u32 vport_id)
{
	struct libie_ctlq_xn_send_params xn_params = {
		.timeout_ms	= IDPF_VC_XN_DEFAULT_TIMEOUT_MSEC,
		.chnl_opcode	= VIRTCHNL2_OP_ENABLE_VPORT,
	};
	struct virtchnl2_vport v_id;
	int err;

	v_id.vport_id = cpu_to_le32(vport_id);

	err = idpf_send_mb_msg(adapter, &xn_params, &v_id, sizeof(v_id));
	if (err)
		return err;

	libie_ctlq_release_rx_buf(&xn_params.recv_mem);

	return 0;
}

/**
 * idpf_send_disable_vport_msg - Send virtchnl disable vport message
 * @adapter: adapter pointer used to send virtchnl message
 * @vport_id: vport identifier used while preparing the virtchnl message
 *
 * Send disable vport virtchnl message.  Returns 0 on success, negative on
 * failure.
 */
int idpf_send_disable_vport_msg(struct idpf_adapter *adapter, u32 vport_id)
{
	struct libie_ctlq_xn_send_params xn_params = {
		.timeout_ms	= IDPF_VC_XN_MIN_TIMEOUT_MSEC,
		.chnl_opcode	= VIRTCHNL2_OP_DISABLE_VPORT,
	};
	struct virtchnl2_vport v_id;
	int err;

	v_id.vport_id = cpu_to_le32(vport_id);

	err = idpf_send_mb_msg(adapter, &xn_params, &v_id, sizeof(v_id));
	if (err)
		return err;

	libie_ctlq_release_rx_buf(&xn_params.recv_mem);

	return 0;
}

/**
 * idpf_send_config_tx_queues_msg - Send virtchnl config tx queues message
 * @adapter: adapter pointer used to send virtchnl message
 * @rsrc: pointer to queue and vector resources
 * @vport_id: vport identifier used while preparing the virtchnl message
 *
 * Send config tx queues virtchnl message. Returns 0 on success, negative on
 * failure.
 */
static int idpf_send_config_tx_queues_msg(struct idpf_adapter *adapter,
					  struct idpf_q_vec_rsrc *rsrc,
					  u32 vport_id)
{
	struct libie_ctlq_xn_send_params xn_params = {
		.timeout_ms	= IDPF_VC_XN_DEFAULT_TIMEOUT_MSEC,
		.chnl_opcode	= VIRTCHNL2_OP_CONFIG_TX_QUEUES,
	};
	struct virtchnl2_txq_info *qi __free(kfree) = NULL;
	struct virtchnl2_config_tx_queues *ctq;
	u32 config_sz, chunk_sz, buf_sz;
	int totqs, num_msgs, num_chunks;
	int k = 0, err = 0;

	totqs = rsrc->num_txq + rsrc->num_complq;
	qi = kcalloc(totqs, sizeof(struct virtchnl2_txq_info), GFP_KERNEL);
	if (!qi)
		return -ENOMEM;

	/* Populate the queue info buffer with all queue context info */
	for (u16 i = 0; i < rsrc->num_txq_grp; i++) {
		struct idpf_txq_group *tx_qgrp = &rsrc->txq_grps[i];
		int sched_mode;

		for (u16 j = 0; j < tx_qgrp->num_txq; j++, k++) {
			qi[k].queue_id =
				cpu_to_le32(tx_qgrp->txqs[j]->q_id);
			qi[k].model =
				cpu_to_le16(rsrc->txq_model);
			qi[k].type =
				cpu_to_le32(VIRTCHNL2_QUEUE_TYPE_TX);
			qi[k].ring_len =
				cpu_to_le16(tx_qgrp->txqs[j]->desc_count);
			qi[k].dma_ring_addr =
				cpu_to_le64(tx_qgrp->txqs[j]->dma);
			if (idpf_is_queue_model_split(rsrc->txq_model)) {
				struct idpf_tx_queue *q = tx_qgrp->txqs[j];

				qi[k].tx_compl_queue_id =
					cpu_to_le16(tx_qgrp->complq->q_id);
				qi[k].relative_queue_id = cpu_to_le16(j);

				if (idpf_queue_has(FLOW_SCH_EN, q))
					qi[k].sched_mode =
					cpu_to_le16(VIRTCHNL2_TXQ_SCHED_MODE_FLOW);
				else
					qi[k].sched_mode =
					cpu_to_le16(VIRTCHNL2_TXQ_SCHED_MODE_QUEUE);
			} else {
				qi[k].sched_mode =
					cpu_to_le16(VIRTCHNL2_TXQ_SCHED_MODE_QUEUE);
			}
		}

		if (!idpf_is_queue_model_split(rsrc->txq_model))
			continue;

		qi[k].queue_id = cpu_to_le32(tx_qgrp->complq->q_id);
		qi[k].model = cpu_to_le16(rsrc->txq_model);
		qi[k].type = cpu_to_le32(VIRTCHNL2_QUEUE_TYPE_TX_COMPLETION);
		qi[k].ring_len = cpu_to_le16(tx_qgrp->complq->desc_count);
		qi[k].dma_ring_addr = cpu_to_le64(tx_qgrp->complq->dma);

		if (idpf_queue_has(FLOW_SCH_EN, tx_qgrp->complq))
			sched_mode = VIRTCHNL2_TXQ_SCHED_MODE_FLOW;
		else
			sched_mode = VIRTCHNL2_TXQ_SCHED_MODE_QUEUE;
		qi[k].sched_mode = cpu_to_le16(sched_mode);

		k++;
	}

	/* Make sure accounting agrees */
	if (k != totqs)
		return -EINVAL;

	/* Chunk up the queue contexts into multiple messages to avoid
	 * sending a control queue message buffer that is too large
	 */
	config_sz = sizeof(struct virtchnl2_config_tx_queues);
	chunk_sz = sizeof(struct virtchnl2_txq_info);

	num_chunks = min_t(u32, IDPF_NUM_CHUNKS_PER_MSG(config_sz, chunk_sz),
			   totqs);
	num_msgs = DIV_ROUND_UP(totqs, num_chunks);

	for (u16 i = 0, k = 0; i < num_msgs; i++) {
		buf_sz = struct_size(ctq, qinfo, num_chunks);
		ctq = kzalloc(buf_sz, GFP_KERNEL);
		if (!ctq)
			return -ENOMEM;

		ctq->vport_id = cpu_to_le32(vport_id);
		ctq->num_qinfo = cpu_to_le16(num_chunks);
		memcpy(ctq->qinfo, &qi[k], chunk_sz * num_chunks);

		err = idpf_send_mb_msg(adapter, &xn_params, ctq, buf_sz);
		if (err)
			goto rel_last_buf;

		libie_ctlq_release_rx_buf(&xn_params.recv_mem);

		k += num_chunks;
		totqs -= num_chunks;
		num_chunks = min(num_chunks, totqs);
	}

rel_last_buf:
	/* Only the last buffer might be of size LIBIE_CP_TX_COPYBREAK or less.
	 * For buffers larger than LIBIE_CP_TX_COPYBREAK, are DMA mapped
	 * and released on mailbox cleanup. Smaller buffers are memcopied into
	 * the pre-allocated DMA buffers and are released here.
	 */
	if (num_msgs && libie_cp_can_send_onstack(buf_sz))
		kfree(ctq);

	return err;
}

/**
 * idpf_send_config_rx_queues_msg - Send virtchnl config rx queues message
 * @adapter: adapter pointer used to send virtchnl message
 * @rsrc: pointer to queue and vector resources
 * @vport_id: vport identifier used while preparing the virtchnl message
 * @rsc_ena: flag to check if RSC feature is enabled
 *
 * Send config rx queues virtchnl message.  Returns 0 on success, negative on
 * failure.
 */
static int idpf_send_config_rx_queues_msg(struct idpf_adapter *adapter,
					  struct idpf_q_vec_rsrc *rsrc,
					  u32 vport_id, bool rsc_ena)
{
	struct libie_ctlq_xn_send_params xn_params = {
		.timeout_ms	= IDPF_VC_XN_DEFAULT_TIMEOUT_MSEC,
		.chnl_opcode	= VIRTCHNL2_OP_CONFIG_RX_QUEUES,
	};
	struct virtchnl2_rxq_info *qi __free(kfree) = NULL;
	struct virtchnl2_config_rx_queues *crq;
	u32 config_sz, chunk_sz, buf_sz;
	int totqs, num_msgs, num_chunks;
	int k = 0, err = 0;

	totqs = rsrc->num_rxq + rsrc->num_bufq;
	qi = kcalloc(totqs, sizeof(struct virtchnl2_rxq_info), GFP_KERNEL);
	if (!qi)
		return -ENOMEM;

	/* Populate the queue info buffer with all queue context info */
	for (u16 i = 0; i < rsrc->num_rxq_grp; i++) {
		struct idpf_rxq_group *rx_qgrp = &rsrc->rxq_grps[i];
		u16 num_rxq;

		if (!idpf_is_queue_model_split(rsrc->rxq_model))
			goto setup_rxqs;

		for (u8 j = 0; j < rsrc->num_bufqs_per_qgrp; j++, k++) {
			struct idpf_buf_queue *bufq =
				&rx_qgrp->splitq.bufq_sets[j].bufq;

			qi[k].queue_id = cpu_to_le32(bufq->q_id);
			qi[k].model = cpu_to_le16(rsrc->rxq_model);
			qi[k].type =
				cpu_to_le32(VIRTCHNL2_QUEUE_TYPE_RX_BUFFER);
			qi[k].desc_ids = cpu_to_le64(VIRTCHNL2_RXDID_2_FLEX_SPLITQ_M);
			qi[k].ring_len = cpu_to_le16(bufq->desc_count);
			qi[k].dma_ring_addr = cpu_to_le64(bufq->dma);
			qi[k].data_buffer_size = cpu_to_le32(bufq->rx_buf_size);
			qi[k].buffer_notif_stride = IDPF_RX_BUF_STRIDE;
			qi[k].rx_buffer_low_watermark =
				cpu_to_le16(bufq->rx_buffer_low_watermark);
			if (rsc_ena)
				qi[k].qflags |= cpu_to_le16(VIRTCHNL2_RXQ_RSC);
		}

setup_rxqs:
		if (idpf_is_queue_model_split(rsrc->rxq_model))
			num_rxq = rx_qgrp->splitq.num_rxq_sets;
		else
			num_rxq = rx_qgrp->singleq.num_rxq;

		for (u16 j = 0; j < num_rxq; j++, k++) {
			const struct idpf_bufq_set *sets;
			struct idpf_rx_queue *rxq;

			if (!idpf_is_queue_model_split(rsrc->rxq_model)) {
				rxq = rx_qgrp->singleq.rxqs[j];
				goto common_qi_fields;
			}

			rxq = &rx_qgrp->splitq.rxq_sets[j]->rxq;
			sets = rxq->bufq_sets;

			/* In splitq mode, RXQ buffer size should be
			 * set to that of the first buffer queue
			 * associated with this RXQ.
			 */
			rxq->rx_buf_size = sets[0].bufq.rx_buf_size;

			qi[k].rx_bufq1_id = cpu_to_le16(sets[0].bufq.q_id);
			if (rsrc->num_bufqs_per_qgrp > IDPF_SINGLE_BUFQ_PER_RXQ_GRP) {
				qi[k].bufq2_ena = IDPF_BUFQ2_ENA;
				qi[k].rx_bufq2_id =
					cpu_to_le16(sets[1].bufq.q_id);
			}
			qi[k].rx_buffer_low_watermark =
				cpu_to_le16(rxq->rx_buffer_low_watermark);
			if (rsc_ena)
				qi[k].qflags |= cpu_to_le16(VIRTCHNL2_RXQ_RSC);

			rxq->rx_hbuf_size = sets[0].bufq.rx_hbuf_size;

			if (idpf_queue_has(HSPLIT_EN, rxq)) {
				qi[k].qflags |=
					cpu_to_le16(VIRTCHNL2_RXQ_HDR_SPLIT);
				qi[k].hdr_buffer_size =
					cpu_to_le16(rxq->rx_hbuf_size);
			}

common_qi_fields:
			qi[k].queue_id = cpu_to_le32(rxq->q_id);
			qi[k].model = cpu_to_le16(rsrc->rxq_model);
			qi[k].type = cpu_to_le32(VIRTCHNL2_QUEUE_TYPE_RX);
			qi[k].ring_len = cpu_to_le16(rxq->desc_count);
			qi[k].dma_ring_addr = cpu_to_le64(rxq->dma);
			qi[k].max_pkt_size = cpu_to_le32(rxq->rx_max_pkt_size);
			qi[k].data_buffer_size = cpu_to_le32(rxq->rx_buf_size);
			qi[k].qflags |=
				cpu_to_le16(VIRTCHNL2_RX_DESC_SIZE_32BYTE);
			qi[k].desc_ids = cpu_to_le64(rxq->rxdids);
		}
	}

	/* Make sure accounting agrees */
	if (k != totqs)
		return -EINVAL;

	/* Chunk up the queue contexts into multiple messages to avoid
	 * sending a control queue message buffer that is too large
	 */
	config_sz = sizeof(struct virtchnl2_config_rx_queues);
	chunk_sz = sizeof(struct virtchnl2_rxq_info);

	num_chunks = min_t(u32, IDPF_NUM_CHUNKS_PER_MSG(config_sz, chunk_sz),
			   totqs);
	num_msgs = DIV_ROUND_UP(totqs, num_chunks);

	for (u16 i = 0, k = 0; i < num_msgs; i++) {
		buf_sz = struct_size(crq, qinfo, num_chunks);
		crq = kzalloc(buf_sz, GFP_KERNEL);
		if (!crq)
			return -ENOMEM;

		crq->vport_id = cpu_to_le32(vport_id);
		crq->num_qinfo = cpu_to_le16(num_chunks);
		memcpy(crq->qinfo, &qi[k], chunk_sz * num_chunks);

		err = idpf_send_mb_msg(adapter, &xn_params, crq, buf_sz);
		if (err)
			goto rel_last_buf;

		libie_ctlq_release_rx_buf(&xn_params.recv_mem);

		k += num_chunks;
		totqs -= num_chunks;
		num_chunks = min(num_chunks, totqs);
	}

rel_last_buf:
	if (num_msgs && libie_cp_can_send_onstack(buf_sz))
		kfree(crq);

	return err;
}

/**
 * idpf_send_ena_dis_queues_msg - Send virtchnl enable or disable
 * queues message
 * @adapter: adapter pointer used to send virtchnl message
 * @chunks: queue register info
 * @vport_id: vport identifier used while preparing the virtchnl message
 * @ena: if true enable, false disable
 *
 * Send enable or disable queues virtchnl message. Returns 0 on success,
 * negative on failure.
 */
static int idpf_send_ena_dis_queues_msg(struct idpf_adapter *adapter,
					struct idpf_queue_id_reg_info *chunks,
					u32 vport_id,
					bool ena)
{
	struct libie_ctlq_xn_send_params xn_params = {
		.chnl_opcode = ena ? VIRTCHNL2_OP_ENABLE_QUEUES :
				     VIRTCHNL2_OP_DISABLE_QUEUES,
		.timeout_ms = ena ? IDPF_VC_XN_DEFAULT_TIMEOUT_MSEC :
				    IDPF_VC_XN_MIN_TIMEOUT_MSEC,
	};
	struct virtchnl2_del_ena_dis_queues *eq;
	u32 num_chunks, buf_sz;
	int err = 0;

	num_chunks = chunks->num_chunks;
	buf_sz = struct_size(eq, chunks.chunks, num_chunks);
	eq = kzalloc(buf_sz, GFP_KERNEL);
	if (!eq)
		return -ENOMEM;

	eq->vport_id = cpu_to_le32(vport_id);
	eq->chunks.num_chunks = cpu_to_le16(num_chunks);

	idpf_convert_reg_to_queue_chunks(eq->chunks.chunks, chunks->queue_chunks,
					 num_chunks);

	err = idpf_send_mb_msg(adapter, &xn_params, eq, buf_sz);
	if (err)
		goto rel_tx;

	libie_ctlq_release_rx_buf(&xn_params.recv_mem);
rel_tx:
	if (libie_cp_can_send_onstack(buf_sz))
		kfree(eq);

	return err;
}

/**
 * idpf_send_map_unmap_queue_vector_msg - Send virtchnl map or unmap queue
 * vector message
 * @adapter: adapter pointer used to send virtchnl message
 * @rsrc: pointer to queue and vector resources
 * @vport_id: vport identifier used while preparing the virtchnl message
 * @map: true for map and false for unmap
 *
 * Send map or unmap queue vector virtchnl message.  Returns 0 on success,
 * negative on failure.
 */
int idpf_send_map_unmap_queue_vector_msg(struct idpf_adapter *adapter,
					 struct idpf_q_vec_rsrc *rsrc,
					 u32 vport_id,
					 bool map)
{
	struct libie_ctlq_xn_send_params xn_params = {
		.timeout_ms =	map ? IDPF_VC_XN_DEFAULT_TIMEOUT_MSEC :
				      IDPF_VC_XN_MIN_TIMEOUT_MSEC,
		.chnl_opcode =	map ? VIRTCHNL2_OP_MAP_QUEUE_VECTOR :
				      VIRTCHNL2_OP_UNMAP_QUEUE_VECTOR,
	};
	struct virtchnl2_queue_vector *vqv __free(kfree) = NULL;
	struct virtchnl2_queue_vector_maps *vqvm;
	u32 config_sz, chunk_sz, buf_sz;
	u32 num_msgs, num_chunks, num_q;
	int k = 0, err = 0;

	num_q = rsrc->num_txq + rsrc->num_rxq;

	buf_sz = sizeof(struct virtchnl2_queue_vector) * num_q;
	vqv = kzalloc(buf_sz, GFP_KERNEL);
	if (!vqv)
		return -ENOMEM;

	for (u16 i = 0; i < rsrc->num_txq_grp; i++) {
		struct idpf_txq_group *tx_qgrp = &rsrc->txq_grps[i];

		for (u16 j = 0; j < tx_qgrp->num_txq; j++, k++) {
			vqv[k].queue_type =
				cpu_to_le32(VIRTCHNL2_QUEUE_TYPE_TX);
			vqv[k].queue_id = cpu_to_le32(tx_qgrp->txqs[j]->q_id);

			if (idpf_is_queue_model_split(rsrc->txq_model)) {
				vqv[k].vector_id =
				cpu_to_le16(tx_qgrp->complq->q_vector->v_idx);
				vqv[k].itr_idx =
				cpu_to_le32(tx_qgrp->complq->q_vector->tx_itr_idx);
			} else {
				vqv[k].vector_id =
				cpu_to_le16(tx_qgrp->txqs[j]->q_vector->v_idx);
				vqv[k].itr_idx =
				cpu_to_le32(tx_qgrp->txqs[j]->q_vector->tx_itr_idx);
			}
		}
	}

	if (rsrc->num_txq != k)
		return -EINVAL;

	for (u16 i = 0; i < rsrc->num_rxq_grp; i++) {
		struct idpf_rxq_group *rx_qgrp = &rsrc->rxq_grps[i];
		u16 num_rxq;

		if (idpf_is_queue_model_split(rsrc->rxq_model))
			num_rxq = rx_qgrp->splitq.num_rxq_sets;
		else
			num_rxq = rx_qgrp->singleq.num_rxq;

		for (u16 j = 0; j < num_rxq; j++, k++) {
			struct idpf_rx_queue *rxq;

			if (idpf_is_queue_model_split(rsrc->rxq_model))
				rxq = &rx_qgrp->splitq.rxq_sets[j]->rxq;
			else
				rxq = rx_qgrp->singleq.rxqs[j];

			vqv[k].queue_type =
				cpu_to_le32(VIRTCHNL2_QUEUE_TYPE_RX);
			vqv[k].queue_id = cpu_to_le32(rxq->q_id);
			vqv[k].vector_id = cpu_to_le16(rxq->q_vector->v_idx);
			vqv[k].itr_idx = cpu_to_le32(rxq->q_vector->rx_itr_idx);
		}
	}

	if (idpf_is_queue_model_split(rsrc->txq_model)) {
		if (rsrc->num_rxq != k - rsrc->num_complq)
			return -EINVAL;
	} else {
		if (rsrc->num_rxq != k - rsrc->num_txq)
			return -EINVAL;
	}

	/* Chunk up the vector info into multiple messages */
	config_sz = sizeof(struct virtchnl2_queue_vector_maps);
	chunk_sz = sizeof(struct virtchnl2_queue_vector);

	num_chunks = min_t(u32, IDPF_NUM_CHUNKS_PER_MSG(config_sz, chunk_sz),
			   num_q);
	num_msgs = DIV_ROUND_UP(num_q, num_chunks);

	for (u16 i = 0, k = 0; i < num_msgs; i++) {
		buf_sz = struct_size(vqvm, qv_maps, num_chunks);
		vqvm = kzalloc(buf_sz, GFP_KERNEL);
		if (!vqvm)
			return -ENOMEM;

		vqvm->vport_id = cpu_to_le32(vport_id);
		vqvm->num_qv_maps = cpu_to_le16(num_chunks);
		memcpy(vqvm->qv_maps, &vqv[k], chunk_sz * num_chunks);

		err = idpf_send_mb_msg(adapter, &xn_params, vqvm,
				       buf_sz);
		if (err)
			goto rel_last_buf;

		libie_ctlq_release_rx_buf(&xn_params.recv_mem);

		k += num_chunks;
		num_q -= num_chunks;
		num_chunks = min(num_chunks, num_q);
	}

rel_last_buf:
	if (num_msgs && libie_cp_can_send_onstack(buf_sz))
		kfree(vqvm);

	return err;
}

/**
 * idpf_send_enable_queues_msg - send enable queues virtchnl message
 * @vport: virtual port private data structure
 * @chunks: queue ids received over mailbox
 *
 * Will send enable queues virtchnl message.  Returns 0 on success, negative on
 * failure.
 */
int idpf_send_enable_queues_msg(struct idpf_vport *vport,
				struct idpf_queue_id_reg_info *chunks)
{
	return idpf_send_ena_dis_queues_msg(vport->adapter, chunks,
					    vport->vport_id, true);
}

/**
 * idpf_send_disable_queues_msg - send disable queues virtchnl message
 * @vport: virtual port private data structure
 * @rsrc: pointer to queue and vector resources
 * @chunks: queue ids received over mailbox
 *
 * Will send disable queues virtchnl message.  Returns 0 on success, negative
 * on failure.
 */
int idpf_send_disable_queues_msg(struct idpf_vport *vport,
				 struct idpf_q_vec_rsrc *rsrc,
				 struct idpf_queue_id_reg_info *chunks)
{
	int err;

	err = idpf_send_ena_dis_queues_msg(vport->adapter, chunks,
					   vport->vport_id, false);
	if (err)
		return err;

	/* switch to poll mode as interrupts will be disabled after disable
	 * queues virtchnl message is sent
	 */
	for (u16 i = 0; i < vport->num_txq; i++)
		idpf_queue_set(POLL_MODE, vport->txqs[i]);

	/* schedule the napi to receive all the marker packets */
	local_bh_disable();
	for (u16 i = 0; i < rsrc->num_q_vectors; i++)
		napi_schedule(&rsrc->q_vectors[i].napi);
	local_bh_enable();

	return idpf_wait_for_marker_event(vport);
}

/**
 * idpf_send_delete_queues_msg - send delete queues virtchnl message
 * @adapter: adapter pointer used to send virtchnl message
 * @chunks: queue ids received over mailbox
 * @vport_id: vport identifier used while preparing the virtchnl message
 *
 * Will send delete queues virtchnl message. Return 0 on success, negative on
 * failure.
 */
int idpf_send_delete_queues_msg(struct idpf_adapter *adapter,
				struct idpf_queue_id_reg_info *chunks,
				u32 vport_id)
{
	struct libie_ctlq_xn_send_params xn_params = {
		.timeout_ms	= IDPF_VC_XN_MIN_TIMEOUT_MSEC,
		.chnl_opcode	= VIRTCHNL2_OP_DEL_QUEUES,
	};
	struct virtchnl2_del_ena_dis_queues *eq;
	int buf_size, err;
	u16 num_chunks;

	num_chunks = chunks->num_chunks;
	buf_size = struct_size(eq, chunks.chunks, num_chunks);

	eq = kzalloc(buf_size, GFP_KERNEL);
	if (!eq)
		return -ENOMEM;

	eq->vport_id = cpu_to_le32(vport_id);
	eq->chunks.num_chunks = cpu_to_le16(num_chunks);

	idpf_convert_reg_to_queue_chunks(eq->chunks.chunks, chunks->queue_chunks,
					 num_chunks);

	err = idpf_send_mb_msg(adapter, &xn_params, eq, buf_size);
	if (err)
		goto rel_buf;

	libie_ctlq_release_rx_buf(&xn_params.recv_mem);

rel_buf:
	if (libie_cp_can_send_onstack(buf_size))
		kfree(eq);

	return err;
}

/**
 * idpf_send_config_queues_msg - Send config queues virtchnl message
 * @adapter: adapter pointer used to send virtchnl message
 * @rsrc: pointer to queue and vector resources
 * @vport_id: vport identifier used while preparing the virtchnl message
 * @rsc_ena: flag to check if RSC feature is enabled
 *
 * Will send config queues virtchnl message. Returns 0 on success, negative on
 * failure.
 */
int idpf_send_config_queues_msg(struct idpf_adapter *adapter,
				struct idpf_q_vec_rsrc *rsrc,
				u32 vport_id, bool rsc_ena)
{
	int err;

	err = idpf_send_config_tx_queues_msg(adapter, rsrc, vport_id);
	if (err)
		return err;

	return idpf_send_config_rx_queues_msg(adapter, rsrc, vport_id, rsc_ena);
}

/**
 * idpf_send_add_queues_msg - Send virtchnl add queues message
 * @adapter: adapter pointer used to send virtchnl message
 * @vport_config: vport persistent structure to store the queue chunk info
 * @rsrc: pointer to queue and vector resources
 * @vport_id: vport identifier used while preparing the virtchnl message
 *
 * Returns 0 on success, negative on failure. vport _MUST_ be const here as
 * we should not change any fields within vport itself in this function.
 */
int idpf_send_add_queues_msg(struct idpf_adapter *adapter,
			     struct idpf_vport_config *vport_config,
			     struct idpf_q_vec_rsrc *rsrc,
			     u32 vport_id)
{
	struct libie_ctlq_xn_send_params xn_params = {
		.timeout_ms	= IDPF_VC_XN_DEFAULT_TIMEOUT_MSEC,
		.chnl_opcode	= VIRTCHNL2_OP_ADD_QUEUES,
	};
	struct virtchnl2_add_queues *vc_msg;
	struct virtchnl2_add_queues aq = {};
	int err;

	aq.vport_id = cpu_to_le32(vport_id);
	aq.num_tx_q = cpu_to_le16(rsrc->num_txq);
	aq.num_tx_complq = cpu_to_le16(rsrc->num_complq);
	aq.num_rx_q = cpu_to_le16(rsrc->num_rxq);
	aq.num_rx_bufq = cpu_to_le16(rsrc->num_bufq);

	err = idpf_send_mb_msg(adapter, &xn_params, &aq, sizeof(aq));
	if (err)
		return err;

	vc_msg = xn_params.recv_mem.iov_base;

	/* compare vc_msg num queues with vport num queues */
	if (le16_to_cpu(vc_msg->num_tx_q) != rsrc->num_txq ||
	    le16_to_cpu(vc_msg->num_rx_q) != rsrc->num_rxq ||
	    le16_to_cpu(vc_msg->num_tx_complq) != rsrc->num_complq ||
	    le16_to_cpu(vc_msg->num_rx_bufq) != rsrc->num_bufq) {
		err = -EINVAL;
		goto free_rx_buf;
	}

	err = idpf_vport_init_queue_reg_chunks(vport_config, &vc_msg->chunks);

free_rx_buf:
	libie_ctlq_release_rx_buf(&xn_params.recv_mem);

	return err;
}

/**
 * idpf_send_alloc_vectors_msg - Send virtchnl alloc vectors message
 * @adapter: Driver specific private structure
 * @num_vectors: number of vectors to be allocated
 *
 * Returns 0 on success, negative on failure.
 */
int idpf_send_alloc_vectors_msg(struct idpf_adapter *adapter, u16 num_vectors)
{
	struct libie_ctlq_xn_send_params xn_params = {
		.timeout_ms	= IDPF_VC_XN_DEFAULT_TIMEOUT_MSEC,
		.chnl_opcode	= VIRTCHNL2_OP_ALLOC_VECTORS,
	};
	struct virtchnl2_alloc_vectors *rcvd_vec;
	struct virtchnl2_alloc_vectors ac = {};
	u16 num_vchunks;
	int size, err;

	ac.num_vectors = cpu_to_le16(num_vectors);

	err = idpf_send_mb_msg(adapter, &xn_params, &ac, sizeof(ac));
	if (err)
		return err;

	rcvd_vec = xn_params.recv_mem.iov_base;

	num_vchunks = le16_to_cpu(rcvd_vec->vchunks.num_vchunks);
	size = struct_size(rcvd_vec, vchunks.vchunks, num_vchunks);
	if (xn_params.recv_mem.iov_len < size) {
		err = -EIO;
		goto free_rx_buf;
	}

	if (size > LIBIE_CTLQ_MAX_BUF_LEN) {
		err = -EINVAL;
		goto free_rx_buf;
	}

	kfree(adapter->req_vec_chunks);
	adapter->req_vec_chunks = kmemdup(rcvd_vec, size, GFP_KERNEL);
	if (!adapter->req_vec_chunks) {
		err = -ENOMEM;
		goto free_rx_buf;
	}

	if (le16_to_cpu(adapter->req_vec_chunks->num_vectors) < num_vectors) {
		kfree(adapter->req_vec_chunks);
		adapter->req_vec_chunks = NULL;
		err = -EINVAL;
	}

free_rx_buf:
	libie_ctlq_release_rx_buf(&xn_params.recv_mem);

	return err;
}

/**
 * idpf_send_dealloc_vectors_msg - Send virtchnl de allocate vectors message
 * @adapter: Driver specific private structure
 *
 * Returns 0 on success, negative on failure.
 */
int idpf_send_dealloc_vectors_msg(struct idpf_adapter *adapter)
{
	struct virtchnl2_alloc_vectors *ac = adapter->req_vec_chunks;
	struct libie_ctlq_xn_send_params xn_params = {
		.timeout_ms	= IDPF_VC_XN_MIN_TIMEOUT_MSEC,
		.chnl_opcode	= VIRTCHNL2_OP_DEALLOC_VECTORS,
	};
	struct virtchnl2_vector_chunks *vcs;
	int buf_size, err;

	buf_size = struct_size(&ac->vchunks, vchunks,
			       le16_to_cpu(ac->vchunks.num_vchunks));
	vcs = kmemdup(&ac->vchunks, buf_size, GFP_KERNEL);
	if (!vcs)
		return -ENOMEM;

	err = idpf_send_mb_msg(adapter, &xn_params, vcs, buf_size);
	if (err)
		goto rel_buf;

	kfree(adapter->req_vec_chunks);
	adapter->req_vec_chunks = NULL;

	libie_ctlq_release_rx_buf(&xn_params.recv_mem);

rel_buf:
	if (libie_cp_can_send_onstack(buf_size))
		kfree(vcs);

	return err;
}

/**
 * idpf_get_max_vfs - Get max number of vfs supported
 * @adapter: Driver specific private structure
 *
 * Returns max number of VFs
 */
static int idpf_get_max_vfs(struct idpf_adapter *adapter)
{
	return le16_to_cpu(adapter->caps.max_sriov_vfs);
}

/**
 * idpf_send_set_sriov_vfs_msg - Send virtchnl set sriov vfs message
 * @adapter: Driver specific private structure
 * @num_vfs: number of virtual functions to be created
 *
 * Returns 0 on success, negative on failure.
 */
int idpf_send_set_sriov_vfs_msg(struct idpf_adapter *adapter, u16 num_vfs)
{
	struct libie_ctlq_xn_send_params xn_params = {
		.timeout_ms	= IDPF_VC_XN_DEFAULT_TIMEOUT_MSEC,
		.chnl_opcode	= VIRTCHNL2_OP_SET_SRIOV_VFS,
	};
	struct virtchnl2_sriov_vfs_info svi = {};
	int err;

	svi.num_vfs = cpu_to_le16(num_vfs);

	err = idpf_send_mb_msg(adapter, &xn_params, &svi, sizeof(svi));
	if (err)
		return err;

	libie_ctlq_release_rx_buf(&xn_params.recv_mem);

	return 0;
}

/**
 * idpf_send_get_stats_msg - Send virtchnl get statistics message
 * @np: netdev private structure
 * @port_stats: structure to store the vport statistics
 *
 * Returns 0 on success, negative on failure.
 */
int idpf_send_get_stats_msg(struct idpf_netdev_priv *np,
			    struct idpf_port_stats *port_stats)
{
	struct libie_ctlq_xn_send_params xn_params = {
		.timeout_ms	= IDPF_VC_XN_DEFAULT_TIMEOUT_MSEC,
		.chnl_opcode	= VIRTCHNL2_OP_GET_STATS,
	};
	struct rtnl_link_stats64 *netstats = &np->netstats;
	struct virtchnl2_vport_stats *stats_recv;
	struct virtchnl2_vport_stats stats_msg = {};
	int err;

	/* Don't send get_stats message if the link is down */
	if (np->state <= __IDPF_VPORT_DOWN)
		return 0;

	stats_msg.vport_id = cpu_to_le32(np->vport_id);

	err = idpf_send_mb_msg(np->adapter, &xn_params, &stats_msg,
			       sizeof(stats_msg));
	if (err)
		return err;

	if (xn_params.recv_mem.iov_len < sizeof(*stats_recv)) {
		err = -EIO;
		goto free_rx_buf;
	}

	stats_recv = xn_params.recv_mem.iov_base;

	spin_lock_bh(&np->stats_lock);

	netstats->rx_packets = le64_to_cpu(stats_recv->rx_unicast) +
			       le64_to_cpu(stats_recv->rx_multicast) +
			       le64_to_cpu(stats_recv->rx_broadcast);
	netstats->tx_packets = le64_to_cpu(stats_recv->tx_unicast) +
			       le64_to_cpu(stats_recv->tx_multicast) +
			       le64_to_cpu(stats_recv->tx_broadcast);
	netstats->rx_bytes = le64_to_cpu(stats_recv->rx_bytes);
	netstats->tx_bytes = le64_to_cpu(stats_recv->tx_bytes);
	netstats->rx_errors = le64_to_cpu(stats_recv->rx_errors);
	netstats->tx_errors = le64_to_cpu(stats_recv->tx_errors);
	netstats->rx_dropped = le64_to_cpu(stats_recv->rx_discards);
	netstats->tx_dropped = le64_to_cpu(stats_recv->tx_discards);

	port_stats->vport_stats = *stats_recv;

	spin_unlock_bh(&np->stats_lock);

free_rx_buf:
	libie_ctlq_release_rx_buf(&xn_params.recv_mem);

	return 0;
}

/**
 * idpf_send_get_set_rss_lut_msg - Send virtchnl get or set RSS lut message
 * @adapter: adapter pointer used to send virtchnl message
 * @rss_data: pointer to RSS key and lut info
 * @vport_id: vport identifier used while preparing the virtchnl message
 * @get: flag to set or get RSS look up table
 *
 * Returns 0 on success, negative on failure.
 */
int idpf_send_get_set_rss_lut_msg(struct idpf_adapter *adapter,
				  struct idpf_rss_data *rss_data,
				  u32 vport_id, bool get)
{
	struct libie_ctlq_xn_send_params xn_params = {
		.timeout_ms	= IDPF_VC_XN_DEFAULT_TIMEOUT_MSEC,
		.chnl_opcode	= get ? VIRTCHNL2_OP_GET_RSS_LUT :
					VIRTCHNL2_OP_SET_RSS_LUT,
	};
	struct virtchnl2_rss_lut *rl, *recv_rl;
	int buf_size, lut_buf_size;
	int i, err;

	buf_size = struct_size(rl, lut, rss_data->rss_lut_size);
	rl = kzalloc(buf_size, GFP_KERNEL);
	if (!rl)
		return -ENOMEM;

	rl->vport_id = cpu_to_le32(vport_id);
	if (!get) {
		rl->lut_entries = cpu_to_le16(rss_data->rss_lut_size);
		for (i = 0; i < rss_data->rss_lut_size; i++)
			rl->lut[i] = cpu_to_le32(rss_data->rss_lut[i]);
	}

	err = idpf_send_mb_msg(adapter, &xn_params, rl, buf_size);
	if (err)
		goto free_tx_buf;

	if (!get)
		goto free_rx_buf;
	if (xn_params.recv_mem.iov_len < sizeof(struct virtchnl2_rss_lut)) {
		err = -EIO;
		goto free_rx_buf;
	}

	recv_rl = xn_params.recv_mem.iov_base;

	lut_buf_size = le16_to_cpu(recv_rl->lut_entries) * sizeof(u32);
	if (xn_params.recv_mem.iov_len < lut_buf_size) {
		err = -EIO;
		goto free_rx_buf;
	}

	/* size didn't change, we can reuse existing lut buf */
	if (rss_data->rss_lut_size == le16_to_cpu(recv_rl->lut_entries))
		goto do_memcpy;

	rss_data->rss_lut_size = le16_to_cpu(recv_rl->lut_entries);
	kfree(rss_data->rss_lut);

	rss_data->rss_lut = kzalloc(lut_buf_size, GFP_KERNEL);
	if (!rss_data->rss_lut) {
		rss_data->rss_lut_size = 0;
		err = -ENOMEM;
		goto free_rx_buf;
	}

do_memcpy:
	memcpy(rss_data->rss_lut, recv_rl->lut, rss_data->rss_lut_size);
free_rx_buf:
	libie_ctlq_release_rx_buf(&xn_params.recv_mem);
free_tx_buf:
	if (libie_cp_can_send_onstack(buf_size))
		kfree(rl);

	return err;
}

/**
 * idpf_send_get_set_rss_key_msg - Send virtchnl get or set RSS key message
 * @adapter: adapter pointer used to send virtchnl message
 * @rss_data: pointer to RSS key and lut info
 * @vport_id: vport identifier used while preparing the virtchnl message
 * @get: flag to set or get RSS look up table
 *
 * Returns 0 on success, negative on failure
 */
int idpf_send_get_set_rss_key_msg(struct idpf_adapter *adapter,
				  struct idpf_rss_data *rss_data,
				  u32 vport_id, bool get)
{
	struct libie_ctlq_xn_send_params xn_params = {
		.timeout_ms	= IDPF_VC_XN_DEFAULT_TIMEOUT_MSEC,
		.chnl_opcode	= get ? VIRTCHNL2_OP_GET_RSS_KEY :
					VIRTCHNL2_OP_SET_RSS_KEY,
	};
	struct virtchnl2_rss_key *rk, *recv_rk;
	u16 key_size, recv_len;
	int i, buf_size, err;

	buf_size = struct_size(rk, key_flex, rss_data->rss_key_size);
	rk = kzalloc(buf_size, GFP_KERNEL);
	if (!rk)
		return -ENOMEM;

	rk->vport_id = cpu_to_le32(vport_id);
	if (!get) {
		rk->key_len = cpu_to_le16(rss_data->rss_key_size);
		for (i = 0; i < rss_data->rss_key_size; i++)
			rk->key_flex[i] = rss_data->rss_key[i];
	}

	err = idpf_send_mb_msg(adapter, &xn_params, rk, buf_size);
	if (err)
		goto free_tx_buf;

	if (!get)
		goto free_rx_buf;

	recv_len = xn_params.recv_mem.iov_len;
	if (recv_len < sizeof(struct virtchnl2_rss_key)) {
		err = -EIO;
		goto free_rx_buf;
	}

	recv_rk = xn_params.recv_mem.iov_base;
	key_size = min_t(u16, NETDEV_RSS_KEY_LEN,
			 le16_to_cpu(recv_rk->key_len));
	if (recv_len < key_size) {
		err = -EIO;
		goto free_rx_buf;
	}

	/* key len didn't change, reuse existing buf */
	if (rss_data->rss_key_size == key_size)
		goto do_memcpy;

	rss_data->rss_key_size = key_size;
	kfree(rss_data->rss_key);
	rss_data->rss_key = kzalloc(key_size, GFP_KERNEL);
	if (!rss_data->rss_key) {
		rss_data->rss_key_size = 0;
		err = -ENOMEM;
		goto free_rx_buf;
	}

do_memcpy:
	memcpy(rss_data->rss_key, recv_rk->key_flex, rss_data->rss_key_size);
free_rx_buf:
	libie_ctlq_release_rx_buf(&xn_params.recv_mem);
free_tx_buf:
	if (libie_cp_can_send_onstack(buf_size))
		kfree(rk);

	return err;
}

/**
 * idpf_fill_ptype_lookup - Fill L3 specific fields in ptype lookup table
 * @ptype: ptype lookup table
 * @pstate: state machine for ptype lookup table
 * @ipv4: ipv4 or ipv6
 * @frag: fragmentation allowed
 *
 */
static void idpf_fill_ptype_lookup(struct libeth_rx_pt *ptype,
				   struct idpf_ptype_state *pstate,
				   bool ipv4, bool frag)
{
	if (!pstate->outer_ip || !pstate->outer_frag) {
		pstate->outer_ip = true;

		if (ipv4)
			ptype->outer_ip = LIBETH_RX_PT_OUTER_IPV4;
		else
			ptype->outer_ip = LIBETH_RX_PT_OUTER_IPV6;

		if (frag) {
			ptype->outer_frag = LIBETH_RX_PT_FRAG;
			pstate->outer_frag = true;
		}
	} else {
		ptype->tunnel_type = LIBETH_RX_PT_TUNNEL_IP_IP;
		pstate->tunnel_state = IDPF_PTYPE_TUNNEL_IP;

		if (ipv4)
			ptype->tunnel_end_prot = LIBETH_RX_PT_TUNNEL_END_IPV4;
		else
			ptype->tunnel_end_prot = LIBETH_RX_PT_TUNNEL_END_IPV6;

		if (frag)
			ptype->tunnel_end_frag = LIBETH_RX_PT_FRAG;
	}
}

static void idpf_finalize_ptype_lookup(struct libeth_rx_pt *ptype)
{
	if (ptype->payload_layer == LIBETH_RX_PT_PAYLOAD_L2 &&
	    ptype->inner_prot)
		ptype->payload_layer = LIBETH_RX_PT_PAYLOAD_L4;
	else if (ptype->payload_layer == LIBETH_RX_PT_PAYLOAD_L2 &&
		 ptype->outer_ip)
		ptype->payload_layer = LIBETH_RX_PT_PAYLOAD_L3;
	else if (ptype->outer_ip == LIBETH_RX_PT_OUTER_L2)
		ptype->payload_layer = LIBETH_RX_PT_PAYLOAD_L2;
	else
		ptype->payload_layer = LIBETH_RX_PT_PAYLOAD_NONE;

	libeth_rx_pt_gen_hash_type(ptype);
}

/**
 * idpf_parse_protocol_ids - parse protocol IDs for a given packet type
 * @ptype: packet type to parse
 * @rx_pt: store the parsed packet type info into
 */
static void idpf_parse_protocol_ids(struct virtchnl2_ptype *ptype,
				    struct libeth_rx_pt *rx_pt)
{
	struct idpf_ptype_state pstate = {};

	for (u32 j = 0; j < ptype->proto_id_count; j++) {
		u16 id = le16_to_cpu(ptype->proto_id[j]);

		switch (id) {
		case VIRTCHNL2_PROTO_HDR_GRE:
			if (pstate.tunnel_state == IDPF_PTYPE_TUNNEL_IP) {
				rx_pt->tunnel_type =
					LIBETH_RX_PT_TUNNEL_IP_GRENAT;
				pstate.tunnel_state |=
					IDPF_PTYPE_TUNNEL_IP_GRENAT;
			}
			break;
		case VIRTCHNL2_PROTO_HDR_MAC:
			rx_pt->outer_ip = LIBETH_RX_PT_OUTER_L2;
			if (pstate.tunnel_state == IDPF_TUN_IP_GRE) {
				rx_pt->tunnel_type =
					LIBETH_RX_PT_TUNNEL_IP_GRENAT_MAC;
				pstate.tunnel_state |=
					IDPF_PTYPE_TUNNEL_IP_GRENAT_MAC;
			}
			break;
		case VIRTCHNL2_PROTO_HDR_IPV4:
			idpf_fill_ptype_lookup(rx_pt, &pstate, true, false);
			break;
		case VIRTCHNL2_PROTO_HDR_IPV6:
			idpf_fill_ptype_lookup(rx_pt, &pstate, false, false);
			break;
		case VIRTCHNL2_PROTO_HDR_IPV4_FRAG:
			idpf_fill_ptype_lookup(rx_pt, &pstate, true, true);
			break;
		case VIRTCHNL2_PROTO_HDR_IPV6_FRAG:
			idpf_fill_ptype_lookup(rx_pt, &pstate, false, true);
			break;
		case VIRTCHNL2_PROTO_HDR_UDP:
			rx_pt->inner_prot = LIBETH_RX_PT_INNER_UDP;
			break;
		case VIRTCHNL2_PROTO_HDR_TCP:
			rx_pt->inner_prot = LIBETH_RX_PT_INNER_TCP;
			break;
		case VIRTCHNL2_PROTO_HDR_SCTP:
			rx_pt->inner_prot = LIBETH_RX_PT_INNER_SCTP;
			break;
		case VIRTCHNL2_PROTO_HDR_ICMP:
			rx_pt->inner_prot = LIBETH_RX_PT_INNER_ICMP;
			break;
		case VIRTCHNL2_PROTO_HDR_PAY:
			rx_pt->payload_layer = LIBETH_RX_PT_PAYLOAD_L2;
			break;
		case VIRTCHNL2_PROTO_HDR_ICMPV6:
		case VIRTCHNL2_PROTO_HDR_IPV6_EH:
		case VIRTCHNL2_PROTO_HDR_PRE_MAC:
		case VIRTCHNL2_PROTO_HDR_POST_MAC:
		case VIRTCHNL2_PROTO_HDR_ETHERTYPE:
		case VIRTCHNL2_PROTO_HDR_SVLAN:
		case VIRTCHNL2_PROTO_HDR_CVLAN:
		case VIRTCHNL2_PROTO_HDR_MPLS:
		case VIRTCHNL2_PROTO_HDR_MMPLS:
		case VIRTCHNL2_PROTO_HDR_PTP:
		case VIRTCHNL2_PROTO_HDR_CTRL:
		case VIRTCHNL2_PROTO_HDR_LLDP:
		case VIRTCHNL2_PROTO_HDR_ARP:
		case VIRTCHNL2_PROTO_HDR_ECP:
		case VIRTCHNL2_PROTO_HDR_EAPOL:
		case VIRTCHNL2_PROTO_HDR_PPPOD:
		case VIRTCHNL2_PROTO_HDR_PPPOE:
		case VIRTCHNL2_PROTO_HDR_IGMP:
		case VIRTCHNL2_PROTO_HDR_AH:
		case VIRTCHNL2_PROTO_HDR_ESP:
		case VIRTCHNL2_PROTO_HDR_IKE:
		case VIRTCHNL2_PROTO_HDR_NATT_KEEP:
		case VIRTCHNL2_PROTO_HDR_L2TPV2:
		case VIRTCHNL2_PROTO_HDR_L2TPV2_CONTROL:
		case VIRTCHNL2_PROTO_HDR_L2TPV3:
		case VIRTCHNL2_PROTO_HDR_GTP:
		case VIRTCHNL2_PROTO_HDR_GTP_EH:
		case VIRTCHNL2_PROTO_HDR_GTPCV2:
		case VIRTCHNL2_PROTO_HDR_GTPC_TEID:
		case VIRTCHNL2_PROTO_HDR_GTPU:
		case VIRTCHNL2_PROTO_HDR_GTPU_UL:
		case VIRTCHNL2_PROTO_HDR_GTPU_DL:
		case VIRTCHNL2_PROTO_HDR_ECPRI:
		case VIRTCHNL2_PROTO_HDR_VRRP:
		case VIRTCHNL2_PROTO_HDR_OSPF:
		case VIRTCHNL2_PROTO_HDR_TUN:
		case VIRTCHNL2_PROTO_HDR_NVGRE:
		case VIRTCHNL2_PROTO_HDR_VXLAN:
		case VIRTCHNL2_PROTO_HDR_VXLAN_GPE:
		case VIRTCHNL2_PROTO_HDR_GENEVE:
		case VIRTCHNL2_PROTO_HDR_NSH:
		case VIRTCHNL2_PROTO_HDR_QUIC:
		case VIRTCHNL2_PROTO_HDR_PFCP:
		case VIRTCHNL2_PROTO_HDR_PFCP_NODE:
		case VIRTCHNL2_PROTO_HDR_PFCP_SESSION:
		case VIRTCHNL2_PROTO_HDR_RTP:
		case VIRTCHNL2_PROTO_HDR_NO_PROTO:
			break;
		default:
			break;
		}
	}
}

/**
 * idpf_send_get_rx_ptype_msg - Send virtchnl for ptype info
 * @adapter: driver specific private structure
 *
 * Returns 0 on success, negative on failure.
 */
static int idpf_send_get_rx_ptype_msg(struct idpf_adapter *adapter)
{
	struct libie_ctlq_xn_send_params xn_params = {
		.timeout_ms	= IDPF_VC_XN_DEFAULT_TIMEOUT_MSEC,
		.chnl_opcode	= VIRTCHNL2_OP_GET_PTYPE_INFO,
	};
	struct libeth_rx_pt *singleq_pt_lkup __free(kfree) = NULL;
	struct libeth_rx_pt *splitq_pt_lkup __free(kfree) = NULL;
	struct virtchnl2_get_ptype_info *get_ptype_info;
	struct virtchnl2_get_ptype_info *ptype_info;
	int buf_size = sizeof(*get_ptype_info);
	int ptypes_recvd = 0, ptype_offset;
	int max_ptype = IDPF_RX_MAX_PTYPE;
	u16 next_ptype_id = 0;
	int err = 0;

	singleq_pt_lkup = kcalloc(IDPF_RX_MAX_BASE_PTYPE,
				  sizeof(*singleq_pt_lkup), GFP_KERNEL);
	if (!singleq_pt_lkup)
		return -ENOMEM;

	splitq_pt_lkup = kcalloc(max_ptype, sizeof(*splitq_pt_lkup), GFP_KERNEL);
	if (!splitq_pt_lkup)
		return -ENOMEM;

	while (next_ptype_id < max_ptype) {
		get_ptype_info = kzalloc(buf_size, GFP_KERNEL);
		if (!get_ptype_info)
			return -ENOMEM;

		get_ptype_info->start_ptype_id = cpu_to_le16(next_ptype_id);

		if ((next_ptype_id + IDPF_RX_MAX_PTYPES_PER_BUF) > max_ptype)
			get_ptype_info->num_ptypes =
				cpu_to_le16(max_ptype - next_ptype_id);
		else
			get_ptype_info->num_ptypes =
				cpu_to_le16(IDPF_RX_MAX_PTYPES_PER_BUF);

		err = idpf_send_mb_msg(adapter, &xn_params, get_ptype_info,
				       buf_size);
		if (err)
			goto free_tx_buf;

		ptype_info = xn_params.recv_mem.iov_base;
		ptypes_recvd += le16_to_cpu(ptype_info->num_ptypes);
		if (ptypes_recvd > max_ptype) {
			err = -EINVAL;
			goto free_rx_buf;
		}

		next_ptype_id = le16_to_cpu(get_ptype_info->start_ptype_id) +
				le16_to_cpu(get_ptype_info->num_ptypes);

		ptype_offset = IDPF_RX_PTYPE_HDR_SZ;

		for (u16 i = 0; i < le16_to_cpu(ptype_info->num_ptypes); i++) {
			struct libeth_rx_pt rx_pt = {};
			struct virtchnl2_ptype *ptype;
			u16 pt_10, pt_8;

			ptype = (struct virtchnl2_ptype *)
					((u8 *)ptype_info + ptype_offset);

			pt_10 = le16_to_cpu(ptype->ptype_id_10);
			pt_8 = ptype->ptype_id_8;

			ptype_offset += IDPF_GET_PTYPE_SIZE(ptype);
			if (ptype_offset > LIBIE_CTLQ_MAX_BUF_LEN) {
				err = -EINVAL;
				goto free_rx_buf;
			}

			/* 0xFFFF indicates end of ptypes */
			if (pt_10 == IDPF_INVALID_PTYPE_ID)
				goto out;

			idpf_parse_protocol_ids(ptype, &rx_pt);
			idpf_finalize_ptype_lookup(&rx_pt);

			/* For a given protocol ID stack, the ptype value might
			 * vary between ptype_id_10 and ptype_id_8. So store
			 * them separately for splitq and singleq. Also skip
			 * the repeated ptypes in case of singleq.
			 */
			splitq_pt_lkup[pt_10] = rx_pt;
			if (!singleq_pt_lkup[pt_8].outer_ip)
				singleq_pt_lkup[pt_8] = rx_pt;
		}

		libie_ctlq_release_rx_buf(&xn_params.recv_mem);
		if (libie_cp_can_send_onstack(buf_size)) {
			kfree(get_ptype_info);
			get_ptype_info = NULL;
		}
		xn_params.recv_mem = (struct kvec) {};
	}

out:
	adapter->splitq_pt_lkup = no_free_ptr(splitq_pt_lkup);
	adapter->singleq_pt_lkup = no_free_ptr(singleq_pt_lkup);
free_rx_buf:
	libie_ctlq_release_rx_buf(&xn_params.recv_mem);
free_tx_buf:
	if (libie_cp_can_send_onstack(buf_size))
		kfree(get_ptype_info);

	return err;
}

/**
 * idpf_rel_rx_pt_lkup - release RX ptype lookup table
 * @adapter: adapter pointer to get the lookup table
 */
static void idpf_rel_rx_pt_lkup(struct idpf_adapter *adapter)
{
	kfree(adapter->splitq_pt_lkup);
	adapter->splitq_pt_lkup = NULL;

	kfree(adapter->singleq_pt_lkup);
	adapter->singleq_pt_lkup = NULL;
}

/**
 * idpf_send_ena_dis_loopback_msg - Send virtchnl enable/disable loopback
 *				    message
 * @adapter: adapter pointer used to send virtchnl message
 * @vport_id: vport identifier used while preparing the virtchnl message
 * @loopback_ena: flag to enable or disable loopback
 *
 * Returns 0 on success, negative on failure.
 */
int idpf_send_ena_dis_loopback_msg(struct idpf_adapter *adapter, u32 vport_id,
				   bool loopback_ena)
{
	struct libie_ctlq_xn_send_params xn_params = {
		.timeout_ms	= IDPF_VC_XN_DEFAULT_TIMEOUT_MSEC,
		.chnl_opcode	= VIRTCHNL2_OP_LOOPBACK,
	};
	struct virtchnl2_loopback loopback;
	int err;

	loopback.vport_id = cpu_to_le32(vport_id);
	loopback.enable = loopback_ena;

	err = idpf_send_mb_msg(adapter, &xn_params, &loopback,
			       sizeof(loopback));
	if (err)
		return err;

	libie_ctlq_release_rx_buf(&xn_params.recv_mem);

	return 0;
}

/**
 * idpf_init_dflt_mbx - Setup default mailbox parameters and make request
 * @adapter: adapter info struct
 *
 * Returns 0 on success, negative otherwise
 */
int idpf_init_dflt_mbx(struct idpf_adapter *adapter)
{
	struct libie_ctlq_ctx *ctx = &adapter->ctlq_ctx;
	struct libie_ctlq_create_info ctlq_info[] = {
		{
			.type = LIBIE_CTLQ_TYPE_TX,
			.id = LIBIE_CTLQ_MBX_ID,
			.len = IDPF_DFLT_MBX_Q_LEN,
		},
		{
			.type = LIBIE_CTLQ_TYPE_RX,
			.id = LIBIE_CTLQ_MBX_ID,
			.len = IDPF_DFLT_MBX_Q_LEN,
		}
	};
	struct libie_ctlq_xn_init_params params = {
		.num_qs = IDPF_NUM_DFLT_MBX_Q,
		.cctlq_info = ctlq_info,
		.ctx = ctx,
	};
	int err;

	adapter->dev_ops.reg_ops.ctlq_reg_init(&ctx->mmio_info,
					       params.cctlq_info);

	err = libie_ctlq_xn_init(&params);
	if (err)
		return err;

	adapter->asq = libie_find_ctlq(ctx, LIBIE_CTLQ_TYPE_TX,
				       LIBIE_CTLQ_MBX_ID);
	adapter->arq = libie_find_ctlq(ctx, LIBIE_CTLQ_TYPE_RX,
				       LIBIE_CTLQ_MBX_ID);
	if (!adapter->asq || !adapter->arq) {
		libie_ctlq_xn_deinit(params.xnm, ctx);
		return -ENOENT;
	}

	adapter->xn_init_params.xnm = params.xnm;
	adapter->state = __IDPF_VER_CHECK;

	queue_delayed_work(adapter->mbx_wq, &adapter->mbx_task, 0);

	return 0;
}

/**
 * idpf_deinit_dflt_mbx - Free up ctlqs setup
 * @adapter: Driver specific private data structure
 */
void idpf_deinit_dflt_mbx(struct idpf_adapter *adapter)
{
	idpf_mb_intr_rel_irq(adapter);
	cancel_delayed_work_sync(&adapter->mbx_task);

	if (adapter->arq && adapter->asq) {
		idpf_mb_clean(adapter, adapter->asq);
		libie_ctlq_xn_deinit(adapter->xn_init_params.xnm,
				     &adapter->ctlq_ctx);
	}
	adapter->arq = NULL;
	adapter->asq = NULL;
}

/**
 * idpf_vport_params_buf_rel - Release memory for MailBox resources
 * @adapter: Driver specific private data structure
 *
 * Will release memory to hold the vport parameters received on MailBox
 */
static void idpf_vport_params_buf_rel(struct idpf_adapter *adapter)
{
	kfree(adapter->vport_params_recvd);
	adapter->vport_params_recvd = NULL;
	kfree(adapter->vport_ids);
	adapter->vport_ids = NULL;
}

/**
 * idpf_vport_params_buf_alloc - Allocate memory for MailBox resources
 * @adapter: Driver specific private data structure
 *
 * Will alloc memory to hold the vport parameters received on MailBox
 */
static int idpf_vport_params_buf_alloc(struct idpf_adapter *adapter)
{
	u16 num_max_vports = idpf_get_max_vports(adapter);

	adapter->vport_params_recvd = kcalloc(num_max_vports,
					      sizeof(*adapter->vport_params_recvd),
					      GFP_KERNEL);
	if (!adapter->vport_params_recvd)
		return -ENOMEM;

	adapter->vport_ids = kcalloc(num_max_vports, sizeof(u32), GFP_KERNEL);
	if (!adapter->vport_ids)
		goto err_mem;

	if (adapter->vport_config)
		return 0;

	adapter->vport_config = kcalloc(num_max_vports,
					sizeof(*adapter->vport_config),
					GFP_KERNEL);
	if (!adapter->vport_config)
		goto err_mem;

	return 0;

err_mem:
	idpf_vport_params_buf_rel(adapter);

	return -ENOMEM;
}

/**
 * idpf_vc_core_init - Initialize state machine and get driver specific
 * resources
 * @adapter: Driver specific private structure
 *
 * This function will initialize the state machine and request all necessary
 * resources required by the device driver. Once the state machine is
 * initialized, allocate memory to store vport specific information and also
 * requests required interrupts.
 *
 * Returns 0 on success, -EAGAIN function will get called again,
 * otherwise negative on failure.
 */
int idpf_vc_core_init(struct idpf_adapter *adapter)
{
	int task_delay = 30;
	u16 num_max_vports;
	int err = 0;

	while (adapter->state != __IDPF_INIT_SW) {
		switch (adapter->state) {
		case __IDPF_VER_CHECK:
			err = idpf_send_ver_msg(adapter);
			switch (err) {
			case 0:
				/* success, move state machine forward */
				adapter->state = __IDPF_GET_CAPS;
				fallthrough;
			case -EAGAIN:
				goto restart;
			default:
				/* Something bad happened, try again but only a
				 * few times.
				 */
				goto init_failed;
			}
		case __IDPF_GET_CAPS:
			err = idpf_send_get_caps_msg(adapter);
			if (err)
				goto init_failed;
			adapter->state = __IDPF_INIT_SW;
			break;
		default:
			dev_err(&adapter->pdev->dev, "Device is in bad state: %d\n",
				adapter->state);
			err = -EINVAL;
			goto init_failed;
		}
		break;
restart:
		/* Give enough time before proceeding further with
		 * state machine
		 */
		msleep(task_delay);
	}

	pci_sriov_set_totalvfs(adapter->pdev, idpf_get_max_vfs(adapter));
	num_max_vports = idpf_get_max_vports(adapter);
	adapter->max_vports = num_max_vports;
	adapter->vports = kcalloc(num_max_vports, sizeof(*adapter->vports),
				  GFP_KERNEL);
	if (!adapter->vports)
		return -ENOMEM;

	if (!adapter->netdevs) {
		adapter->netdevs = kcalloc(num_max_vports,
					   sizeof(struct net_device *),
					   GFP_KERNEL);
		if (!adapter->netdevs) {
			err = -ENOMEM;
			goto err_netdev_alloc;
		}
	}

	err = idpf_vport_params_buf_alloc(adapter);
	if (err) {
		dev_err(&adapter->pdev->dev, "Failed to alloc vport params buffer: %d\n",
			err);
		goto err_netdev_alloc;
	}

	/* Start the mailbox task before requesting vectors. This will ensure
	 * vector information response from mailbox is handled
	 */
	queue_delayed_work(adapter->mbx_wq, &adapter->mbx_task, 0);

	queue_delayed_work(adapter->serv_wq, &adapter->serv_task,
			   msecs_to_jiffies(5 * (adapter->pdev->devfn & 0x07)));

	err = idpf_intr_req(adapter);
	if (err) {
		dev_err(&adapter->pdev->dev, "failed to enable interrupt vectors: %d\n",
			err);
		goto err_intr_req;
	}

	err = idpf_send_get_rx_ptype_msg(adapter);
	if (err) {
		dev_err(&adapter->pdev->dev, "failed to get RX ptypes: %d\n",
			err);
		goto intr_rel;
	}

	err = idpf_ptp_init(adapter);
	if (err)
		pci_err(adapter->pdev, "PTP init failed, err=%pe\n",
			ERR_PTR(err));

	idpf_init_avail_queues(adapter);

	/* Skew the delay for init tasks for each function based on fn number
	 * to prevent every function from making the same call simultaneously.
	 */
	queue_delayed_work(adapter->init_wq, &adapter->init_task,
			   msecs_to_jiffies(5 * (adapter->pdev->devfn & 0x07)));

	set_bit(IDPF_VC_CORE_INIT, adapter->flags);

	return 0;

intr_rel:
	idpf_intr_rel(adapter);
err_intr_req:
	cancel_delayed_work_sync(&adapter->serv_task);
	cancel_delayed_work_sync(&adapter->mbx_task);
	idpf_vport_params_buf_rel(adapter);
err_netdev_alloc:
	kfree(adapter->vports);
	adapter->vports = NULL;
	return err;

init_failed:
	/* Don't retry if we're trying to go down, just bail. */
	if (test_bit(IDPF_REMOVE_IN_PROG, adapter->flags))
		return err;

	if (++adapter->mb_wait_count > IDPF_MB_MAX_ERR) {
		dev_err(&adapter->pdev->dev, "Failed to establish mailbox communications with hardware\n");

		return -EFAULT;
	}
	/* If it reached here, it is possible that mailbox queue initialization
	 * register writes might not have taken effect. Retry to initialize
	 * the mailbox again
	 */
	adapter->state = __IDPF_VER_CHECK;
	idpf_deinit_dflt_mbx(adapter);
	set_bit(IDPF_HR_DRV_LOAD, adapter->flags);
	queue_delayed_work(adapter->vc_event_wq, &adapter->vc_event_task,
			   msecs_to_jiffies(task_delay));

	return -EAGAIN;
}

/**
 * idpf_vc_core_deinit - Device deinit routine
 * @adapter: Driver specific private structure
 *
 */
void idpf_vc_core_deinit(struct idpf_adapter *adapter)
{
	bool remove_in_prog;

	if (!test_bit(IDPF_VC_CORE_INIT, adapter->flags))
		return;

	/* Avoid transaction timeouts when called during reset */
	remove_in_prog = test_bit(IDPF_REMOVE_IN_PROG, adapter->flags);
	if (!remove_in_prog)
		idpf_deinit_dflt_mbx(adapter);

	idpf_ptp_release(adapter);
	idpf_deinit_task(adapter);
	idpf_rel_rx_pt_lkup(adapter);
	idpf_intr_rel(adapter);

	if (remove_in_prog)
		idpf_deinit_dflt_mbx(adapter);

	cancel_delayed_work_sync(&adapter->serv_task);

	idpf_vport_params_buf_rel(adapter);

	kfree(adapter->vports);
	adapter->vports = NULL;

	clear_bit(IDPF_VC_CORE_INIT, adapter->flags);
}

/**
 * idpf_vport_alloc_vec_indexes - Get relative vector indexes
 * @vport: virtual port data struct
 * @rsrc: pointer to queue and vector resources
 *
 * This function requests the vector information required for the vport and
 * stores the vector indexes received from the 'global vector distribution'
 * in the vport's queue vectors array.
 *
 * Return 0 on success, error on failure
 */
int idpf_vport_alloc_vec_indexes(struct idpf_vport *vport,
				 struct idpf_q_vec_rsrc *rsrc)
{
	struct idpf_vector_info vec_info;
	int num_alloc_vecs;

	vec_info.num_curr_vecs = rsrc->num_q_vectors;
	vec_info.num_req_vecs = max(rsrc->num_txq, rsrc->num_rxq);
	vec_info.default_vport = vport->default_vport;
	vec_info.index = vport->idx;

	num_alloc_vecs = idpf_req_rel_vector_indexes(vport->adapter,
						     rsrc->q_vector_idxs,
						     &vec_info);
	if (num_alloc_vecs <= 0) {
		dev_err(&vport->adapter->pdev->dev, "Vector distribution failed: %d\n",
			num_alloc_vecs);
		return -EINVAL;
	}

	rsrc->num_q_vectors = num_alloc_vecs;

	return 0;
}

/**
 * idpf_vport_init - Initialize virtual port
 * @vport: virtual port to be initialized
 * @max_q: vport max queue info
 *
 * Will initialize vport with the info received through MB earlier
 *
 * Return: %0 on success, -%errno on failure.
 */
int idpf_vport_init(struct idpf_vport *vport, struct idpf_vport_max_q *max_q)
{
	struct idpf_q_vec_rsrc *rsrc = &vport->dflt_qv_rsrc;
	struct idpf_adapter *adapter = vport->adapter;
	struct virtchnl2_create_vport *vport_msg;
	struct idpf_vport_config *vport_config;
	u16 tx_itr[] = {2, 8, 64, 128, 256};
	u16 rx_itr[] = {2, 8, 32, 96, 128};
	struct idpf_rss_data *rss_data;
	u16 idx = vport->idx;
	int err;

	vport_config = adapter->vport_config[idx];
	rss_data = &vport_config->user_config.rss_data;
	vport_msg = adapter->vport_params_recvd[idx];

	err = idpf_vport_init_queue_reg_chunks(vport_config,
					       &vport_msg->chunks);
	if (err)
		return err;

	vport_config->max_q.max_txq = max_q->max_txq;
	vport_config->max_q.max_rxq = max_q->max_rxq;
	vport_config->max_q.max_complq = max_q->max_complq;
	vport_config->max_q.max_bufq = max_q->max_bufq;

	rsrc->txq_model = le16_to_cpu(vport_msg->txq_model);
	rsrc->rxq_model = le16_to_cpu(vport_msg->rxq_model);
	vport->vport_type = le16_to_cpu(vport_msg->vport_type);
	vport->vport_id = le32_to_cpu(vport_msg->vport_id);

	rss_data->rss_key_size = min_t(u16, NETDEV_RSS_KEY_LEN,
				       le16_to_cpu(vport_msg->rss_key_size));
	rss_data->rss_lut_size = le16_to_cpu(vport_msg->rss_lut_size);

	ether_addr_copy(vport->default_mac_addr, vport_msg->default_mac_addr);
	vport->max_mtu = le16_to_cpu(vport_msg->max_mtu) - LIBETH_RX_LL_LEN;

	/* Initialize Tx and Rx profiles for Dynamic Interrupt Moderation */
	memcpy(vport->rx_itr_profile, rx_itr, IDPF_DIM_PROFILE_SLOTS);
	memcpy(vport->tx_itr_profile, tx_itr, IDPF_DIM_PROFILE_SLOTS);

	idpf_vport_set_hsplit(vport, ETHTOOL_TCP_DATA_SPLIT_ENABLED);

	idpf_vport_init_num_qs(vport, vport_msg, rsrc);
	idpf_vport_calc_num_q_desc(vport, rsrc);
	idpf_vport_calc_num_q_groups(rsrc);
	idpf_vport_alloc_vec_indexes(vport, rsrc);

	vport->crc_enable = adapter->crc_enable;

	if (!(vport_msg->vport_flags &
	      cpu_to_le16(VIRTCHNL2_VPORT_UPLINK_PORT)))
		return 0;

	err = idpf_ptp_get_vport_tstamps_caps(vport);
	if (err) {
		pci_dbg(vport->adapter->pdev, "Tx timestamping not supported\n");
		return err == -EOPNOTSUPP ? 0 : err;
	}

	INIT_WORK(&vport->tstamp_task, idpf_tstamp_task);

	return 0;
}

/**
 * idpf_get_vec_ids - Initialize vector id from Mailbox parameters
 * @adapter: adapter structure to get the mailbox vector id
 * @vecids: Array of vector ids
 * @num_vecids: number of vector ids
 * @chunks: vector ids received over mailbox
 *
 * Will initialize the mailbox vector id which is received from the
 * get capabilities and data queue vector ids with ids received as
 * mailbox parameters.
 * Returns number of ids filled
 */
int idpf_get_vec_ids(struct idpf_adapter *adapter,
		     u16 *vecids, int num_vecids,
		     struct virtchnl2_vector_chunks *chunks)
{
	u16 num_chunks = le16_to_cpu(chunks->num_vchunks);
	int num_vecid_filled = 0;
	int i, j;

	vecids[num_vecid_filled] = adapter->mb_vector.v_idx;
	num_vecid_filled++;

	for (j = 0; j < num_chunks; j++) {
		struct virtchnl2_vector_chunk *chunk;
		u16 start_vecid, num_vec;

		chunk = &chunks->vchunks[j];
		num_vec = le16_to_cpu(chunk->num_vectors);
		start_vecid = le16_to_cpu(chunk->start_vector_id);

		for (i = 0; i < num_vec; i++) {
			if ((num_vecid_filled + i) < num_vecids) {
				vecids[num_vecid_filled + i] = start_vecid;
				start_vecid++;
			} else {
				break;
			}
		}
		num_vecid_filled = num_vecid_filled + i;
	}

	return num_vecid_filled;
}

/**
 * idpf_vport_get_queue_ids - Initialize queue id from Mailbox parameters
 * @qids: Array of queue ids
 * @num_qids: number of queue ids
 * @q_type: queue model
 * @chunks: queue ids received over mailbox
 *
 * Will initialize all queue ids with ids received as mailbox parameters
 * Returns number of ids filled
 */
static int idpf_vport_get_queue_ids(u32 *qids, int num_qids, u16 q_type,
				    struct idpf_queue_id_reg_info *chunks)
{
	u16 num_chunks = chunks->num_chunks;
	u32 num_q_id_filled = 0, i;
	u32 start_q_id, num_q;

	while (num_chunks--) {
		struct idpf_queue_id_reg_chunk *chunk;

		chunk = &chunks->queue_chunks[num_chunks];
		if (chunk->type != q_type)
			continue;

		num_q = chunk->num_queues;
		start_q_id = chunk->start_queue_id;

		for (i = 0; i < num_q; i++) {
			if ((num_q_id_filled + i) < num_qids) {
				qids[num_q_id_filled + i] = start_q_id;
				start_q_id++;
			} else {
				break;
			}
		}
		num_q_id_filled = num_q_id_filled + i;
	}

	return num_q_id_filled;
}

/**
 * __idpf_vport_queue_ids_init - Initialize queue ids from Mailbox parameters
 * @vport: virtual port for which the queues ids are initialized
 * @rsrc: pointer to queue and vector resources
 * @qids: queue ids
 * @num_qids: number of queue ids
 * @q_type: type of queue
 *
 * Will initialize all queue ids with ids received as mailbox
 * parameters. Returns number of queue ids initialized.
 */
static int __idpf_vport_queue_ids_init(struct idpf_vport *vport,
				       struct idpf_q_vec_rsrc *rsrc,
				       const u32 *qids,
				       int num_qids,
				       u32 q_type)
{
	int i, j, k = 0;

	switch (q_type) {
	case VIRTCHNL2_QUEUE_TYPE_TX:
		for (i = 0; i < rsrc->num_txq_grp; i++) {
			struct idpf_txq_group *tx_qgrp = &rsrc->txq_grps[i];

			for (j = 0; j < tx_qgrp->num_txq && k < num_qids; j++, k++)
				tx_qgrp->txqs[j]->q_id = qids[k];
		}
		break;
	case VIRTCHNL2_QUEUE_TYPE_RX:
		for (i = 0; i < rsrc->num_rxq_grp; i++) {
			struct idpf_rxq_group *rx_qgrp = &rsrc->rxq_grps[i];
			u16 num_rxq;

			if (idpf_is_queue_model_split(rsrc->rxq_model))
				num_rxq = rx_qgrp->splitq.num_rxq_sets;
			else
				num_rxq = rx_qgrp->singleq.num_rxq;

			for (j = 0; j < num_rxq && k < num_qids; j++, k++) {
				struct idpf_rx_queue *q;

				if (idpf_is_queue_model_split(rsrc->rxq_model))
					q = &rx_qgrp->splitq.rxq_sets[j]->rxq;
				else
					q = rx_qgrp->singleq.rxqs[j];
				q->q_id = qids[k];
			}
		}
		break;
	case VIRTCHNL2_QUEUE_TYPE_TX_COMPLETION:
		for (i = 0; i < rsrc->num_txq_grp && k < num_qids; i++, k++) {
			struct idpf_txq_group *tx_qgrp = &rsrc->txq_grps[i];

			tx_qgrp->complq->q_id = qids[k];
		}
		break;
	case VIRTCHNL2_QUEUE_TYPE_RX_BUFFER:
		for (i = 0; i < rsrc->num_rxq_grp; i++) {
			struct idpf_rxq_group *rx_qgrp = &rsrc->rxq_grps[i];
			u8 num_bufqs = rsrc->num_bufqs_per_qgrp;

			for (j = 0; j < num_bufqs && k < num_qids; j++, k++) {
				struct idpf_buf_queue *q;

				q = &rx_qgrp->splitq.bufq_sets[j].bufq;
				q->q_id = qids[k];
			}
		}
		break;
	default:
		break;
	}

	return k;
}

/**
 * idpf_vport_queue_ids_init - Initialize queue ids from Mailbox parameters
 * @vport: virtual port for which the queues ids are initialized
 * @rsrc: pointer to queue and vector resources
 * @chunks: queue ids received over mailbox
 *
 * Will initialize all queue ids with ids received as mailbox parameters.
 * Returns 0 on success, negative if all the queues are not initialized.
 */
int idpf_vport_queue_ids_init(struct idpf_vport *vport,
			      struct idpf_q_vec_rsrc *rsrc,
			      struct idpf_queue_id_reg_info *chunks)
{
	int num_ids, err = 0;
	u16 q_type;
	u32 *qids;

	qids = kcalloc(IDPF_MAX_QIDS, sizeof(u32), GFP_KERNEL);
	if (!qids)
		return -ENOMEM;

	num_ids = idpf_vport_get_queue_ids(qids, IDPF_MAX_QIDS,
					   VIRTCHNL2_QUEUE_TYPE_TX,
					   chunks);
	if (num_ids < rsrc->num_txq) {
		err = -EINVAL;
		goto mem_rel;
	}
	num_ids = __idpf_vport_queue_ids_init(vport, rsrc, qids, num_ids,
					      VIRTCHNL2_QUEUE_TYPE_TX);
	if (num_ids < rsrc->num_txq) {
		err = -EINVAL;
		goto mem_rel;
	}

	num_ids = idpf_vport_get_queue_ids(qids, IDPF_MAX_QIDS,
					   VIRTCHNL2_QUEUE_TYPE_RX,
					   chunks);
	if (num_ids < rsrc->num_rxq) {
		err = -EINVAL;
		goto mem_rel;
	}
	num_ids = __idpf_vport_queue_ids_init(vport, rsrc, qids, num_ids,
					      VIRTCHNL2_QUEUE_TYPE_RX);
	if (num_ids < rsrc->num_rxq) {
		err = -EINVAL;
		goto mem_rel;
	}

	if (!idpf_is_queue_model_split(rsrc->txq_model))
		goto check_rxq;

	q_type = VIRTCHNL2_QUEUE_TYPE_TX_COMPLETION;
	num_ids = idpf_vport_get_queue_ids(qids, IDPF_MAX_QIDS, q_type, chunks);
	if (num_ids < rsrc->num_complq) {
		err = -EINVAL;
		goto mem_rel;
	}
	num_ids = __idpf_vport_queue_ids_init(vport, rsrc, qids,
					      num_ids, q_type);
	if (num_ids < rsrc->num_complq) {
		err = -EINVAL;
		goto mem_rel;
	}

check_rxq:
	if (!idpf_is_queue_model_split(rsrc->rxq_model))
		goto mem_rel;

	q_type = VIRTCHNL2_QUEUE_TYPE_RX_BUFFER;
	num_ids = idpf_vport_get_queue_ids(qids, IDPF_MAX_QIDS, q_type, chunks);
	if (num_ids < rsrc->num_bufq) {
		err = -EINVAL;
		goto mem_rel;
	}
	num_ids = __idpf_vport_queue_ids_init(vport, rsrc, qids,
					      num_ids, q_type);
	if (num_ids < rsrc->num_bufq)
		err = -EINVAL;

mem_rel:
	kfree(qids);

	return err;
}

/**
 * idpf_vport_adjust_qs - Adjust to new requested queues
 * @vport: virtual port data struct
 * @rsrc: pointer to queue and vector resources
 *
 * Renegotiate queues.  Returns 0 on success, negative on failure.
 */
int idpf_vport_adjust_qs(struct idpf_vport *vport, struct idpf_q_vec_rsrc *rsrc)
{
	struct virtchnl2_create_vport vport_msg;
	int err;

	vport_msg.txq_model = cpu_to_le16(rsrc->txq_model);
	vport_msg.rxq_model = cpu_to_le16(rsrc->rxq_model);
	err = idpf_vport_calc_total_qs(vport->adapter, vport->idx, &vport_msg,
				       NULL);
	if (err)
		return err;

	idpf_vport_init_num_qs(vport, &vport_msg, rsrc);
	idpf_vport_calc_num_q_groups(rsrc);

	return 0;
}

/**
 * idpf_is_capability_ena - Default implementation of capability checking
 * @adapter: Private data struct
 * @all: all or one flag
 * @field: caps field to check for flags
 * @flag: flag to check
 *
 * Return true if all capabilities are supported, false otherwise
 */
bool idpf_is_capability_ena(struct idpf_adapter *adapter, bool all,
			    enum idpf_cap_field field, u64 flag)
{
	u8 *caps = (u8 *)&adapter->caps;
	u32 *cap_field;

	if (!caps)
		return false;

	if (field == IDPF_BASE_CAPS)
		return false;

	cap_field = (u32 *)(caps + field);

	if (all)
		return (*cap_field & flag) == flag;
	else
		return !!(*cap_field & flag);
}

/**
 * idpf_vport_is_cap_ena - Check if vport capability is enabled
 * @vport: Private data struct
 * @flag: flag(s) to check
 *
 * Return: true if the capability is supported, false otherwise
 */
bool idpf_vport_is_cap_ena(struct idpf_vport *vport, u16 flag)
{
	struct virtchnl2_create_vport *vport_msg;

	vport_msg = vport->adapter->vport_params_recvd[vport->idx];

	return !!(le16_to_cpu(vport_msg->vport_flags) & flag);
}

/**
 * idpf_sideband_flow_type_ena - Check if steering is enabled for flow type
 * @vport: Private data struct
 * @flow_type: flow type to check (from ethtool.h)
 *
 * Return: true if sideband filters are allowed for @flow_type, false otherwise
 */
bool idpf_sideband_flow_type_ena(struct idpf_vport *vport, u32 flow_type)
{
	struct virtchnl2_create_vport *vport_msg;
	__le64 caps;

	vport_msg = vport->adapter->vport_params_recvd[vport->idx];
	caps = vport_msg->sideband_flow_caps;

	switch (flow_type) {
	case TCP_V4_FLOW:
		return !!(caps & cpu_to_le64(VIRTCHNL2_FLOW_IPV4_TCP));
	case UDP_V4_FLOW:
		return !!(caps & cpu_to_le64(VIRTCHNL2_FLOW_IPV4_UDP));
	default:
		return false;
	}
}

/**
 * idpf_sideband_action_ena - Check if steering is enabled for action
 * @vport: Private data struct
 * @fsp: flow spec
 *
 * Return: true if sideband filters are allowed for @fsp, false otherwise
 */
bool idpf_sideband_action_ena(struct idpf_vport *vport,
			      struct ethtool_rx_flow_spec *fsp)
{
	struct virtchnl2_create_vport *vport_msg;
	unsigned int supp_actions;

	vport_msg = vport->adapter->vport_params_recvd[vport->idx];
	supp_actions = le32_to_cpu(vport_msg->sideband_flow_actions);

	/* Actions Drop/Wake are not supported */
	if (fsp->ring_cookie == RX_CLS_FLOW_DISC ||
	    fsp->ring_cookie == RX_CLS_FLOW_WAKE)
		return false;

	return !!(supp_actions & VIRTCHNL2_ACTION_QUEUE);
}

unsigned int idpf_fsteer_max_rules(struct idpf_vport *vport)
{
	struct virtchnl2_create_vport *vport_msg;

	vport_msg = vport->adapter->vport_params_recvd[vport->idx];
	return le32_to_cpu(vport_msg->flow_steer_max_rules);
}

/**
 * idpf_get_vport_id: Get vport id
 * @vport: virtual port structure
 *
 * Return vport id from the adapter persistent data
 */
u32 idpf_get_vport_id(struct idpf_vport *vport)
{
	struct virtchnl2_create_vport *vport_msg;

	vport_msg = vport->adapter->vport_params_recvd[vport->idx];

	return le32_to_cpu(vport_msg->vport_id);
}

/**
 * idpf_mac_filter_async_handler - Async callback for mac filters
 * @ctx: controlq context structure
 * @buff: response buffer pointer and size
 * @status: async call return value
 *
 * In some scenarios driver can't sleep and wait for a reply (e.g.: stack is
 * holding rtnl_lock) when adding a new mac filter. It puts us in a difficult
 * situation to deal with errors returned on the reply. The best we can
 * ultimately do is remove it from our list of mac filters and report the
 * error.
 */
static void idpf_mac_filter_async_handler(void *ctx,
					  struct kvec *buff,
					  int status)
{
	struct virtchnl2_mac_addr_list *ma_list;
	struct idpf_vport_config *vport_config;
	struct virtchnl2_mac_addr *mac_addr;
	struct idpf_adapter *adapter = ctx;
	struct idpf_mac_filter *f, *tmp;
	struct list_head *ma_list_head;
	struct idpf_vport *vport;
	u16 num_entries;
	int i;

	/* if success we're done, we're only here if something bad happened */
	if (!status)
		goto free_mem;

	ma_list = buff->iov_base;
	/* make sure at least struct is there */
	if (buff->iov_len < sizeof(*ma_list))
		goto invalid_payload;

	mac_addr = ma_list->mac_addr_list;
	num_entries = le16_to_cpu(ma_list->num_mac_addr);
	/* we should have received a buffer at least this big */
	if (buff->iov_len < struct_size(ma_list, mac_addr_list, num_entries))
		goto invalid_payload;

	vport = idpf_vid_to_vport(adapter, le32_to_cpu(ma_list->vport_id));
	if (!vport)
		goto invalid_payload;

	vport_config = adapter->vport_config[le32_to_cpu(ma_list->vport_id)];
	ma_list_head = &vport_config->user_config.mac_filter_list;

	/* We can't do much to reconcile bad filters at this point, however we
	 * should at least remove them from our list one way or the other so we
	 * have some idea what good filters we have.
	 */
	spin_lock_bh(&vport_config->mac_filter_list_lock);
	list_for_each_entry_safe(f, tmp, ma_list_head, list)
		for (i = 0; i < num_entries; i++)
			if (ether_addr_equal(mac_addr[i].addr, f->macaddr))
				list_del(&f->list);
	spin_unlock_bh(&vport_config->mac_filter_list_lock);
	dev_err_ratelimited(&adapter->pdev->dev, "Received error %d on sending MAC filter request\n",
			    status);
	goto free_mem;

invalid_payload:
	dev_err_ratelimited(&adapter->pdev->dev, "Received invalid MAC filter payload (len %zd)\n",
			    buff->iov_len);
free_mem:
	libie_ctlq_release_rx_buf(buff);
}

/**
 * idpf_add_del_mac_filters - Add/del mac filters
 * @adapter: adapter pointer used to send virtchnl message
 * @vport_config: persistent vport structure to get the MAC filter list
 * @vport_id: vport identifier used while preparing the virtchnl message
 * @add: Add or delete flag
 * @async: Don't wait for return message
 *
 * Returns 0 on success, error on failure.
 **/
int idpf_add_del_mac_filters(struct idpf_adapter *adapter,
			     struct idpf_vport_config *vport_config,
			     u32 vport_id, bool add, bool async)
{
	struct libie_ctlq_xn_send_params xn_params = {
		.timeout_ms	= IDPF_VC_XN_DEFAULT_TIMEOUT_MSEC,
		.chnl_opcode	= add ? VIRTCHNL2_OP_ADD_MAC_ADDR :
					VIRTCHNL2_OP_DEL_MAC_ADDR,
	};
	struct virtchnl2_mac_addr *mac_addr __free(kfree) = NULL;
	struct virtchnl2_mac_addr_list *ma_list;
	u32 num_msgs, total_filters = 0;
	struct idpf_mac_filter *f;
	int i = 0, k, err = 0;
	u32 buf_size;

	if (async) {
		xn_params.resp_cb = idpf_mac_filter_async_handler;
		xn_params.send_ctx = adapter;
	}

	spin_lock_bh(&vport_config->mac_filter_list_lock);

	/* Find the number of newly added filters */
	list_for_each_entry(f, &vport_config->user_config.mac_filter_list,
			    list) {
		if (add && f->add)
			total_filters++;
		else if (!add && f->remove)
			total_filters++;
	}

	if (!total_filters) {
		spin_unlock_bh(&vport_config->mac_filter_list_lock);

		return 0;
	}

	/* Fill all the new filters into virtchannel message */
	mac_addr = kcalloc(total_filters, sizeof(struct virtchnl2_mac_addr),
			   GFP_ATOMIC);
	if (!mac_addr) {
		spin_unlock_bh(&vport_config->mac_filter_list_lock);

		return -ENOMEM;
	}

	list_for_each_entry(f, &vport_config->user_config.mac_filter_list,
			    list) {
		if (add && f->add) {
			ether_addr_copy(mac_addr[i].addr, f->macaddr);
			i++;
			f->add = false;
			if (i == total_filters)
				break;
		}
		if (!add && f->remove) {
			ether_addr_copy(mac_addr[i].addr, f->macaddr);
			i++;
			f->remove = false;
			if (i == total_filters)
				break;
		}
	}

	spin_unlock_bh(&vport_config->mac_filter_list_lock);

	/* Chunk up the filters into multiple messages to avoid
	 * sending a control queue message buffer that is too large
	 */
	num_msgs = DIV_ROUND_UP(total_filters, IDPF_NUM_FILTERS_PER_MSG);

	for (i = 0, k = 0; i < num_msgs; i++) {
		u32 entries_size, num_entries;

		num_entries = min_t(u32, total_filters,
				    IDPF_NUM_FILTERS_PER_MSG);
		entries_size = sizeof(struct virtchnl2_mac_addr) * num_entries;
		buf_size = struct_size(ma_list, mac_addr_list, num_entries);

		ma_list = kzalloc(buf_size, GFP_KERNEL);
		if (!ma_list)
			return -ENOMEM;

		ma_list->vport_id = cpu_to_le32(vport_id);
		ma_list->num_mac_addr = cpu_to_le16(num_entries);
		memcpy(ma_list->mac_addr_list, &mac_addr[k], entries_size);

		err = idpf_send_mb_msg(adapter, &xn_params, ma_list, buf_size);
		if (err)
			goto free_tx_buf;

		if (!async)
			libie_ctlq_release_rx_buf(&xn_params.recv_mem);

		k += num_entries;
		total_filters -= num_entries;
	}

free_tx_buf:
	if (num_msgs && libie_cp_can_send_onstack(buf_size))
		kfree(ma_list);

	return err;
}

/**
 * idpf_promiscuous_async_handler - async callback for promiscuous mode
 * @ctx: controlq context structure
 * @buff: response buffer pointer and size
 * @status: async call return value
 *
 * Nobody is waiting for the promiscuous virtchnl message response. Print
 * an error message if something went wrong and return.
 */
static void idpf_promiscuous_async_handler(void *ctx,
					   struct kvec *buff,
					   int status)
{
	struct idpf_adapter *adapter = ctx;

	if (status)
		dev_err_ratelimited(&adapter->pdev->dev, "Failed to set promiscuous mode: %d\n",
				    status);

	libie_ctlq_release_rx_buf(buff);
}

/**
 * idpf_set_promiscuous - set promiscuous and send message to mailbox
 * @adapter: Driver specific private structure
 * @config_data: Vport specific config data
 * @vport_id: Vport identifier
 *
 * Request to enable promiscuous mode for the vport. Message is sent
 * asynchronously and won't wait for response.  Returns 0 on success, negative
 * on failure;
 */
int idpf_set_promiscuous(struct idpf_adapter *adapter,
			 struct idpf_vport_user_config_data *config_data,
			 u32 vport_id)
{
	struct libie_ctlq_xn_send_params xn_params = {
		.timeout_ms	= IDPF_VC_XN_DEFAULT_TIMEOUT_MSEC,
		.chnl_opcode	= VIRTCHNL2_OP_CONFIG_PROMISCUOUS_MODE,
		.resp_cb	= idpf_promiscuous_async_handler,
		.send_ctx	= adapter,
	};
	struct virtchnl2_promisc_info vpi;
	u16 flags = 0;

	if (test_bit(__IDPF_PROMISC_UC, config_data->user_flags))
		flags |= VIRTCHNL2_UNICAST_PROMISC;
	if (test_bit(__IDPF_PROMISC_MC, config_data->user_flags))
		flags |= VIRTCHNL2_MULTICAST_PROMISC;

	vpi.vport_id = cpu_to_le32(vport_id);
	vpi.flags = cpu_to_le16(flags);

	return idpf_send_mb_msg(adapter, &xn_params, &vpi, sizeof(vpi));
}
