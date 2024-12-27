/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2008-2010 Cisco Systems, Inc.  All rights reserved.
 * Copyright 2007 Nuova Systems, Inc.  All rights reserved.
 */

#ifndef _ENIC_RQ_H_
#define _ENIC_RQ_H_

void enic_intr_update_pkt_size(struct vnic_rx_bytes_counter *pkt_size,
			       u32 pkt_len);
void enic_rq_set_skb_flags(struct vnic_rq *rq, u8 type, u32 rss_hash, u8 rss_type,
			   u8 fcoe, u8 fcoe_fc_crc_ok, u8 vlan_stripped,
			   u8 csum_not_calc, u8 tcp_udp_csum_ok, u8 ipv6,
			   u8 ipv4_csum_ok, u16 vlan_tci, struct sk_buff *skb);
int enic_rq_pkt_error(struct vnic_rq *rq, u8 packet_error, u8 fcs_ok,
		      u16 bytes_written);
int enic_rq_service(struct vnic_dev *vdev, struct cq_desc *cq_desc,
		    u8 type, u16 q_number, u16 completed_index, void *opaque);
void enic_rq_indicate_buf(struct vnic_rq *rq, struct cq_desc *cq_desc,
			  struct vnic_rq_buf *buf, int skipped, void *opaque);
void enic_rq_indicate_page(struct vnic_rq *rq, struct cq_desc *cq_desc,
			   struct vnic_rq_buf *buf, int skipped, void *opaque);
int enic_rq_alloc_page(struct vnic_rq *rq);
void enic_rq_free_page(struct vnic_rq *rq, struct vnic_rq_buf *buf);
void enic_rq_page_cleanup(struct enic_rq *rq);
#endif /* _ENIC_RQ_H_ */
