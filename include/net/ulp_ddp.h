/* SPDX-License-Identifier: GPL-2.0
 *
 * ulp_ddp.h
 *   Author:	Boris Pismenny <borisp@nvidia.com>
 *   Copyright (c) 2023, NVIDIA CORPORATION & AFFILIATES.  All rights reserved.
 */
#ifndef _ULP_DDP_H
#define _ULP_DDP_H

#include <linux/netdevice.h>
#include <net/inet_connection_sock.h>
#include <net/sock.h>
#include <uapi/linux/ulp_ddp.h>

enum ulp_ddp_type {
	ULP_DDP_NVME = 1,
};

/**
 * struct nvme_tcp_ddp_limits - nvme tcp driver limitations
 *
 * @full_ccid_range:	true if the driver supports the full CID range
 */
struct nvme_tcp_ddp_limits {
	bool			full_ccid_range;
};

/**
 * struct ulp_ddp_limits - Generic ulp ddp limits: tcp ddp
 * protocol limits.
 * Add new instances of ulp_ddp_limits in the union below (nvme-tcp, etc.).
 *
 * @type:		type of this limits struct
 * @max_ddp_sgl_len:	maximum sgl size supported (zero means no limit)
 * @io_threshold:	minimum payload size required to offload
 * @tls:		support for ULP over TLS
 * @nvmeotcp:		NVMe-TCP specific limits
 */
struct ulp_ddp_limits {
	enum ulp_ddp_type	type;
	int			max_ddp_sgl_len;
	int			io_threshold;
	bool			tls:1;
	union {
		struct nvme_tcp_ddp_limits nvmeotcp;
	};
};

/**
 * struct nvme_tcp_ddp_config - nvme tcp ddp configuration for an IO queue
 *
 * @pfv:	pdu version (e.g., NVME_TCP_PFV_1_0)
 * @cpda:	controller pdu data alignment (dwords, 0's based)
 * @dgst:	digest types enabled (header or data, see
 *		enum nvme_tcp_digest_option).
 *		The netdev will offload crc if it is supported.
 * @queue_size: number of nvme-tcp IO queue elements
 * @queue_id:	queue identifier
 */
struct nvme_tcp_ddp_config {
	u16			pfv;
	u8			cpda;
	u8			dgst;
	int			queue_size;
	int			queue_id;
};

/**
 * struct ulp_ddp_config - Generic ulp ddp configuration
 * Add new instances of ulp_ddp_config in the union below (nvme-tcp, etc.).
 *
 * @type:	type of this config struct
 * @nvmeotcp:	NVMe-TCP specific config
 * @io_cpu:	cpu core running the IO thread for this socket
 */
struct ulp_ddp_config {
	enum ulp_ddp_type    type;
	int		     io_cpu;
	union {
		struct nvme_tcp_ddp_config nvmeotcp;
	};
};

/**
 * struct ulp_ddp_io - ulp ddp configuration for an IO request.
 *
 * @command_id: identifier on the wire associated with these buffers
 * @nents:	number of entries in the sg_table
 * @sg_table:	describing the buffers for this IO request
 * @first_sgl:	first SGL in sg_table
 */
struct ulp_ddp_io {
	u32			command_id;
	int			nents;
	struct sg_table		sg_table;
	struct scatterlist	first_sgl[SG_CHUNK_SIZE];
};

/**
 * struct ulp_ddp_stats - ULP DDP offload statistics
 * @rx_nvmeotcp_sk_add: number of sockets successfully prepared for offloading.
 * @rx_nvmeotcp_sk_add_fail: number of sockets that failed to be prepared
 *                           for offloading.
 * @rx_nvmeotcp_sk_del: number of sockets where offloading has been removed.
 * @rx_nvmeotcp_ddp_setup: number of NVMeTCP PDU successfully prepared for
 *                         Direct Data Placement.
 * @rx_nvmeotcp_ddp_setup_fail: number of PDUs that failed DDP preparation.
 * @rx_nvmeotcp_ddp_teardown: number of PDUs done with DDP.
 * @rx_nvmeotcp_drop: number of PDUs dropped.
 * @rx_nvmeotcp_resync: number of resync.
 * @rx_nvmeotcp_packets: number of offloaded PDUs.
 * @rx_nvmeotcp_bytes: number of offloaded bytes.
 */
struct ulp_ddp_stats {
	u64 rx_nvmeotcp_sk_add;
	u64 rx_nvmeotcp_sk_add_fail;
	u64 rx_nvmeotcp_sk_del;
	u64 rx_nvmeotcp_ddp_setup;
	u64 rx_nvmeotcp_ddp_setup_fail;
	u64 rx_nvmeotcp_ddp_teardown;
	u64 rx_nvmeotcp_drop;
	u64 rx_nvmeotcp_resync;
	u64 rx_nvmeotcp_packets;
	u64 rx_nvmeotcp_bytes;

	/*
	 * add new stats at the end and keep in sync with
	 * Documentation/netlink/specs/ulp_ddp.yaml
	 */
};

#define ULP_DDP_CAP_COUNT (ULP_DDP_CAP_MAX + 1)

struct ulp_ddp_dev_caps {
	DECLARE_BITMAP(active, ULP_DDP_CAP_COUNT);
	DECLARE_BITMAP(hw, ULP_DDP_CAP_COUNT);
};

struct netlink_ext_ack;

/**
 * struct ulp_ddp_dev_ops - operations used by an upper layer protocol
 *                          to configure ddp offload
 *
 * @limits:    query ulp driver limitations and quirks.
 * @sk_add:    add offload for the queue represented by socket+config
 *             pair. this function is used to configure either copy, crc
 *             or both offloads.
 * @sk_del:    remove offload from the socket, and release any device
 *             related resources.
 * @setup:     request copy offload for buffers associated with a
 *             command_id in ulp_ddp_io.
 * @teardown:  release offload resources association between buffers
 *             and command_id in ulp_ddp_io.
 * @resync:    respond to the driver's resync_request. Called only if
 *             resync is successful.
 * @set_caps:  set device ULP DDP capabilities.
 *	       returns a negative error code or zero.
 * @get_caps:  get device ULP DDP capabilities.
 * @get_stats: query ULP DDP statistics.
 */
struct ulp_ddp_dev_ops {
	int (*limits)(struct net_device *netdev,
		      struct ulp_ddp_limits *limits);
	int (*sk_add)(struct net_device *netdev,
		      struct sock *sk,
		      struct ulp_ddp_config *config);
	void (*sk_del)(struct net_device *netdev,
		       struct sock *sk);
	int (*setup)(struct net_device *netdev,
		     struct sock *sk,
		     struct ulp_ddp_io *io);
	void (*teardown)(struct net_device *netdev,
			 struct sock *sk,
			 struct ulp_ddp_io *io,
			 void *ddp_ctx);
	void (*resync)(struct net_device *netdev,
		       struct sock *sk, u32 seq);
	int (*set_caps)(struct net_device *dev, unsigned long *bits,
			struct netlink_ext_ack *extack);
	void (*get_caps)(struct net_device *dev,
			 struct ulp_ddp_dev_caps *caps);
	int (*get_stats)(struct net_device *dev,
			 struct ulp_ddp_stats *stats);
};

#define ULP_DDP_RESYNC_PENDING BIT(0)

/**
 * struct ulp_ddp_ulp_ops - Interface to register upper layer
 *                          Direct Data Placement (DDP) TCP offload.
 * @resync_request:         NIC requests ulp to indicate if @seq is the start
 *                          of a message.
 * @ddp_teardown_done:      NIC driver informs the ulp that teardown is done,
 *                          used for async completions.
 */
struct ulp_ddp_ulp_ops {
	bool (*resync_request)(struct sock *sk, u32 seq, u32 flags);
	void (*ddp_teardown_done)(void *ddp_ctx);
};

/**
 * struct ulp_ddp_ctx - Generic ulp ddp context
 *
 * @type:	type of this context struct
 * @buf:	protocol-specific context struct
 */
struct ulp_ddp_ctx {
	enum ulp_ddp_type	type;
	unsigned char		buf[];
};

static inline struct ulp_ddp_ctx *ulp_ddp_get_ctx(const struct sock *sk)
{
#ifdef CONFIG_ULP_DDP
	struct inet_connection_sock *icsk = inet_csk(sk);

	return (__force struct ulp_ddp_ctx *)icsk->icsk_ulp_ddp_data;
#else
	return NULL;
#endif
}

static inline void ulp_ddp_set_ctx(struct sock *sk, void *ctx)
{
#ifdef CONFIG_ULP_DDP
	struct inet_connection_sock *icsk = inet_csk(sk);

	rcu_assign_pointer(icsk->icsk_ulp_ddp_data, ctx);
#endif
}

static inline int ulp_ddp_setup(struct net_device *netdev,
				struct sock *sk,
				struct ulp_ddp_io *io)
{
#ifdef CONFIG_ULP_DDP
	return netdev->netdev_ops->ulp_ddp_ops->setup(netdev, sk, io);
#else
	return -EOPNOTSUPP;
#endif
}

static inline void ulp_ddp_teardown(struct net_device *netdev,
				    struct sock *sk,
				    struct ulp_ddp_io *io,
				    void *ddp_ctx)
{
#ifdef CONFIG_ULP_DDP
	netdev->netdev_ops->ulp_ddp_ops->teardown(netdev, sk, io, ddp_ctx);
#endif
}

static inline void ulp_ddp_resync(struct net_device *netdev,
				  struct sock *sk,
				  u32 seq)
{
#ifdef CONFIG_ULP_DDP
	netdev->netdev_ops->ulp_ddp_ops->resync(netdev, sk, seq);
#endif
}

static inline bool ulp_ddp_cap_turned_on(unsigned long *old,
					 unsigned long *new,
					 int bit_nr)
{
	return !test_bit(bit_nr, old) && test_bit(bit_nr, new);
}

static inline bool ulp_ddp_cap_turned_off(unsigned long *old,
					  unsigned long *new,
					  int bit_nr)
{
	return test_bit(bit_nr, old) && !test_bit(bit_nr, new);
}

#ifdef CONFIG_ULP_DDP

int ulp_ddp_sk_add(struct net_device *netdev,
		   struct sock *sk,
		   struct ulp_ddp_config *config,
		   const struct ulp_ddp_ulp_ops *ops);

void ulp_ddp_sk_del(struct net_device *netdev,
		    struct sock *sk);

bool ulp_ddp_query_limits(struct net_device *netdev,
			  struct ulp_ddp_limits *limits,
			  enum ulp_ddp_type type,
			  int cap_bit_nr,
			  bool tls);

bool ulp_ddp_is_cap_active(struct net_device *netdev, int cap_bit_nr);

#else

static inline int ulp_ddp_sk_add(struct net_device *netdev,
				 struct sock *sk,
				 struct ulp_ddp_config *config,
				 const struct ulp_ddp_ulp_ops *ops)
{
	return -EOPNOTSUPP;
}

static inline void ulp_ddp_sk_del(struct net_device *netdev,
				  struct sock *sk)
{}

static inline bool ulp_ddp_query_limits(struct net_device *netdev,
					struct ulp_ddp_limits *limits,
					enum ulp_ddp_type type,
					int cap_bit_nr,
					bool tls)
{
	return false;
}

bool ulp_ddp_is_cap_active(struct net_device *netdev, int cap_bit_nr)
{
	return false;
}

#endif

#endif	/* _ULP_DDP_H */
