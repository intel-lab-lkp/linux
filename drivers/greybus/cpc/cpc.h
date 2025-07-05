/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2025, Silicon Laboratories, Inc.
 */

#ifndef __CPC_H
#define __CPC_H

#include <linux/device.h>
#include <linux/types.h>

#include "header.h"

#define GB_CPC_SPI_NUM_CPORTS			8

struct cpc_endpoint;
struct cpc_endpoint_tcb;
struct cpc_frame;
struct cpc_host_device;

/**
 * struct cpc_host_device - CPC host device
 * @gb_hd: pointer to Greybus Host Device
 * @lock: mutex to synchronize access to endpoint array
 * @tx_queue: list of cpc_frame to send
 * @endpoints: array of endpoint pointers
 * @wake_tx: function called when a new packet must be transmitted
 */
struct cpc_host_device {
	struct gb_host_device	*gb_hd;

	struct mutex		lock;
	struct list_head	tx_queue;

	struct cpc_endpoint	*endpoints[GB_CPC_SPI_NUM_CPORTS];

	int (*wake_tx)(struct cpc_host_device *cpc_hd);
};

struct cpc_endpoint *cpc_hd_get_endpoint(struct cpc_host_device *cpc_hd, u16 cport_id);
void cpc_hd_send_frame(struct cpc_host_device *cpc_hd, struct cpc_frame *frame);
void cpc_hd_rcvd(struct cpc_host_device *cpc_hd, struct cpc_header *hdr,
		 u8 *data, size_t length);
struct cpc_frame *cpc_hd_dequeue(struct cpc_host_device *cpc_hd);
bool cpc_hd_tx_queue_empty(struct cpc_host_device *cpc_hd);

/**
 * struct cpc_endpoint_tcb - endpoint's transmission control block
 * @send_wnd: send window, maximum number of frames that the remote can accept
 *            TX frames should have a sequence in the range
 *            [send_una; send_una + send_wnd].
 * @send_nxt: send next, the next sequence number that will be used for transmission
 * @send_una: send unacknowledged, the oldest unacknowledged sequence number
 * @ack: current acknowledge number
 * @seq: current sequence number
 * @mtu: maximum transmission unit
 */
struct cpc_endpoint_tcb {
	u8 send_wnd;
	u8 send_nxt;
	u8 send_una;
	u8 ack;
	u8 seq;
	u16 mtu;
};

/**
 * struct cpc_endpint - CPC endpoint
 * @id: endpoint ID
 * @cpc_hd: pointer to the CPC host device this endpoint belongs to
 * @lock: synchronize access to other attributes
 * @completion: (dis)connection completion
 * @tcb: transmission control block
 * @holding_queue: list of CPC frames queued to be sent
 * @pending_ack_queue: list of CPC frames sent and waiting for acknowledgment
 */
struct cpc_endpoint {
	u16			id;

	struct cpc_host_device	*cpc_hd;

	struct mutex		lock;		/* Synchronize access to all other attributes. */
	struct completion	completion;
	struct cpc_endpoint_tcb	tcb;
	struct list_head	holding_queue;
	struct list_head	pending_ack_queue;
};

struct cpc_endpoint *cpc_endpoint_alloc(u16 ep_id, gfp_t gfp_mask);
void cpc_endpoint_release(struct cpc_endpoint *ep);
int cpc_endpoint_frame_send(struct cpc_endpoint *ep, struct cpc_frame *frame);
int cpc_endpoint_connect(struct cpc_endpoint *ep);
int cpc_endpoint_disconnect(struct cpc_endpoint *ep);

/**
 * struct cpc_frame - CPC frame
 * @header: CPC header
 * @message: Greybus message to transmit
 * @cancelled: indicate if Greybus message is cancelled and should not be sent
 * @ep: endpoint this frame is sent over
 * @links: list head in endpoint's queue
 * @txq_links: list head in cpc host device's queue
 */
struct cpc_frame {
	struct cpc_header	header;
	struct gb_message	*message;

	bool			cancelled;

	struct cpc_endpoint	*ep;

	struct list_head	links;		/* endpoint->holding_queue or
						 * endpoint->pending_ack_queue.
						 */
	struct list_head	txq_links;	/* cpc_host_device->tx_queue. */

};

struct cpc_frame *cpc_frame_alloc(struct gb_message *message, gfp_t gfp_mask);
void cpc_frame_free(struct cpc_frame *frame);
void cpc_frame_sent(struct cpc_frame *frame, int status);

int __cpc_protocol_write(struct cpc_endpoint *ep, struct cpc_frame *frame);

void cpc_protocol_on_data(struct cpc_endpoint *ep, struct cpc_header *hdr, u8 *data, size_t length);
void cpc_protocol_on_syn(struct cpc_endpoint *ep, struct cpc_header *hdr);
void cpc_protocol_on_rst(struct cpc_endpoint *ep);

void cpc_protocol_send_rst(struct cpc_host_device *cpc_hd, u8 ep_id);
int cpc_protocol_send_syn(struct cpc_endpoint *ep);

int cpc_spi_register_driver(void);
void cpc_spi_unregister_driver(void);

#endif
