/* SPDX-License-Identifier: GPL-2.0 */
/* AF_XDP RX in the device's buffer layout, see xsk_sink.c */

#ifndef XSK_SINK_H_
#define XSK_SINK_H_

struct net_device;
struct xsk_buff_pool;

int xsk_sink_bind(struct xsk_buff_pool *pool, struct net_device *dev,
		  u16 queue_id, u16 flags);
void xsk_sink_unbind(struct xsk_buff_pool *pool);

#endif /* XSK_SINK_H_ */
