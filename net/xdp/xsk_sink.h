/* SPDX-License-Identifier: GPL-2.0 */
/* AF_XDP RX in the device's buffer layout, see xsk_sink.c */

#ifndef XSK_SINK_H_
#define XSK_SINK_H_

struct net_device;
struct xdp_sock;
struct xsk_buff_pool;
struct xsk_sink;

int xsk_sink_bind(struct xsk_buff_pool *pool, struct net_device *dev,
		  u16 queue_id, u16 flags);
void xsk_sink_unbind(struct xsk_buff_pool *pool);
void xsk_sink_flush(struct xsk_sink *sink);
void xsk_sink_add_sock(struct xsk_sink *sink, struct xdp_sock *xs);
struct xdp_sock *xsk_sink_sock(struct xsk_sink *sink);

#endif /* XSK_SINK_H_ */
