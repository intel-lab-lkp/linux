// SPDX-License-Identifier: GPL-2.0-only
/*
 * CAN bus driver for the Virtio CAN controller
 * Copyright (C) 2021-2023 OpenSynergy GmbH
 */

#include <linux/atomic.h>
#include <linux/idr.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/stddef.h>
#include <linux/can/dev.h>
#include <linux/virtio.h>
#include <linux/virtio_ring.h>
#include <linux/virtio_can.h>

/* CAN device queues */
#define VIRTIO_CAN_QUEUE_TX 0 /* Driver side view! The device receives here */
#define VIRTIO_CAN_QUEUE_RX 1 /* Driver side view! The device transmits here */
#define VIRTIO_CAN_QUEUE_CONTROL 2
#define VIRTIO_CAN_QUEUE_COUNT 3

#define CAN_KNOWN_FLAGS \
	(VIRTIO_CAN_FLAGS_EXTENDED |\
	 VIRTIO_CAN_FLAGS_FD |\
	 VIRTIO_CAN_FLAGS_RTR)

/* Max. number of in flight TX messages */
#define VIRTIO_CAN_ECHO_SKB_MAX 128

struct virtio_can_tx {
	struct list_head list;
	unsigned int putidx;
	struct virtio_can_tx_out tx_out;
	struct virtio_can_tx_in tx_in;
};

/* virtio_can private data structure */
struct virtio_can_priv {
	struct can_priv can;	/* must be the first member */
	/* NAPI for RX messages */
	struct napi_struct napi;
	/* NAPI for TX messages */
	struct napi_struct napi_tx;
	/* The network device we're associated with */
	struct net_device *dev;
	/* The virtio device we're associated with */
	struct virtio_device *vdev;
	/* The virtqueues */
	struct virtqueue *vqs[VIRTIO_CAN_QUEUE_COUNT];
	/* I/O callback function pointers for the virtqueues */
	vq_callback_t *io_callbacks[VIRTIO_CAN_QUEUE_COUNT];
	/* Lock for TX operations */
	spinlock_t tx_lock;
	/* Control queue lock. Defensive programming, may be not needed */
	struct mutex ctrl_lock;
	/* Wait for control queue processing without polling */
	struct completion ctrl_done;
	/* List of virtio CAN TX message */
	struct list_head tx_list;
	/* Array of receive queue messages */
	struct virtio_can_rx rpkt[128];
	/* Those control queue messages cannot live on the stack! */
	struct virtio_can_control_out cpkt_out;
	struct virtio_can_control_in cpkt_in;
	/* Data to get and maintain the putidx for local TX echo */
	struct ida tx_putidx_ida;
	/* In flight TX messages */
	atomic_t tx_inflight;
	/* BusOff pending. Reset after successful indication to upper layer */
	bool busoff_pending;
};

/* Function copied from virtio_net.c */
static void virtqueue_napi_schedule(struct napi_struct *napi,
				    struct virtqueue *vq)
{
	if (napi_schedule_prep(napi)) {
		virtqueue_disable_cb(vq);
		__napi_schedule(napi);
	}
}

/* Function copied from virtio_net.c */
static void virtqueue_napi_complete(struct napi_struct *napi,
				    struct virtqueue *vq, int processed)
{
	int opaque;

	opaque = virtqueue_enable_cb_prepare(vq);
	if (napi_complete_done(napi, processed)) {
		if (unlikely(virtqueue_poll(vq, opaque)))
			virtqueue_napi_schedule(napi, vq);
	} else {
		virtqueue_disable_cb(vq);
	}
}

static void virtio_can_free_candev(struct net_device *ndev)
{
	struct virtio_can_priv *priv = netdev_priv(ndev);

	ida_destroy(&priv->tx_putidx_ida);
	free_candev(ndev);
}

static int virtio_can_alloc_tx_idx(struct virtio_can_priv *priv)
{
	int tx_idx;

	tx_idx = ida_alloc_range(&priv->tx_putidx_ida, 0,
				 priv->can.echo_skb_max - 1, GFP_KERNEL);
	if (tx_idx >= 0)
		atomic_add(1, &priv->tx_inflight);

	return tx_idx;
}

static void virtio_can_free_tx_idx(struct virtio_can_priv *priv,
				   unsigned int idx)
{
	ida_free(&priv->tx_putidx_ida, idx);
	atomic_sub(1, &priv->tx_inflight);
}

/* Create a scatter-gather list representing our input buffer and put
 * it in the queue.
 *
 * Callers should take appropriate locks.
 */
static int virtio_can_add_inbuf(struct virtqueue *vq, void *buf,
				unsigned int size)
{
	struct scatterlist sg[1];
	int ret;

	sg_init_one(sg, buf, size);

	ret = virtqueue_add_inbuf(vq, sg, 1, buf, GFP_ATOMIC);
	virtqueue_kick(vq);
	if (ret == 0)
		ret = vq->num_free;
	return ret;
}

/* Send a control message with message type either
 *
 * - VIRTIO_CAN_SET_CTRL_MODE_START or
 * - VIRTIO_CAN_SET_CTRL_MODE_STOP.
 *
 * Unlike AUTOSAR CAN Driver Can_SetControllerMode() there is no requirement
 * for this Linux driver to have an asynchronous implementation of the mode
 * setting function so in order to keep things simple the function is
 * implemented as synchronous function. Design pattern is
 * virtio_console.c/__send_control_msg() & virtio_net.c/virtnet_send_command().
 */
static u8 virtio_can_send_ctrl_msg(struct net_device *ndev, u16 msg_type)
{
	struct scatterlist sg_out, sg_in, *sgs[2] = { &sg_out, &sg_in };
	struct virtio_can_priv *priv = netdev_priv(ndev);
	struct device *dev = &priv->vdev->dev;
	struct virtqueue *vq;
	unsigned int len;
	int err;

	vq = priv->vqs[VIRTIO_CAN_QUEUE_CONTROL];

	/* The function may be serialized by rtnl lock. Not sure.
	 * Better safe than sorry.
	 */
	mutex_lock(&priv->ctrl_lock);

	priv->cpkt_out.msg_type = cpu_to_le16(msg_type);
	sg_init_one(&sg_out, &priv->cpkt_out, sizeof(priv->cpkt_out));
	sg_init_one(&sg_in, &priv->cpkt_in, sizeof(priv->cpkt_in));

	err = virtqueue_add_sgs(vq, sgs, 1u, 1u, priv, GFP_ATOMIC);
	if (err != 0) {
		/* Not expected to happen */
		dev_err(dev, "%s(): virtqueue_add_sgs() failed\n", __func__);
	}

	if (!virtqueue_kick(vq)) {
		/* Not expected to happen */
		dev_err(dev, "%s(): Kick failed\n", __func__);
	}

	while (!virtqueue_get_buf(vq, &len) && !virtqueue_is_broken(vq))
		wait_for_completion(&priv->ctrl_done);

	mutex_unlock(&priv->ctrl_lock);

	return priv->cpkt_in.result;
}

static void virtio_can_start(struct net_device *ndev)
{
	struct virtio_can_priv *priv = netdev_priv(ndev);
	u8 result;

	result = virtio_can_send_ctrl_msg(ndev, VIRTIO_CAN_SET_CTRL_MODE_START);
	if (result != VIRTIO_CAN_RESULT_OK) {
		/* Not expected to happen */
		netdev_err(ndev, "CAN controller start failed\n");
	}

	priv->busoff_pending = false;
	priv->can.state = CAN_STATE_ERROR_ACTIVE;

	/* Switch carrier on if device was not connected to the bus */
	if (!netif_carrier_ok(ndev))
		netif_carrier_on(ndev);
}

/* See also m_can.c/m_can_set_mode()
 *
 * It is interesting that not only the M-CAN implementation but also all other
 * implementations I looked into only support CAN_MODE_START.
 * That CAN_MODE_SLEEP is frequently not found to be supported anywhere did not
 * come not as surprise but that CAN_MODE_STOP is also never supported was one.
 * The function is accessible via the method pointer do_set_mode in
 * struct can_priv. As usual no documentation there.
 * May not play any role as grepping through the code did not reveal any place
 * from where the method is actually called.
 */
static int virtio_can_set_mode(struct net_device *dev, enum can_mode mode)
{
	switch (mode) {
	case CAN_MODE_START:
		virtio_can_start(dev);
		netif_wake_queue(dev);
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

/* Called by issuing "ip link set up can0" */
static int virtio_can_open(struct net_device *dev)
{
	/* start the virtio_can controller */
	virtio_can_start(dev);

	/* RX and TX napi were already enabled in virtio_can_probe() */
	netif_start_queue(dev);

	return 0;
}

static void virtio_can_stop(struct net_device *ndev)
{
	struct virtio_can_priv *priv = netdev_priv(ndev);
	struct device *dev = &priv->vdev->dev;
	u8 result;

	result = virtio_can_send_ctrl_msg(ndev, VIRTIO_CAN_SET_CTRL_MODE_STOP);
	if (result != VIRTIO_CAN_RESULT_OK)
		dev_err(dev, "CAN controller stop failed\n");

	priv->busoff_pending = false;
	priv->can.state = CAN_STATE_STOPPED;

	/* Switch carrier off if device was connected to the bus */
	if (netif_carrier_ok(ndev))
		netif_carrier_off(ndev);
}

static int virtio_can_close(struct net_device *dev)
{
	netif_stop_queue(dev);
	/* Keep RX napi active to allow dropping of pending RX CAN messages,
	 * keep TX napi active to allow processing of cancelled CAN messages
	 */
	virtio_can_stop(dev);
	close_candev(dev);

	return 0;
}

static netdev_tx_t virtio_can_start_xmit(struct sk_buff *skb,
					 struct net_device *dev)
{
	const unsigned int hdr_size = offsetof(struct virtio_can_tx_out, sdu);
	struct scatterlist sg_out, sg_in, *sgs[2] = { &sg_out, &sg_in };
	struct canfd_frame *cf = (struct canfd_frame *)skb->data;
	struct virtio_can_priv *priv = netdev_priv(dev);
	netdev_tx_t xmit_ret = NETDEV_TX_OK;
	struct virtio_can_tx *can_tx_msg;
	struct virtqueue *vq;
	unsigned long flags;
	u32 can_flags;
	int putidx;
	int err;

	vq = priv->vqs[VIRTIO_CAN_QUEUE_TX];

	if (can_dev_dropped_skb(dev, skb))
		goto kick; /* No way to return NET_XMIT_DROP here */

	/* No local check for CAN_RTR_FLAG or FD frame against negotiated
	 * features. The device will reject those anyway if not supported.
	 */

	can_tx_msg = kzalloc(sizeof(*can_tx_msg), GFP_ATOMIC);
	if (!can_tx_msg) {
		dev->stats.tx_dropped++;
		goto kick; /* No way to return NET_XMIT_DROP here */
	}

	can_tx_msg->tx_out.msg_type = cpu_to_le16(VIRTIO_CAN_TX);
	can_flags = 0;

	if (cf->can_id & CAN_EFF_FLAG) {
		can_flags |= VIRTIO_CAN_FLAGS_EXTENDED;
		can_tx_msg->tx_out.can_id = cpu_to_le32(cf->can_id & CAN_EFF_MASK);
	} else {
		can_tx_msg->tx_out.can_id = cpu_to_le32(cf->can_id & CAN_SFF_MASK);
	}
	if (cf->can_id & CAN_RTR_FLAG)
		can_flags |= VIRTIO_CAN_FLAGS_RTR;
	else
		memcpy(can_tx_msg->tx_out.sdu, cf->data, cf->len);
	if (can_is_canfd_skb(skb))
		can_flags |= VIRTIO_CAN_FLAGS_FD;

	can_tx_msg->tx_out.flags = cpu_to_le32(can_flags);
	can_tx_msg->tx_out.length = cpu_to_le16(cf->len);

	/* Prepare sending of virtio message */
	sg_init_one(&sg_out, &can_tx_msg->tx_out, hdr_size + cf->len);
	sg_init_one(&sg_in, &can_tx_msg->tx_in, sizeof(can_tx_msg->tx_in));

	putidx = virtio_can_alloc_tx_idx(priv);

	if (unlikely(putidx < 0)) {
		/* -ENOMEM or -ENOSPC here. -ENOSPC should not be possible as
		 * tx_inflight >= can.echo_skb_max is checked in flow control
		 */
		WARN_ON_ONCE(putidx == -ENOSPC);
		kfree(can_tx_msg);
		dev->stats.tx_dropped++;
		goto kick; /* No way to return NET_XMIT_DROP here */
	}

	can_tx_msg->putidx = (unsigned int)putidx;

	/* Protect list operation */
	spin_lock_irqsave(&priv->tx_lock, flags);
	list_add_tail(&can_tx_msg->list, &priv->tx_list);
	spin_unlock_irqrestore(&priv->tx_lock, flags);

	/* Push loopback echo. Will be looped back on TX interrupt/TX NAPI */
	can_put_echo_skb(skb, dev, can_tx_msg->putidx, 0);

	/* Protect queue and list operations */
	spin_lock_irqsave(&priv->tx_lock, flags);
	err = virtqueue_add_sgs(vq, sgs, 1u, 1u, can_tx_msg, GFP_ATOMIC);
	if (unlikely(err)) { /* checking vq->num_free in flow control */
		list_del(&can_tx_msg->list);
		can_free_echo_skb(dev, can_tx_msg->putidx, NULL);
		virtio_can_free_tx_idx(priv, can_tx_msg->putidx);
		spin_unlock_irqrestore(&priv->tx_lock, flags);
		netif_stop_queue(dev);
		kfree(can_tx_msg);
		/* Expected never to be seen */
		netdev_warn(dev, "TX: Stop queue, err = %d\n", err);
		xmit_ret = NETDEV_TX_BUSY;
		goto kick;
	}

	/* Normal flow control: stop queue when no transmission slots left */
	if (atomic_read(&priv->tx_inflight) >= priv->can.echo_skb_max ||
	    vq->num_free == 0 || (vq->num_free < ARRAY_SIZE(sgs) &&
	    !virtio_has_feature(vq->vdev, VIRTIO_RING_F_INDIRECT_DESC))) {
		netif_stop_queue(dev);
		netdev_dbg(dev, "TX: Normal stop queue\n");
	}

	spin_unlock_irqrestore(&priv->tx_lock, flags);

kick:
	if (netif_queue_stopped(dev) || !netdev_xmit_more()) {
		if (!virtqueue_kick(vq))
			netdev_err(dev, "%s(): Kick failed\n", __func__);
	}

	return xmit_ret;
}

static const struct net_device_ops virtio_can_netdev_ops = {
	.ndo_open = virtio_can_open,
	.ndo_stop = virtio_can_close,
	.ndo_start_xmit = virtio_can_start_xmit,
	.ndo_change_mtu = can_change_mtu,
};

static int register_virtio_can_dev(struct net_device *dev)
{
	dev->flags |= IFF_ECHO;	/* we support local echo */
	dev->netdev_ops = &virtio_can_netdev_ops;

	return register_candev(dev);
}

/* Compare with m_can.c/m_can_echo_tx_event() */
static int virtio_can_read_tx_queue(struct virtqueue *vq)
{
	struct virtio_can_priv *can_priv = vq->vdev->priv;
	struct net_device *dev = can_priv->dev;
	struct virtio_can_tx *can_tx_msg;
	struct net_device_stats *stats;
	unsigned long flags;
	unsigned int len;
	u8 result;

	stats = &dev->stats;

	/* Protect list and virtio queue operations */
	spin_lock_irqsave(&can_priv->tx_lock, flags);

	can_tx_msg = virtqueue_get_buf(vq, &len);
	if (!can_tx_msg) {
		spin_unlock_irqrestore(&can_priv->tx_lock, flags);
		return 0; /* No more data */
	}

	if (unlikely(len < sizeof(struct virtio_can_tx_in))) {
		netdev_err(dev, "TX ACK: Device sent no result code\n");
		result = VIRTIO_CAN_RESULT_NOT_OK; /* Keep things going */
	} else {
		result = can_tx_msg->tx_in.result;
	}

	if (can_priv->can.state < CAN_STATE_BUS_OFF) {
		/* Here also frames with result != VIRTIO_CAN_RESULT_OK are
		 * echoed. Intentional to bring a waiting process in an upper
		 * layer to an end.
		 * TODO: Any better means to indicate a problem here?
		 */
		if (result != VIRTIO_CAN_RESULT_OK)
			netdev_warn(dev, "TX ACK: Result = %u\n", result);

		stats->tx_bytes += can_get_echo_skb(dev, can_tx_msg->putidx,
						    NULL);
		stats->tx_packets++;
	} else {
		netdev_dbg(dev, "TX ACK: Controller inactive, drop echo\n");
		can_free_echo_skb(dev, can_tx_msg->putidx, NULL);
	}

	list_del(&can_tx_msg->list);
	virtio_can_free_tx_idx(can_priv, can_tx_msg->putidx);

	/* Flow control */
	if (netif_queue_stopped(dev)) {
		netdev_dbg(dev, "TX ACK: Wake up stopped queue\n");
		netif_wake_queue(dev);
	}

	spin_unlock_irqrestore(&can_priv->tx_lock, flags);

	kfree(can_tx_msg);

	return 1; /* Queue was not empty so there may be more data */
}

/* Poll TX used queue for sent CAN messages
 * See https://wiki.linuxfoundation.org/networking/napi function
 * int (*poll)(struct napi_struct *napi, int budget);
 */
static int virtio_can_tx_poll(struct napi_struct *napi, int quota)
{
	struct net_device *dev = napi->dev;
	struct virtio_can_priv *priv;
	struct virtqueue *vq;
	int work_done = 0;

	priv = netdev_priv(dev);
	vq = priv->vqs[VIRTIO_CAN_QUEUE_TX];

	while (work_done < quota && virtio_can_read_tx_queue(vq) != 0)
		work_done++;

	if (work_done < quota)
		virtqueue_napi_complete(napi, vq, work_done);

	return work_done;
}

static void virtio_can_tx_intr(struct virtqueue *vq)
{
	struct virtio_can_priv *can_priv = vq->vdev->priv;

	virtqueue_disable_cb(vq);
	napi_schedule(&can_priv->napi_tx);
}

/* This function is the NAPI RX poll function and NAPI guarantees that this
 * function is not invoked simultaneously on multiple processors.
 * Read a RX message from the used queue and sends it to the upper layer.
 * (See also m_can.c / m_can_read_fifo()).
 */
static int virtio_can_read_rx_queue(struct virtqueue *vq)
{
	const unsigned int header_size = offsetof(struct virtio_can_rx, sdu);
	struct virtio_can_priv *priv = vq->vdev->priv;
	struct net_device *dev = priv->dev;
	struct net_device_stats *stats;
	struct virtio_can_rx *can_rx;
	unsigned int transport_len;
	struct canfd_frame *cf;
	struct sk_buff *skb;
	unsigned int len;
	u32 can_flags;
	u16 msg_type;
	u32 can_id;

	stats = &dev->stats;

	can_rx = virtqueue_get_buf(vq, &transport_len);
	if (!can_rx)
		return 0; /* No more data */

	if (transport_len < header_size) {
		netdev_warn(dev, "RX: Message too small\n");
		goto putback;
	}

	if (priv->can.state >= CAN_STATE_ERROR_PASSIVE) {
		netdev_dbg(dev, "%s(): Controller not active\n", __func__);
		goto putback;
	}

	msg_type = le16_to_cpu(can_rx->msg_type);
	if (msg_type != VIRTIO_CAN_RX) {
		netdev_warn(dev, "RX: Got unknown msg_type %04x\n", msg_type);
		goto putback;
	}

	len = le16_to_cpu(can_rx->length);
	can_flags = le32_to_cpu(can_rx->flags);
	can_id = le32_to_cpu(can_rx->can_id);

	if (can_flags & ~CAN_KNOWN_FLAGS) {
		stats->rx_dropped++;
		netdev_warn(dev, "RX: CAN Id 0x%08x: Invalid flags 0x%x\n",
			    can_id, can_flags);
		goto putback;
	}

	if (can_flags & VIRTIO_CAN_FLAGS_EXTENDED) {
		can_id &= CAN_EFF_MASK;
		can_id |= CAN_EFF_FLAG;
	} else {
		can_id &= CAN_SFF_MASK;
	}

	if (can_flags & VIRTIO_CAN_FLAGS_RTR) {
		if (!virtio_has_feature(vq->vdev, VIRTIO_CAN_F_RTR_FRAMES)) {
			stats->rx_dropped++;
			netdev_warn(dev, "RX: CAN Id 0x%08x: RTR not negotiated\n",
				    can_id);
			goto putback;
		}
		if (can_flags & VIRTIO_CAN_FLAGS_FD) {
			stats->rx_dropped++;
			netdev_warn(dev, "RX: CAN Id 0x%08x: RTR with FD not possible\n",
				    can_id);
			goto putback;
		}

		if (len > 0xF) {
			stats->rx_dropped++;
			netdev_warn(dev, "RX: CAN Id 0x%08x: RTR with DLC > 0xF\n",
				    can_id);
			goto putback;
		}

		if (len > 0x8)
			len = 0x8;

		can_id |= CAN_RTR_FLAG;
	}

	if (transport_len < header_size + len) {
		netdev_warn(dev, "RX: Message too small for payload\n");
		goto putback;
	}

	if (can_flags & VIRTIO_CAN_FLAGS_FD) {
		if (!virtio_has_feature(vq->vdev, VIRTIO_CAN_F_CAN_FD)) {
			stats->rx_dropped++;
			netdev_warn(dev, "RX: CAN Id 0x%08x: FD not negotiated\n",
				    can_id);
			goto putback;
		}

		if (len > CANFD_MAX_DLEN)
			len = CANFD_MAX_DLEN;

		skb = alloc_canfd_skb(priv->dev, &cf);
	} else {
		if (!virtio_has_feature(vq->vdev, VIRTIO_CAN_F_CAN_CLASSIC)) {
			stats->rx_dropped++;
			netdev_warn(dev, "RX: CAN Id 0x%08x: classic not negotiated\n",
				    can_id);
			goto putback;
		}

		if (len > CAN_MAX_DLEN)
			len = CAN_MAX_DLEN;

		skb = alloc_can_skb(priv->dev, (struct can_frame **)&cf);
	}
	if (!skb) {
		stats->rx_dropped++;
		netdev_warn(dev, "RX: No skb available\n");
		goto putback;
	}

	cf->can_id = can_id;
	cf->len = len;
	if (!(can_flags & VIRTIO_CAN_FLAGS_RTR)) {
		/* RTR frames have a DLC but no payload */
		memcpy(cf->data, can_rx->sdu, len);
	}

	if (netif_receive_skb(skb) == NET_RX_SUCCESS) {
		stats->rx_packets++;
		if (!(can_flags & VIRTIO_CAN_FLAGS_RTR))
			stats->rx_bytes += cf->len;
	}

putback:
	/* Put processed RX buffer back into avail queue */
	virtio_can_add_inbuf(vq, can_rx, sizeof(struct virtio_can_rx));

	return 1; /* Queue was not empty so there may be more data */
}

/* See m_can_poll() / m_can_handle_state_errors() m_can_handle_state_change() */
static int virtio_can_handle_busoff(struct net_device *dev)
{
	struct virtio_can_priv *priv = netdev_priv(dev);
	struct can_frame *cf;
	struct sk_buff *skb;

	if (!priv->busoff_pending)
		return 0;

	if (priv->can.state < CAN_STATE_BUS_OFF) {
		netdev_dbg(dev, "entered error bus off state\n");

		/* bus-off state */
		priv->can.state = CAN_STATE_BUS_OFF;
		priv->can.can_stats.bus_off++;
		can_bus_off(dev);
	}

	/* propagate the error condition to the CAN stack */
	skb = alloc_can_err_skb(dev, &cf);
	if (unlikely(!skb))
		return 0;

	/* bus-off state */
	cf->can_id |= CAN_ERR_BUSOFF;

	/* Ensure that the BusOff indication does not get lost */
	if (netif_receive_skb(skb) == NET_RX_SUCCESS)
		priv->busoff_pending = false;

	return 1;
}

/* Poll RX used queue for received CAN messages
 * See https://wiki.linuxfoundation.org/networking/napi function
 * int (*poll)(struct napi_struct *napi, int budget);
 * Important: "The networking subsystem promises that poll() will not be
 * invoked simultaneously (for the same napi_struct) on multiple processors"
 */
static int virtio_can_rx_poll(struct napi_struct *napi, int quota)
{
	struct net_device *dev = napi->dev;
	struct virtio_can_priv *priv;
	struct virtqueue *vq;
	int work_done = 0;

	priv = netdev_priv(dev);
	vq = priv->vqs[VIRTIO_CAN_QUEUE_RX];

	work_done += virtio_can_handle_busoff(dev);

	while (work_done < quota && virtio_can_read_rx_queue(vq) != 0)
		work_done++;

	if (work_done < quota)
		virtqueue_napi_complete(napi, vq, work_done);

	return work_done;
}

static void virtio_can_rx_intr(struct virtqueue *vq)
{
	struct virtio_can_priv *can_priv = vq->vdev->priv;

	virtqueue_disable_cb(vq);
	napi_schedule(&can_priv->napi);
}

static void virtio_can_control_intr(struct virtqueue *vq)
{
	struct virtio_can_priv *can_priv = vq->vdev->priv;

	complete(&can_priv->ctrl_done);
}

static void virtio_can_config_changed(struct virtio_device *vdev)
{
	struct virtio_can_priv *can_priv = vdev->priv;
	u16 status;

	status = virtio_cread16(vdev, offsetof(struct virtio_can_config,
					       status));

	if (!(status & VIRTIO_CAN_S_CTRL_BUSOFF))
		return;

	if (!can_priv->busoff_pending &&
	    can_priv->can.state < CAN_STATE_BUS_OFF) {
		can_priv->busoff_pending = true;
		napi_schedule(&can_priv->napi);
	}
}

static void virtio_can_populate_vqs(struct virtio_device *vdev)

{
	struct virtio_can_priv *priv = vdev->priv;
	struct virtqueue *vq;
	unsigned int idx;
	int ret;

	/* Fill RX queue */
	vq = priv->vqs[VIRTIO_CAN_QUEUE_RX];
	for (idx = 0; idx < ARRAY_SIZE(priv->rpkt); idx++) {
		ret = virtio_can_add_inbuf(vq, &priv->rpkt[idx],
					   sizeof(struct virtio_can_rx));
		if (ret < 0) {
			dev_dbg(&vdev->dev, "rpkt fill: ret=%d, idx=%u\n",
				ret, idx);
			break;
		}
	}
	dev_dbg(&vdev->dev, "%u rpkt added\n", idx);
}

static int virtio_can_find_vqs(struct virtio_can_priv *priv)
{
	/* The order of RX and TX is exactly the opposite as in console and
	 * network. Does not play any role but is a bad trap.
	 */
	static const char * const io_names[VIRTIO_CAN_QUEUE_COUNT] = {
		"can-tx",
		"can-rx",
		"can-state-ctrl"
	};

	priv->io_callbacks[VIRTIO_CAN_QUEUE_TX] = virtio_can_tx_intr;
	priv->io_callbacks[VIRTIO_CAN_QUEUE_RX] = virtio_can_rx_intr;
	priv->io_callbacks[VIRTIO_CAN_QUEUE_CONTROL] = virtio_can_control_intr;

	/* Find the queues. */
	return virtio_find_vqs(priv->vdev, VIRTIO_CAN_QUEUE_COUNT, priv->vqs,
			       priv->io_callbacks, io_names, NULL);
}

/* Function must not be called before virtio_can_find_vqs() has been run */
static void virtio_can_del_vq(struct virtio_device *vdev)
{
	struct virtio_can_priv *priv = vdev->priv;
	struct list_head *cursor, *next;
	struct virtqueue *vq;

	/* Reset the device */
	if (vdev->config->reset)
		vdev->config->reset(vdev);

	/* From here we have dead silence from the device side so no locks
	 * are needed to protect against device side events.
	 */

	vq = priv->vqs[VIRTIO_CAN_QUEUE_CONTROL];
	while (virtqueue_detach_unused_buf(vq))
		; /* Do nothing, content allocated statically */

	vq = priv->vqs[VIRTIO_CAN_QUEUE_RX];
	while (virtqueue_detach_unused_buf(vq))
		; /* Do nothing, content allocated statically */

	vq = priv->vqs[VIRTIO_CAN_QUEUE_TX];
	while (virtqueue_detach_unused_buf(vq))
		; /* Do nothing, content to be de-allocated separately */

	/* Is keeping track of allocated elements by an own linked list
	 * really necessary or may this be optimized using only
	 * virtqueue_detach_unused_buf()?
	 */
	list_for_each_safe(cursor, next, &priv->tx_list) {
		struct virtio_can_tx *can_tx;

		can_tx = list_entry(cursor, struct virtio_can_tx, list);
		list_del(cursor);
		kfree(can_tx);
	}

	if (vdev->config->del_vqs)
		vdev->config->del_vqs(vdev);
}

/* See virtio_net.c/virtnet_remove() and also m_can.c/m_can_plat_remove() */
static void virtio_can_remove(struct virtio_device *vdev)
{
	struct virtio_can_priv *priv = vdev->priv;
	struct net_device *dev = priv->dev;

	unregister_candev(dev);

	/* No calls of netif_napi_del() needed as free_candev() will do this */

	virtio_can_del_vq(vdev);

	virtio_can_free_candev(dev);
}

static int virtio_can_validate(struct virtio_device *vdev)
{
	/* CAN needs always access to the config space.
	 * Check that the driver can access the config space
	 */
	if (!vdev->config->get) {
		dev_err(&vdev->dev, "%s failure: config access disabled\n",
			__func__);
		return -EINVAL;
	}

	if (!virtio_has_feature(vdev, VIRTIO_F_VERSION_1)) {
		dev_err(&vdev->dev,
			"device does not comply with spec version 1.x\n");
		return -EINVAL;
	}

	return 0;
}

static int virtio_can_probe(struct virtio_device *vdev)
{
	struct virtio_can_priv *priv;
	struct net_device *dev;
	int err;

	dev = alloc_candev(sizeof(struct virtio_can_priv),
			   VIRTIO_CAN_ECHO_SKB_MAX);
	if (!dev)
		return -ENOMEM;

	priv = netdev_priv(dev);

	ida_init(&priv->tx_putidx_ida);

	netif_napi_add(dev, &priv->napi, virtio_can_rx_poll);
	netif_napi_add(dev, &priv->napi_tx, virtio_can_tx_poll);

	SET_NETDEV_DEV(dev, &vdev->dev);

	priv->dev = dev;
	priv->vdev = vdev;
	vdev->priv = priv;

	priv->can.do_set_mode = virtio_can_set_mode;
	/* Set Virtio CAN supported operations */
	priv->can.ctrlmode_supported = CAN_CTRLMODE_BERR_REPORTING;
	if (virtio_has_feature(vdev, VIRTIO_CAN_F_CAN_FD)) {
		err = can_set_static_ctrlmode(dev, CAN_CTRLMODE_FD);
		if (err != 0)
			goto on_failure;
	}

	/* Initialize virtqueues */
	err = virtio_can_find_vqs(priv);
	if (err != 0)
		goto on_failure;

	INIT_LIST_HEAD(&priv->tx_list);

	spin_lock_init(&priv->tx_lock);
	mutex_init(&priv->ctrl_lock);

	init_completion(&priv->ctrl_done);

	virtio_can_populate_vqs(vdev);

	register_virtio_can_dev(dev);

	napi_enable(&priv->napi);
	napi_enable(&priv->napi_tx);

	/* Request device going live */
	virtio_device_ready(vdev); /* Optionally done by virtio_dev_probe() */

	return 0;

on_failure:
	virtio_can_free_candev(dev);
	return err;
}

/* Compare with m_can.c/m_can_suspend(), virtio_net.c/virtnet_freeze() and
 * virtio_card.c/virtsnd_freeze()
 */
static int __maybe_unused virtio_can_freeze(struct virtio_device *vdev)
{
	struct virtio_can_priv *priv = vdev->priv;
	struct net_device *ndev = priv->dev;

	napi_disable(&priv->napi);
	napi_disable(&priv->napi_tx);

	if (netif_running(ndev)) {
		netif_stop_queue(ndev);
		netif_device_detach(ndev);
		virtio_can_stop(ndev);
	}

	priv->can.state = CAN_STATE_SLEEPING;

	virtio_can_del_vq(vdev);

	return 0;
}

/* Compare with m_can.c/m_can_resume(), virtio_net.c/virtnet_restore() and
 * virtio_card.c/virtsnd_restore()
 */
static int __maybe_unused virtio_can_restore(struct virtio_device *vdev)
{
	struct virtio_can_priv *priv = vdev->priv;
	struct net_device *ndev = priv->dev;
	int err;

	err = virtio_can_find_vqs(priv);
	if (err != 0)
		return err;
	virtio_can_populate_vqs(vdev);

	priv->can.state = CAN_STATE_ERROR_ACTIVE;

	if (netif_running(ndev)) {
		virtio_can_start(ndev);
		netif_device_attach(ndev);
		netif_start_queue(ndev);
	}

	napi_enable(&priv->napi);
	napi_enable(&priv->napi_tx);

	return 0;
}

static struct virtio_device_id virtio_can_id_table[] = {
	{ VIRTIO_ID_CAN, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static unsigned int features[] = {
	VIRTIO_CAN_F_CAN_CLASSIC,
	VIRTIO_CAN_F_CAN_FD,
	VIRTIO_CAN_F_LATE_TX_ACK,
	VIRTIO_CAN_F_RTR_FRAMES,
};

static struct virtio_driver virtio_can_driver = {
	.feature_table = features,
	.feature_table_size = ARRAY_SIZE(features),
	.driver.name = KBUILD_MODNAME,
	.driver.owner = THIS_MODULE,
	.id_table = virtio_can_id_table,
	.validate = virtio_can_validate,
	.probe = virtio_can_probe,
	.remove = virtio_can_remove,
	.config_changed = virtio_can_config_changed,
#ifdef CONFIG_PM_SLEEP
	.freeze = virtio_can_freeze,
	.restore = virtio_can_restore,
#endif
};

module_virtio_driver(virtio_can_driver);
MODULE_DEVICE_TABLE(virtio, virtio_can_id_table);

MODULE_AUTHOR("OpenSynergy GmbH");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("CAN bus driver for Virtio CAN controller");
