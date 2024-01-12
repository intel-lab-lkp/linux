// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023, Fibocom Wireless Inc.
 *
 * Authors:
 *  Jinjian Song <jinjian.song@fibocom.com>
 */

#include <linux/atomic.h>
#include <linux/dev_printk.h>
#include <linux/err.h>
#include <linux/minmax.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/wwan.h>

#include "t7xx_port.h"
#include "t7xx_port_proxy.h"
#include "t7xx_state_monitor.h"

static int t7xx_port_fastboot_start(struct wwan_port *port)
{
	struct t7xx_port *port_mtk = wwan_port_get_drvdata(port);

	if (atomic_read(&port_mtk->usage_cnt))
		return -EBUSY;

	atomic_inc(&port_mtk->usage_cnt);
	return 0;
}

static void t7xx_port_fastboot_stop(struct wwan_port *port)
{
	struct t7xx_port *port_mtk = wwan_port_get_drvdata(port);

	atomic_dec(&port_mtk->usage_cnt);
}

static int t7xx_port_fastboot_tx(struct wwan_port *port, struct sk_buff *skb)
{
	struct t7xx_port *port_private = wwan_port_get_drvdata(port);
	struct sk_buff *cur = skb, *cloned;
	size_t actual, len, offset = 0;
	int ret;
	int txq_mtu;

	if (!port_private->chan_enable)
		return -EINVAL;

	txq_mtu = t7xx_get_port_mtu(port_private);
	if (txq_mtu < 0)
		return -EINVAL;

	actual = cur->len;
	while (actual) {
		len = min_t(size_t, actual, txq_mtu);
		cloned = __dev_alloc_skb(len, GFP_KERNEL);
		if (!cloned)
			return -ENOMEM;

		skb_put_data(cloned, cur->data + offset, len);

		ret = t7xx_port_send_raw_skb(port_private, cloned);
		if (ret) {
			dev_kfree_skb(cloned);
			dev_err(port_private->dev, "Write error on fastboot port, %d\n", ret);
			break;
		}
		offset += len;
		actual -= len;
	}

	dev_kfree_skb(skb);
	return 0;
}

static const struct wwan_port_ops wwan_ops = {
	.start = t7xx_port_fastboot_start,
	.stop = t7xx_port_fastboot_stop,
	.tx = t7xx_port_fastboot_tx,
};

static int t7xx_port_fastboot_init(struct t7xx_port *port)
{
	const struct t7xx_port_conf *port_conf = port->port_conf;
	unsigned int header_len = sizeof(struct ccci_header), mtu;
	struct wwan_port_caps caps;

	port->rx_length_th = RX_QUEUE_MAXLEN;

	if (!port->wwan.wwan_port) {
		mtu = t7xx_get_port_mtu(port);
		caps.frag_len = mtu - header_len;
		caps.headroom_len = header_len;
		port->wwan.wwan_port = wwan_create_port(port->dev, port_conf->port_type,
							&wwan_ops, &caps, port);
		if (IS_ERR(port->wwan.wwan_port))
			dev_err(port->dev, "Unable to create WWWAN port %s", port_conf->name);
	}

	return 0;
}

static void t7xx_port_fastboot_uninit(struct t7xx_port *port)
{
	if (!port->wwan.wwan_port)
		return;

	port->rx_length_th = 0;
	wwan_remove_port(port->wwan.wwan_port);
	port->wwan.wwan_port = NULL;
}

static int t7xx_port_fastboot_recv_skb(struct t7xx_port *port, struct sk_buff *skb)
{
	if (!atomic_read(&port->usage_cnt) || !port->chan_enable) {
		const struct t7xx_port_conf *port_conf = port->port_conf;

		dev_kfree_skb_any(skb);
		dev_err_ratelimited(port->dev, "Port %s is not opened, drop packets\n",
				    port_conf->name);
		/* Dropping skb, caller should not access skb.*/
		return 0;
	}

	wwan_port_rx(port->wwan.wwan_port, skb);

	return 0;
}

static int t7xx_port_fastboot_enable_chl(struct t7xx_port *port)
{
	spin_lock(&port->port_update_lock);
	port->chan_enable = true;
	spin_unlock(&port->port_update_lock);

	return 0;
}

static int t7xx_port_fastboot_disable_chl(struct t7xx_port *port)
{
	spin_lock(&port->port_update_lock);
	port->chan_enable = false;
	spin_unlock(&port->port_update_lock);

	return 0;
}

struct port_ops fastboot_port_ops = {
	.init = t7xx_port_fastboot_init,
	.recv_skb = t7xx_port_fastboot_recv_skb,
	.uninit = t7xx_port_fastboot_uninit,
	.enable_chl = t7xx_port_fastboot_enable_chl,
	.disable_chl = t7xx_port_fastboot_disable_chl,
};
