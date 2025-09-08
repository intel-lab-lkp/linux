// SPDX-License-Identifier: GPL-2.0
/* RPMsg Based Virtual Ethernet Driver
 *
 * Copyright (C) 2025 Texas Instruments Incorporated - https://www.ti.com/
 */

#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/remoteproc.h>
#include "rpmsg_eth.h"

/**
 * rpmsg_eth_validate_handshake - Validate handshake parameters from remote
 * @port: Pointer to rpmsg_eth_port structure
 * @shm_info: Pointer to shared memory info received from remote
 *
 * Checks buffer size, magic numbers, and TX/RX offsets in the handshake
 * response to ensure they match expected values and are within valid ranges.
 *
 * Return: 0 on success, -EINVAL on validation failure.
 */
static int rpmsg_eth_validate_handshake(struct rpmsg_eth_port *port,
					struct rpmsg_eth_shm *shm_info)
{
	if (shm_info->buff_slot_size != RPMSG_ETH_BUFFER_SIZE) {
		dev_err(port->common->dev, "Buffer configuration mismatch in handshake: expected_buf_size=%zu, received_buf_size=%d\n",
			RPMSG_ETH_BUFFER_SIZE,
			shm_info->buff_slot_size);
		return -EINVAL;
	}

	if (readl(port->shm + port->tx_offset + HEAD_MAGIC_NUM_OFFSET) != RPMSG_ETH_SHM_MAGIC_NUM ||
	    readl(port->shm + port->rx_offset + HEAD_MAGIC_NUM_OFFSET) != RPMSG_ETH_SHM_MAGIC_NUM ||
	    readl(port->shm + port->tx_offset + TAIL_MAGIC_NUM_OFFSET(port->tx_max_buffers)) != RPMSG_ETH_SHM_MAGIC_NUM ||
	    readl(port->shm + port->rx_offset + TAIL_MAGIC_NUM_OFFSET(port->rx_max_buffers)) != RPMSG_ETH_SHM_MAGIC_NUM) {
		dev_err(port->common->dev, "Magic number mismatch in handshake at head/tail\n");
		return -EINVAL;
	}

	if (shm_info->tx_offset >= port->buf_size ||
	    shm_info->rx_offset >= port->buf_size) {
		dev_err(port->common->dev, "TX/RX offset out of range in handshake: tx_offset=0x%x, rx_offset=0x%x, size=0x%llx\n",
			shm_info->tx_offset,
			shm_info->rx_offset,
			port->buf_size);
		return -EINVAL;
	}

	return 0;
}

static int create_request(struct rpmsg_eth_common *common,
			  enum rpmsg_eth_rpmsg_type rpmsg_type)
{
	struct message *msg = &common->send_msg;
	int ret = 0;

	msg->msg_hdr.src_id = common->port->port_id;
	msg->req_msg.type = rpmsg_type;

	switch (rpmsg_type) {
	case RPMSG_ETH_REQ_SHM_INFO:
		msg->msg_hdr.msg_type = RPMSG_ETH_REQUEST_MSG;
		break;
	case RPMSG_ETH_REQ_SET_MAC_ADDR:
		msg->msg_hdr.msg_type = RPMSG_ETH_REQUEST_MSG;
		ether_addr_copy(msg->req_msg.mac_addr.addr,
				common->port->ndev->dev_addr);
		break;
	case RPMSG_ETH_REQ_ADD_MC_ADDR:
	case RPMSG_ETH_REQ_DEL_MC_ADDR:
		ether_addr_copy(msg->req_msg.mac_addr.addr,
				common->mcast_addr);
		break;
	case RPMSG_ETH_NOTIFY_PORT_UP:
	case RPMSG_ETH_NOTIFY_PORT_DOWN:
		msg->msg_hdr.msg_type = RPMSG_ETH_NOTIFY_MSG;
		break;
	default:
		ret = -EINVAL;
		dev_err(common->dev, "Invalid RPMSG request\n");
	}
	return ret;
}

static int rpmsg_eth_create_send_request(struct rpmsg_eth_common *common,
					 enum rpmsg_eth_rpmsg_type rpmsg_type,
					 bool wait)
{
	unsigned long flags;
	int ret = 0;

	if (wait)
		reinit_completion(&common->sync_msg);

	spin_lock_irqsave(&common->send_msg_lock, flags);
	ret = create_request(common, rpmsg_type);
	if (ret)
		goto release_lock;

	ret = rpmsg_send(common->rpdev->ept, (void *)(&common->send_msg),
			 sizeof(common->send_msg));
	if (ret) {
		dev_err(common->dev, "Failed to send RPMSG message\n");
		goto release_lock;
	}

	spin_unlock_irqrestore(&common->send_msg_lock, flags);
	if (wait) {
		ret = wait_for_completion_timeout(&common->sync_msg,
						  RPMSG_ETH_REQ_TIMEOUT_JIFFIES);

		if (!ret) {
			dev_err(common->dev, "Failed to receive response within %ld jiffies\n",
				RPMSG_ETH_REQ_TIMEOUT_JIFFIES);
			return -ETIMEDOUT;
		}
		ret = 0;
	}
	return ret;
release_lock:
	spin_unlock_irqrestore(&common->send_msg_lock, flags);
	return ret;
}

static int rpmsg_eth_add_mc_addr(struct net_device *ndev, const u8 *addr)
{
	struct rpmsg_eth_common *common = rpmsg_eth_ndev_to_common(ndev);

	ether_addr_copy(common->mcast_addr, addr);
	return rpmsg_eth_create_send_request(common, RPMSG_ETH_REQ_ADD_MC_ADDR, true);
}

static int rpmsg_eth_del_mc_addr(struct net_device *ndev, const u8 *addr)
{
	struct rpmsg_eth_common *common = rpmsg_eth_ndev_to_common(ndev);

	ether_addr_copy(common->mcast_addr, addr);
	return rpmsg_eth_create_send_request(common, RPMSG_ETH_REQ_DEL_MC_ADDR, true);
}

static void rpmsg_eth_state_machine(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct rpmsg_eth_common *common;
	struct rpmsg_eth_port *port;
	int ret;

	common = container_of(dwork, struct rpmsg_eth_common, state_work);
	port = common->port;

	mutex_lock(&common->state_lock);

	switch (common->state) {
	case RPMSG_ETH_STATE_PROBE:
		break;
	case RPMSG_ETH_STATE_OPEN:
		rpmsg_eth_create_send_request(common, RPMSG_ETH_REQ_SHM_INFO, false);
		break;
	case RPMSG_ETH_STATE_CLOSE:
		break;
	case RPMSG_ETH_STATE_READY:
		ret = rpmsg_eth_create_send_request(common, RPMSG_ETH_REQ_SET_MAC_ADDR, false);
		if (!ret) {
			napi_enable(&port->rx_napi);
			netif_carrier_on(port->ndev);
			mod_timer(&port->rx_timer, RX_POLL_TIMEOUT_JIFFIES);
		}
		break;
	case RPMSG_ETH_STATE_RUNNING:
		break;
	}
	mutex_unlock(&common->state_lock);
}

static int rpmsg_eth_rpmsg_cb(struct rpmsg_device *rpdev, void *data, int len,
			      void *priv, u32 src)
{
	struct rpmsg_eth_common *common = dev_get_drvdata(&rpdev->dev);
	struct message *msg = (struct message *)data;
	struct rpmsg_eth_port *port = common->port;
	u32 msg_type = msg->msg_hdr.msg_type;
	u32 rpmsg_type;
	int ret = 0;

	switch (msg_type) {
	case RPMSG_ETH_REQUEST_MSG:
		rpmsg_type = msg->req_msg.type;
		dev_dbg(common->dev, "Msg type = %d, RPMsg type = %d, Src Id = %d, Msg Id = %d\n",
			msg_type, rpmsg_type, msg->msg_hdr.src_id, msg->req_msg.id);
		break;
	case RPMSG_ETH_RESPONSE_MSG:
		rpmsg_type = msg->resp_msg.type;
		dev_dbg(common->dev, "Msg type = %d, RPMsg type = %d, Src Id = %d, Msg Id = %d\n",
			msg_type, rpmsg_type, msg->msg_hdr.src_id, msg->resp_msg.id);
		switch (rpmsg_type) {
		case RPMSG_ETH_RESP_SHM_INFO:
			/* Retrieve Tx and Rx shared memory info from msg */
			port->tx_offset = msg->resp_msg.shm_info.tx_offset;
			port->rx_offset = msg->resp_msg.shm_info.rx_offset;
			port->tx_max_buffers =
				msg->resp_msg.shm_info.num_pkt_bufs;
			port->rx_max_buffers =
				msg->resp_msg.shm_info.num_pkt_bufs;

			/* Handshake validation */
			ret = rpmsg_eth_validate_handshake(port, &msg->resp_msg.shm_info);
			if (ret) {
				dev_err(common->dev, "RPMSG handshake failed %d\n", ret);
				return ret;
			}

			mutex_lock(&common->state_lock);
			common->state = RPMSG_ETH_STATE_READY;
			mutex_unlock(&common->state_lock);

			mod_delayed_work(system_wq,
					 &common->state_work,
					 STATE_MACHINE_TIME_JIFFIES);

			break;
		case RPMSG_ETH_RESP_SET_MAC_ADDR:
			break;
		case RPMSG_ETH_RESP_ADD_MC_ADDR:
		case RPMSG_ETH_RESP_DEL_MC_ADDR:
			complete(&common->sync_msg);
			break;
		}
		break;
	case RPMSG_ETH_NOTIFY_MSG:
		rpmsg_type = msg->notify_msg.type;
		dev_dbg(common->dev, "Msg type = %d, RPMsg type = %d, Src Id = %d, Msg Id = %d\n",
			msg_type, rpmsg_type, msg->msg_hdr.src_id, msg->notify_msg.id);
		switch (rpmsg_type) {
		case RPMSG_ETH_NOTIFY_REMOTE_READY:
			mutex_lock(&common->state_lock);
			common->state = RPMSG_ETH_STATE_RUNNING;
			mutex_unlock(&common->state_lock);

			mod_delayed_work(system_wq,
					 &common->state_work,
					 STATE_MACHINE_TIME_JIFFIES);
			break;
		case RPMSG_ETH_NOTIFY_PORT_UP:
		case RPMSG_ETH_NOTIFY_PORT_DOWN:
			break;
		}
		break;
	default:
		dev_err(common->dev, "Invalid msg type\n");
		ret = -EINVAL;
		break;
	}
	return ret;
}

/**
 * rpmsg_eth_get_shm_info - Retrieve shared memory region for RPMsg Ethernet
 * @common: Pointer to rpmsg_eth_common structure
 *
 * This function locates and maps the reserved memory region for the RPMsg
 * Ethernet device by traversing the device tree hierarchy. It first identifies
 * the associated remote processor (rproc), then locates the "rpmsg-eth" child
 * node within the rproc's device tree node, and finally retrieves the
 * "memory-region" phandle that points to the reserved memory region.
 * Once found, the shared memory region is mapped into the
 * kernel's virtual address space using devm_ioremap()
 *
 * Return: 0 on success, negative error code on failure.
 */
static int rpmsg_eth_get_shm_info(struct rpmsg_eth_common *common)
{
	struct device_node *np, *rmem_np;
	struct reserved_mem *rmem;
	struct rproc *rproc;

	/* Get the remote processor associated with this device */
	rproc = rproc_get_by_child(&common->rpdev->dev);
	if (!rproc) {
		dev_err(common->dev, "rpmsg eth device not child of rproc\n");
		return -EINVAL;
	}

	/* Get the device node from rproc or its parent */
	np = rproc->dev.of_node ?: (rproc->dev.parent ? rproc->dev.parent->of_node : NULL);
	if (!np) {
		dev_err(common->dev, "Cannot find rproc device node\n");
		return -ENODEV;
	}

	/* Parse the memory-region phandle */
	rmem_np = of_parse_phandle(np, "memory-region", common->data.shm_region_index);
	of_node_put(np);
	if (!rmem_np)
		return -EINVAL;

	/* Lookup the reserved memory region */
	rmem = of_reserved_mem_lookup(rmem_np);
	of_node_put(rmem_np);
	if (!rmem)
		return -EINVAL;

	common->port->shm = devm_ioremap(common->dev, rmem->base, rmem->size);
	if (IS_ERR(common->port->shm))
		return PTR_ERR(common->port->shm);

	common->port->buf_size = rmem->size;

	return 0;
}

static void rpmsg_eth_rx_timer(struct timer_list *timer)
{
	struct rpmsg_eth_port *port = timer_container_of(port, timer, rx_timer);
	struct napi_struct *napi;
	int num_pkts = 0;
	u32 head, tail;

	head = readl(port->shm + port->rx_offset + HEAD_IDX_OFFSET);
	tail = readl(port->shm + port->rx_offset +
		     TAIL_IDX_OFFSET(port->rx_max_buffers));

	num_pkts = tail - head;
	num_pkts = num_pkts >= 0 ? num_pkts :
				   (num_pkts + port->rx_max_buffers);

	napi = &port->rx_napi;
	if (num_pkts && likely(napi_schedule_prep(napi)))
		__napi_schedule(napi);
	else
		mod_timer(&port->rx_timer, RX_POLL_JIFFIES);
}

static int rpmsg_eth_rx_packets(struct napi_struct *napi, int budget)
{
	struct rpmsg_eth_port *port = container_of(napi, struct rpmsg_eth_port, rx_napi);
	u32 count, process_pkts;
	struct sk_buff *skb;
	u32 head, tail;
	int num_pkts;
	u32 pkt_len;

	head = readl(port->shm + port->rx_offset + HEAD_IDX_OFFSET);
	tail = readl(port->shm + port->rx_offset +
		     TAIL_IDX_OFFSET(port->rx_max_buffers));

	num_pkts = head - tail;

	num_pkts = num_pkts >= 0 ? num_pkts :
				   (num_pkts + port->rx_max_buffers);
	process_pkts = min(num_pkts, budget);
	count = 0;
	while (count < process_pkts) {
		memcpy_fromio((void *)&pkt_len,
			      port->shm + port->rx_offset + MAGIC_NUM_SIZE_TYPE +
			      PKT_START_OFFSET((tail + count) % port->rx_max_buffers),
			      PKT_LEN_SIZE_TYPE);
		/* Start building the skb */
		skb = napi_alloc_skb(napi, pkt_len);
		if (!skb) {
			port->ndev->stats.rx_dropped++;
			goto rx_dropped;
		}

		skb->dev = port->ndev;
		skb_put(skb, pkt_len);
		memcpy_fromio((void *)skb->data,
			      port->shm + port->rx_offset + PKT_LEN_SIZE_TYPE +
			      MAGIC_NUM_SIZE_TYPE +
			      PKT_START_OFFSET((tail + count) % port->rx_max_buffers),
			      pkt_len);

		skb->protocol = eth_type_trans(skb, port->ndev);

		/* Push skb into network stack */
		napi_gro_receive(napi, skb);

		count++;
		port->ndev->stats.rx_packets++;
		port->ndev->stats.rx_bytes += skb->len;
	}

rx_dropped:

	if (num_pkts) {
		writel((tail + count) % port->rx_max_buffers,
		       port->shm + port->rx_offset +
		       TAIL_IDX_OFFSET(port->rx_max_buffers));

		if (num_pkts < budget && napi_complete_done(napi, count))
			mod_timer(&port->rx_timer, RX_POLL_TIMEOUT_JIFFIES);
	}

	return count;
}

static int rpmsg_eth_ndo_open(struct net_device *ndev)
{
	struct rpmsg_eth_common *common = rpmsg_eth_ndev_to_common(ndev);

	mutex_lock(&common->state_lock);
	common->state = RPMSG_ETH_STATE_OPEN;
	mutex_unlock(&common->state_lock);
	mod_delayed_work(system_wq, &common->state_work, msecs_to_jiffies(100));

	return 0;
}

static int rpmsg_eth_ndo_stop(struct net_device *ndev)
{
	struct rpmsg_eth_common *common = rpmsg_eth_ndev_to_common(ndev);
	struct rpmsg_eth_port *port = rpmsg_eth_ndev_to_port(ndev);

	mutex_lock(&common->state_lock);
	common->state = RPMSG_ETH_STATE_CLOSE;
	mutex_unlock(&common->state_lock);

	netif_carrier_off(port->ndev);

	__dev_mc_unsync(ndev, rpmsg_eth_del_mc_addr);
	__hw_addr_init(&common->mc_list);

	cancel_delayed_work_sync(&common->state_work);
	timer_delete_sync(&port->rx_timer);
	napi_disable(&port->rx_napi);

	cancel_work_sync(&common->rx_mode_work);

	return 0;
}

static netdev_tx_t rpmsg_eth_start_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct rpmsg_eth_port *port = rpmsg_eth_ndev_to_port(ndev);
	u32 head, tail;
	int num_pkts;
	u32 len;

	len = skb_headlen(skb);
	head = readl(port->shm + port->tx_offset + HEAD_IDX_OFFSET);
	tail = readl(port->shm + port->tx_offset +
		     TAIL_IDX_OFFSET(port->tx_max_buffers));

	/* If the buffer queue is full, then drop packet */
	num_pkts = head - tail;
	num_pkts = num_pkts >= 0 ? num_pkts :
				   (num_pkts + port->tx_max_buffers);

	if ((num_pkts + 1) == port->tx_max_buffers) {
		netdev_warn(ndev, "Tx buffer full %d\n", num_pkts);
		goto ring_full;
	}
	/* Copy length */
	memcpy_toio(port->shm + port->tx_offset + PKT_START_OFFSET(head) + MAGIC_NUM_SIZE_TYPE,
		    (void *)&len, PKT_LEN_SIZE_TYPE);
	/* Copy data to shared mem */
	memcpy_toio(port->shm + port->tx_offset + PKT_START_OFFSET(head) + MAGIC_NUM_SIZE_TYPE +
		    PKT_LEN_SIZE_TYPE, (void *)skb->data, len);
	writel((head + 1) % port->tx_max_buffers,
	       port->shm + port->tx_offset + HEAD_IDX_OFFSET);

	ndev->stats.tx_packets++;
	ndev->stats.tx_bytes += skb->len;

	dev_consume_skb_any(skb);
	return NETDEV_TX_OK;

ring_full:
	return NETDEV_TX_BUSY;
}

static int rpmsg_eth_set_mac_address(struct net_device *ndev, void *addr)
{
	struct rpmsg_eth_common *common = rpmsg_eth_ndev_to_common(ndev);
	int ret;

	ret = eth_mac_addr(ndev, addr);

	if (ret < 0)
		return ret;
	ret = rpmsg_eth_create_send_request(common, RPMSG_ETH_REQ_SET_MAC_ADDR, false);
	return ret;
}

static void rpmsg_eth_ndo_set_rx_mode_work(struct work_struct *work)
{
	struct rpmsg_eth_common *common;
	struct net_device *ndev;

	common = container_of(work, struct rpmsg_eth_common, rx_mode_work);
	ndev = common->port->ndev;

	/* make a mc list copy */
	netif_addr_lock_bh(ndev);
	__hw_addr_sync(&common->mc_list, &ndev->mc, ndev->addr_len);
	netif_addr_unlock_bh(ndev);

	__hw_addr_sync_dev(&common->mc_list, ndev, rpmsg_eth_add_mc_addr,
			   rpmsg_eth_del_mc_addr);
}

static void rpmsg_eth_set_rx_mode(struct net_device *ndev)
{
	struct rpmsg_eth_common *common = rpmsg_eth_ndev_to_common(ndev);

	queue_work(common->cmd_wq, &common->rx_mode_work);
}

static const struct net_device_ops rpmsg_eth_netdev_ops = {
	.ndo_open = rpmsg_eth_ndo_open,
	.ndo_stop = rpmsg_eth_ndo_stop,
	.ndo_start_xmit = rpmsg_eth_start_xmit,
	.ndo_set_rx_mode = rpmsg_eth_set_rx_mode,
	.ndo_set_mac_address = rpmsg_eth_set_mac_address,
};

static int rpmsg_eth_init_ndev(struct rpmsg_eth_common *common)
{
	struct device *dev = &common->rpdev->dev;
	struct rpmsg_eth_ndev_priv *ndev_priv;
	struct rpmsg_eth_port *port;
	static u32 port_id;
	int err = 0;

	port = common->port;
	port->common = common;
	port->port_id = port_id++;

	port->ndev = devm_alloc_etherdev_mqs(common->dev, sizeof(*ndev_priv),
					     RPMSG_ETH_MAX_TX_QUEUES,
					     RPMSG_ETH_MAX_RX_QUEUES);

	if (!port->ndev) {
		dev_err(dev, "error allocating net_device\n");
		return -ENOMEM;
	}

	ndev_priv = netdev_priv(port->ndev);
	ndev_priv->port = port;
	port->ndev->netdev_ops = &rpmsg_eth_netdev_ops;
	SET_NETDEV_DEV(port->ndev, dev);

	port->ndev->min_mtu = RPMSG_ETH_MIN_PACKET_SIZE;
	port->ndev->max_mtu = MAX_MTU;

	if (!is_valid_ether_addr(port->ndev->dev_addr)) {
		eth_hw_addr_random(port->ndev);
		dev_dbg(dev, "Using random MAC address %pM\n", port->ndev->dev_addr);
	}

	netif_carrier_off(port->ndev);
	netif_napi_add(port->ndev, &port->rx_napi, rpmsg_eth_rx_packets);
	timer_setup(&port->rx_timer, rpmsg_eth_rx_timer, 0);
	err = register_netdev(port->ndev);
	if (err)
		dev_err(dev, "error registering rpmsg_eth net device %d\n", err);

	return err;
}

static int rpmsg_eth_probe(struct rpmsg_device *rpdev)
{
	struct device *dev = &rpdev->dev;
	struct rpmsg_eth_common *common;
	int ret = 0;

	common = devm_kzalloc(&rpdev->dev, sizeof(*common), GFP_KERNEL);
	if (!common)
		return -ENOMEM;

	dev_set_drvdata(dev, common);

	common->port = devm_kzalloc(dev, sizeof(*common->port), GFP_KERNEL);
	common->dev = dev;
	common->rpdev = rpdev;
	common->data = *(const struct rpmsg_eth_data *)rpdev->id.driver_data;
	dev_err(dev, "shm_index = %d\n", common->data.shm_region_index);
	common->state = RPMSG_ETH_STATE_PROBE;

	ret = rpmsg_eth_get_shm_info(common);
	if (ret)
		return ret;

	spin_lock_init(&common->send_msg_lock);
	spin_lock_init(&common->recv_msg_lock);
	mutex_init(&common->state_lock);
	INIT_DELAYED_WORK(&common->state_work, rpmsg_eth_state_machine);
	init_completion(&common->sync_msg);

	__hw_addr_init(&common->mc_list);
	INIT_WORK(&common->rx_mode_work, rpmsg_eth_ndo_set_rx_mode_work);
	common->cmd_wq = create_singlethread_workqueue("rpmsg_eth_rx_work");
	if (!common->cmd_wq) {
		dev_err(dev, "Failure requesting workqueue\n");
		return -ENOMEM;
	}
	/* Register the network device */
	ret = rpmsg_eth_init_ndev(common);
	if (ret)
		return ret;

	return 0;
}

static void rpmsg_eth_remove(struct rpmsg_device *rpdev)
{
	struct rpmsg_eth_common *common = dev_get_drvdata(&rpdev->dev);
	struct rpmsg_eth_port *port = common->port;

	netif_napi_del(&port->rx_napi);
	timer_delete_sync(&port->rx_timer);
	destroy_workqueue(common->cmd_wq);

	dev_dbg(&rpdev->dev, "rpmsg-eth client driver is removed\n");
}

static const struct rpmsg_eth_data ti_rpmsg_eth_data = {
	.shm_region_index = 2,
};

static struct rpmsg_device_id rpmsg_eth_id_table[] = {
	{ .name = "ti.shm-eth", .driver_data = (kernel_ulong_t)&ti_rpmsg_eth_data },
	{},
};
MODULE_DEVICE_TABLE(rpmsg, rpmsg_eth_id_table);

static struct rpmsg_driver rpmsg_eth_rpmsg_client = {
	.drv.name = KBUILD_MODNAME,
	.id_table = rpmsg_eth_id_table,
	.probe = rpmsg_eth_probe,
	.callback = rpmsg_eth_rpmsg_cb,
	.remove = rpmsg_eth_remove,
};
module_rpmsg_driver(rpmsg_eth_rpmsg_client);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("MD Danish Anwar <danishanwar@ti.com>");
MODULE_DESCRIPTION("RPMsg Based Virtual Ethernet driver");
