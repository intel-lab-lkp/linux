/* SPDX-License-Identifier: GPL-2.0 or BSD-3-Clause */
/*
 * Copyright(c) 2025-2026 Cornelis Networks, Inc.
 */
#ifndef _HFI2_PINNING_H
#define _HFI2_PINNING_H

#include <rdma/hfi2-abi.h>

struct hfi2_user_sdma_pkt_q;
struct user_sdma_request;
struct user_sdma_txreq;
struct user_sdma_iovec;

struct pinning_interface {
	int (*init)(struct hfi2_user_sdma_pkt_q *pq);
	void (*free)(struct hfi2_user_sdma_pkt_q *pq);

	/*
	 * Add up to pkt_data_remaining bytes to the txreq, starting at the
	 * current offset in the given iovec entry and continuing until all
	 * data has been added to the iovec or the iovec entry type changes.
	 * On success, prior to returning, the implementation must adjust
	 * pkt_data_remaining, req->iov_idx, and the offset value in
	 * req->iov[req->iov_idx] to reflect the data that has been
	 * consumed.
	 */
	int (*add_to_sdma_packet)(struct user_sdma_request *req,
				  struct user_sdma_txreq *tx,
				  struct user_sdma_iovec *iovec,
				  u32 *pkt_data_remaining);

	int (*get_stats)(struct hfi2_user_sdma_pkt_q *pq, int index,
			 struct hfi2_pin_stats *stats);
	void (*put)(void *ptr);
};

#define PINNING_MAX_INTERFACES BIT(HFI2_MEMINFO_TYPE_ENTRY_BITS)

struct pinning_state {
	void *interface[PINNING_MAX_INTERFACES];
};

#define PINNING_STATE(pq, i) ((pq)->pinning_state.interface[(i)])

extern struct pinning_interface hfi2_pinning_interfaces[PINNING_MAX_INTERFACES];

void hfi2_register_pinning_interface(unsigned int type,
				struct pinning_interface *interface);
void hfi2_deregister_pinning_interface(unsigned int type);

void hfi2_register_system_pinning_interface(void);
void hfi2_deregister_system_pinning_interface(void);

int hfi2_init_pinning_interfaces(struct hfi2_user_sdma_pkt_q *pq);
void hfi2_free_pinning_interfaces(struct hfi2_user_sdma_pkt_q *pq);

static inline bool pinning_type_supported(unsigned int type)
{
	return (type < PINNING_MAX_INTERFACES &&
		hfi2_pinning_interfaces[type].add_to_sdma_packet);
}

static inline int add_to_sdma_packet(unsigned int type,
				     struct user_sdma_request *req,
				     struct user_sdma_txreq *tx,
				     struct user_sdma_iovec *iovec,
				     u32 *rem)
{
	return hfi2_pinning_interfaces[type].add_to_sdma_packet(req, tx, iovec,
							   rem);
}

#endif /* _HFI2_PINNING_H */
