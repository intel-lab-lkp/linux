/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause) */
/* Do not edit directly, auto-generated from: */
/*	Documentation/netlink/specs/netdev.yaml */
/* YNL-GEN uapi header */

#ifndef _UAPI_LINUX_NETDEV_H
#define _UAPI_LINUX_NETDEV_H

#define NETDEV_FAMILY_NAME	"netdev"
#define NETDEV_FAMILY_VERSION	1

/**
 * enum netdev_xdp_act
 * @NETDEV_XDP_ACT_BASIC: XDP features set supported by all drivers
 *   (XDP_ABORTED, XDP_DROP, XDP_PASS, XDP_TX)
 * @NETDEV_XDP_ACT_REDIRECT: The netdev supports XDP_REDIRECT
 * @NETDEV_XDP_ACT_NDO_XMIT: This feature informs if netdev implements
 *   ndo_xdp_xmit callback.
 * @NETDEV_XDP_ACT_XSK_ZEROCOPY: This feature informs if netdev supports AF_XDP
 *   in zero copy mode.
 * @NETDEV_XDP_ACT_HW_OFFLOAD: This feature informs if netdev supports XDP hw
 *   offloading.
 * @NETDEV_XDP_ACT_RX_SG: This feature informs if netdev implements non-linear
 *   XDP buffer support in the driver napi callback.
 * @NETDEV_XDP_ACT_NDO_XMIT_SG: This feature informs if netdev implements
 *   non-linear XDP buffer support in ndo_xdp_xmit callback.
 */
enum netdev_xdp_act {
	NETDEV_XDP_ACT_BASIC = 1,
	NETDEV_XDP_ACT_REDIRECT = 2,
	NETDEV_XDP_ACT_NDO_XMIT = 4,
	NETDEV_XDP_ACT_XSK_ZEROCOPY = 8,
	NETDEV_XDP_ACT_HW_OFFLOAD = 16,
	NETDEV_XDP_ACT_RX_SG = 32,
	NETDEV_XDP_ACT_NDO_XMIT_SG = 64,

	/* private: */
	NETDEV_XDP_ACT_MASK = 127,
};

/**
 * enum netdev_xdp_rx_metadata
 * @NETDEV_XDP_RX_METADATA_TIMESTAMP: Device is capable of exposing receive HW
 *   timestamp via bpf_xdp_metadata_rx_timestamp().
 * @NETDEV_XDP_RX_METADATA_HASH: Device is capable of exposing receive packet
 *   hash via bpf_xdp_metadata_rx_hash().
 * @NETDEV_XDP_RX_METADATA_VLAN_TAG: Device is capable of exposing receive
 *   packet VLAN tag via bpf_xdp_metadata_rx_vlan_tag().
 */
enum netdev_xdp_rx_metadata {
	NETDEV_XDP_RX_METADATA_TIMESTAMP = 1,
	NETDEV_XDP_RX_METADATA_HASH = 2,
	NETDEV_XDP_RX_METADATA_VLAN_TAG = 4,
};

/**
 * enum netdev_xsk_flags
 * @NETDEV_XSK_FLAGS_TX_TIMESTAMP: HW timestamping egress packets is supported
 *   by the driver.
 * @NETDEV_XSK_FLAGS_TX_CHECKSUM: L3 checksum HW offload is supported by the
 *   driver.
 * @NETDEV_XSK_FLAGS_TX_LAUNCH_TIME_FIFO: Launch time HW offload is supported
 *   by the driver.
 */
enum netdev_xsk_flags {
	NETDEV_XSK_FLAGS_TX_TIMESTAMP = 1,
	NETDEV_XSK_FLAGS_TX_CHECKSUM = 2,
	NETDEV_XSK_FLAGS_TX_LAUNCH_TIME_FIFO = 4,
};

enum netdev_queue_type {
	NETDEV_QUEUE_TYPE_RX,
	NETDEV_QUEUE_TYPE_TX,
};

enum netdev_qstats_scope {
	NETDEV_QSTATS_SCOPE_QUEUE = 1,
};

enum netdev_napi_threaded {
	NETDEV_NAPI_THREADED_DISABLED,
	NETDEV_NAPI_THREADED_ENABLED,
};

/**
 * enum netdev_dev
 * @NETDEV_A_DEV_IFINDEX: netdev ifindex
 * @NETDEV_A_DEV_XDP_FEATURES: Bitmask of enabled xdp-features.
 * @NETDEV_A_DEV_XDP_ZC_MAX_SEGS: max fragment count supported by ZC driver
 * @NETDEV_A_DEV_XDP_RX_METADATA_FEATURES: Bitmask of supported XDP receive
 *   metadata features. See Documentation/networking/xdp-rx-metadata.rst for
 *   more details.
 * @NETDEV_A_DEV_XSK_FEATURES: Bitmask of enabled AF_XDP features.
 */
enum {
	NETDEV_A_DEV_IFINDEX = 1,
	NETDEV_A_DEV_PAD,
	NETDEV_A_DEV_XDP_FEATURES,
	NETDEV_A_DEV_XDP_ZC_MAX_SEGS,
	NETDEV_A_DEV_XDP_RX_METADATA_FEATURES,
	NETDEV_A_DEV_XSK_FEATURES,

	__NETDEV_A_DEV_MAX,
	NETDEV_A_DEV_MAX = (__NETDEV_A_DEV_MAX - 1)
};

enum {
	__NETDEV_A_IO_URING_PROVIDER_INFO_MAX,
	NETDEV_A_IO_URING_PROVIDER_INFO_MAX = (__NETDEV_A_IO_URING_PROVIDER_INFO_MAX - 1)
};

/**
 * enum netdev_page_pool
 * @NETDEV_A_PAGE_POOL_ID: Unique ID of a Page Pool instance.
 * @NETDEV_A_PAGE_POOL_IFINDEX: ifindex of the netdev to which the pool
 *   belongs. May be reported as 0 if the page pool was allocated for a netdev
 *   which got destroyed already (page pools may outlast their netdevs because
 *   they wait for all memory to be returned).
 * @NETDEV_A_PAGE_POOL_NAPI_ID: Id of NAPI using this Page Pool instance.
 * @NETDEV_A_PAGE_POOL_INFLIGHT: Number of outstanding references to this page
 *   pool (allocated but yet to be freed pages). Allocated pages may be held in
 *   socket receive queues, driver receive ring, page pool recycling ring, the
 *   page pool cache, etc.
 * @NETDEV_A_PAGE_POOL_INFLIGHT_MEM: Amount of memory held by inflight pages.
 * @NETDEV_A_PAGE_POOL_DETACH_TIME: Seconds in CLOCK_BOOTTIME of when Page Pool
 *   was detached by the driver. Once detached Page Pool can no longer be used
 *   to allocate memory. Page Pools wait for all the memory allocated from them
 *   to be freed before truly disappearing. "Detached" Page Pools cannot be
 *   "re-attached", they are just waiting to disappear. Attribute is absent if
 *   Page Pool has not been detached, and can still be used to allocate new
 *   memory.
 * @NETDEV_A_PAGE_POOL_DMABUF: ID of the dmabuf this page-pool is attached to.
 * @NETDEV_A_PAGE_POOL_IO_URING: io-uring memory provider information.
 */
enum {
	NETDEV_A_PAGE_POOL_ID = 1,
	NETDEV_A_PAGE_POOL_IFINDEX,
	NETDEV_A_PAGE_POOL_NAPI_ID,
	NETDEV_A_PAGE_POOL_INFLIGHT,
	NETDEV_A_PAGE_POOL_INFLIGHT_MEM,
	NETDEV_A_PAGE_POOL_DETACH_TIME,
	NETDEV_A_PAGE_POOL_DMABUF,
	NETDEV_A_PAGE_POOL_IO_URING,

	__NETDEV_A_PAGE_POOL_MAX,
	NETDEV_A_PAGE_POOL_MAX = (__NETDEV_A_PAGE_POOL_MAX - 1)
};

/**
 * enum netdev_page_pool_stats - Page pool statistics, see docs for struct
 *   page_pool_stats for information about individual statistics.
 * @NETDEV_A_PAGE_POOL_STATS_INFO: Page pool identifying information.
 */
enum {
	NETDEV_A_PAGE_POOL_STATS_INFO = 1,
	NETDEV_A_PAGE_POOL_STATS_ALLOC_FAST = 8,
	NETDEV_A_PAGE_POOL_STATS_ALLOC_SLOW,
	NETDEV_A_PAGE_POOL_STATS_ALLOC_SLOW_HIGH_ORDER,
	NETDEV_A_PAGE_POOL_STATS_ALLOC_EMPTY,
	NETDEV_A_PAGE_POOL_STATS_ALLOC_REFILL,
	NETDEV_A_PAGE_POOL_STATS_ALLOC_WAIVE,
	NETDEV_A_PAGE_POOL_STATS_RECYCLE_CACHED,
	NETDEV_A_PAGE_POOL_STATS_RECYCLE_CACHE_FULL,
	NETDEV_A_PAGE_POOL_STATS_RECYCLE_RING,
	NETDEV_A_PAGE_POOL_STATS_RECYCLE_RING_FULL,
	NETDEV_A_PAGE_POOL_STATS_RECYCLE_RELEASED_REFCNT,

	__NETDEV_A_PAGE_POOL_STATS_MAX,
	NETDEV_A_PAGE_POOL_STATS_MAX = (__NETDEV_A_PAGE_POOL_STATS_MAX - 1)
};

/**
 * enum netdev_napi
 * @NETDEV_A_NAPI_IFINDEX: ifindex of the netdevice to which NAPI instance
 *   belongs.
 * @NETDEV_A_NAPI_ID: ID of the NAPI instance.
 * @NETDEV_A_NAPI_IRQ: The associated interrupt vector number for the napi
 * @NETDEV_A_NAPI_PID: PID of the napi thread, if NAPI is configured to operate
 *   in threaded mode. If NAPI is not in threaded mode (i.e. uses normal
 *   softirq context), the attribute will be absent.
 * @NETDEV_A_NAPI_DEFER_HARD_IRQS: The number of consecutive empty polls before
 *   IRQ deferral ends and hardware IRQs are re-enabled.
 * @NETDEV_A_NAPI_GRO_FLUSH_TIMEOUT: The timeout, in nanoseconds, of when to
 *   trigger the NAPI watchdog timer which schedules NAPI processing.
 *   Additionally, a non-zero value will also prevent GRO from flushing recent
 *   super-frames at the end of a NAPI cycle. This may add receive latency in
 *   exchange for reducing the number of frames processed by the network stack.
 * @NETDEV_A_NAPI_IRQ_SUSPEND_TIMEOUT: The timeout, in nanoseconds, of how long
 *   to suspend irq processing, if event polling finds events
 * @NETDEV_A_NAPI_THREADED: Whether the NAPI is configured to operate in
 *   threaded polling mode. If this is set to enabled then the NAPI context
 *   operates in threaded polling mode.
 */
enum {
	NETDEV_A_NAPI_IFINDEX = 1,
	NETDEV_A_NAPI_ID,
	NETDEV_A_NAPI_IRQ,
	NETDEV_A_NAPI_PID,
	NETDEV_A_NAPI_DEFER_HARD_IRQS,
	NETDEV_A_NAPI_GRO_FLUSH_TIMEOUT,
	NETDEV_A_NAPI_IRQ_SUSPEND_TIMEOUT,
	NETDEV_A_NAPI_THREADED,

	__NETDEV_A_NAPI_MAX,
	NETDEV_A_NAPI_MAX = (__NETDEV_A_NAPI_MAX - 1)
};

enum {
	__NETDEV_A_XSK_INFO_MAX,
	NETDEV_A_XSK_INFO_MAX = (__NETDEV_A_XSK_INFO_MAX - 1)
};

/**
 * enum netdev_queue
 * @NETDEV_A_QUEUE_ID: Queue index; most queue types are indexed like a C
 *   array, with indexes starting at 0 and ending at queue count - 1. Queue
 *   indexes are scoped to an interface and queue type.
 * @NETDEV_A_QUEUE_IFINDEX: ifindex of the netdevice to which the queue
 *   belongs.
 * @NETDEV_A_QUEUE_TYPE: Queue type as rx, tx. Each queue type defines a
 *   separate ID space. XDP TX queues allocated in the kernel are not linked to
 *   NAPIs and thus not listed. AF_XDP queues will have more information set in
 *   the xsk attribute.
 * @NETDEV_A_QUEUE_NAPI_ID: ID of the NAPI instance which services this queue.
 * @NETDEV_A_QUEUE_DMABUF: ID of the dmabuf attached to this queue, if any.
 * @NETDEV_A_QUEUE_IO_URING: io_uring memory provider information.
 * @NETDEV_A_QUEUE_XSK: XSK information for this queue, if any.
 */
enum {
	NETDEV_A_QUEUE_ID = 1,
	NETDEV_A_QUEUE_IFINDEX,
	NETDEV_A_QUEUE_TYPE,
	NETDEV_A_QUEUE_NAPI_ID,
	NETDEV_A_QUEUE_DMABUF,
	NETDEV_A_QUEUE_IO_URING,
	NETDEV_A_QUEUE_XSK,

	__NETDEV_A_QUEUE_MAX,
	NETDEV_A_QUEUE_MAX = (__NETDEV_A_QUEUE_MAX - 1)
};

/**
 * enum netdev_qstats - Get device statistics, scoped to a device or a queue.
 *   These statistics extend (and partially duplicate) statistics available in
 *   struct rtnl_link_stats64. Value of the `scope` attribute determines how
 *   statistics are aggregated. When aggregated for the entire device the
 *   statistics represent the total number of events since last explicit reset
 *   of the device (i.e. not a reconfiguration like changing queue count). When
 *   reported per-queue, however, the statistics may not add up to the total
 *   number of events, will only be reported for currently active objects, and
 *   will likely report the number of events since last reconfiguration.
 * @NETDEV_A_QSTATS_IFINDEX: ifindex of the netdevice to which stats belong.
 * @NETDEV_A_QSTATS_QUEUE_TYPE: Queue type as rx, tx, for queue-id.
 * @NETDEV_A_QSTATS_QUEUE_ID: Queue ID, if stats are scoped to a single queue
 *   instance.
 * @NETDEV_A_QSTATS_SCOPE: What object type should be used to iterate over the
 *   stats.
 * @NETDEV_A_QSTATS_RX_PACKETS: Number of wire packets successfully received
 *   and passed to the stack. For drivers supporting XDP, XDP is considered the
 *   first layer of the stack, so packets consumed by XDP are still counted
 *   here.
 * @NETDEV_A_QSTATS_RX_BYTES: Successfully received bytes, see `rx-packets`.
 * @NETDEV_A_QSTATS_TX_PACKETS: Number of wire packets successfully sent.
 *   Packet is considered to be successfully sent once it is in device memory
 *   (usually this means the device has issued a DMA completion for the
 *   packet).
 * @NETDEV_A_QSTATS_TX_BYTES: Successfully sent bytes, see `tx-packets`.
 * @NETDEV_A_QSTATS_RX_ALLOC_FAIL: Number of times skb or buffer allocation
 *   failed on the Rx datapath. Allocation failure may, or may not result in a
 *   packet drop, depending on driver implementation and whether system
 *   recovers quickly.
 * @NETDEV_A_QSTATS_RX_HW_DROPS: Number of all packets which entered the
 *   device, but never left it, including but not limited to: packets dropped
 *   due to lack of buffer space, processing errors, explicit or implicit
 *   policies and packet filters.
 * @NETDEV_A_QSTATS_RX_HW_DROP_OVERRUNS: Number of packets dropped due to
 *   transient lack of resources, such as buffer space, host descriptors etc.
 * @NETDEV_A_QSTATS_RX_CSUM_COMPLETE: Number of packets that were marked as
 *   CHECKSUM_COMPLETE.
 * @NETDEV_A_QSTATS_RX_CSUM_UNNECESSARY: Number of packets that were marked as
 *   CHECKSUM_UNNECESSARY.
 * @NETDEV_A_QSTATS_RX_CSUM_NONE: Number of packets that were not checksummed
 *   by device.
 * @NETDEV_A_QSTATS_RX_CSUM_BAD: Number of packets with bad checksum. The
 *   packets are not discarded, but still delivered to the stack.
 * @NETDEV_A_QSTATS_RX_HW_GRO_PACKETS: Number of packets that were coalesced
 *   from smaller packets by the device. Counts only packets coalesced with the
 *   HW-GRO netdevice feature, LRO-coalesced packets are not counted.
 * @NETDEV_A_QSTATS_RX_HW_GRO_BYTES: See `rx-hw-gro-packets`.
 * @NETDEV_A_QSTATS_RX_HW_GRO_WIRE_PACKETS: Number of packets that were
 *   coalesced to bigger packetss with the HW-GRO netdevice feature.
 *   LRO-coalesced packets are not counted.
 * @NETDEV_A_QSTATS_RX_HW_GRO_WIRE_BYTES: See `rx-hw-gro-wire-packets`.
 * @NETDEV_A_QSTATS_RX_HW_DROP_RATELIMITS: Number of the packets dropped by the
 *   device due to the received packets bitrate exceeding the device rate
 *   limit.
 * @NETDEV_A_QSTATS_TX_HW_DROPS: Number of packets that arrived at the device
 *   but never left it, encompassing packets dropped for reasons such as
 *   processing errors, as well as those affected by explicitly defined
 *   policies and packet filtering criteria.
 * @NETDEV_A_QSTATS_TX_HW_DROP_ERRORS: Number of packets dropped because they
 *   were invalid or malformed.
 * @NETDEV_A_QSTATS_TX_CSUM_NONE: Number of packets that did not require the
 *   device to calculate the checksum.
 * @NETDEV_A_QSTATS_TX_NEEDS_CSUM: Number of packets that required the device
 *   to calculate the checksum. This counter includes the number of GSO wire
 *   packets for which device calculated the L4 checksum.
 * @NETDEV_A_QSTATS_TX_HW_GSO_PACKETS: Number of packets that necessitated
 *   segmentation into smaller packets by the device.
 * @NETDEV_A_QSTATS_TX_HW_GSO_BYTES: See `tx-hw-gso-packets`.
 * @NETDEV_A_QSTATS_TX_HW_GSO_WIRE_PACKETS: Number of wire-sized packets
 *   generated by processing `tx-hw-gso-packets`
 * @NETDEV_A_QSTATS_TX_HW_GSO_WIRE_BYTES: See `tx-hw-gso-wire-packets`.
 * @NETDEV_A_QSTATS_TX_HW_DROP_RATELIMITS: Number of the packets dropped by the
 *   device due to the transmit packets bitrate exceeding the device rate
 *   limit.
 * @NETDEV_A_QSTATS_TX_STOP: Number of times driver paused accepting new tx
 *   packets from the stack to this queue, because the queue was full. Note
 *   that if BQL is supported and enabled on the device the networking stack
 *   will avoid queuing a lot of data at once.
 * @NETDEV_A_QSTATS_TX_WAKE: Number of times driver re-started accepting send
 *   requests to this queue from the stack.
 */
enum {
	NETDEV_A_QSTATS_IFINDEX = 1,
	NETDEV_A_QSTATS_QUEUE_TYPE,
	NETDEV_A_QSTATS_QUEUE_ID,
	NETDEV_A_QSTATS_SCOPE,
	NETDEV_A_QSTATS_RX_PACKETS = 8,
	NETDEV_A_QSTATS_RX_BYTES,
	NETDEV_A_QSTATS_TX_PACKETS,
	NETDEV_A_QSTATS_TX_BYTES,
	NETDEV_A_QSTATS_RX_ALLOC_FAIL,
	NETDEV_A_QSTATS_RX_HW_DROPS,
	NETDEV_A_QSTATS_RX_HW_DROP_OVERRUNS,
	NETDEV_A_QSTATS_RX_CSUM_COMPLETE,
	NETDEV_A_QSTATS_RX_CSUM_UNNECESSARY,
	NETDEV_A_QSTATS_RX_CSUM_NONE,
	NETDEV_A_QSTATS_RX_CSUM_BAD,
	NETDEV_A_QSTATS_RX_HW_GRO_PACKETS,
	NETDEV_A_QSTATS_RX_HW_GRO_BYTES,
	NETDEV_A_QSTATS_RX_HW_GRO_WIRE_PACKETS,
	NETDEV_A_QSTATS_RX_HW_GRO_WIRE_BYTES,
	NETDEV_A_QSTATS_RX_HW_DROP_RATELIMITS,
	NETDEV_A_QSTATS_TX_HW_DROPS,
	NETDEV_A_QSTATS_TX_HW_DROP_ERRORS,
	NETDEV_A_QSTATS_TX_CSUM_NONE,
	NETDEV_A_QSTATS_TX_NEEDS_CSUM,
	NETDEV_A_QSTATS_TX_HW_GSO_PACKETS,
	NETDEV_A_QSTATS_TX_HW_GSO_BYTES,
	NETDEV_A_QSTATS_TX_HW_GSO_WIRE_PACKETS,
	NETDEV_A_QSTATS_TX_HW_GSO_WIRE_BYTES,
	NETDEV_A_QSTATS_TX_HW_DROP_RATELIMITS,
	NETDEV_A_QSTATS_TX_STOP,
	NETDEV_A_QSTATS_TX_WAKE,

	__NETDEV_A_QSTATS_MAX,
	NETDEV_A_QSTATS_MAX = (__NETDEV_A_QSTATS_MAX - 1)
};

/**
 * enum netdev_dmabuf
 * @NETDEV_A_DMABUF_IFINDEX: netdev ifindex to bind the dmabuf to.
 * @NETDEV_A_DMABUF_QUEUES: receive queues to bind the dmabuf to.
 * @NETDEV_A_DMABUF_FD: dmabuf file descriptor to bind.
 * @NETDEV_A_DMABUF_ID: id of the dmabuf binding
 */
enum {
	NETDEV_A_DMABUF_IFINDEX = 1,
	NETDEV_A_DMABUF_QUEUES,
	NETDEV_A_DMABUF_FD,
	NETDEV_A_DMABUF_ID,

	__NETDEV_A_DMABUF_MAX,
	NETDEV_A_DMABUF_MAX = (__NETDEV_A_DMABUF_MAX - 1)
};

enum {
	NETDEV_CMD_DEV_GET = 1,
	NETDEV_CMD_DEV_ADD_NTF,
	NETDEV_CMD_DEV_DEL_NTF,
	NETDEV_CMD_DEV_CHANGE_NTF,
	NETDEV_CMD_PAGE_POOL_GET,
	NETDEV_CMD_PAGE_POOL_ADD_NTF,
	NETDEV_CMD_PAGE_POOL_DEL_NTF,
	NETDEV_CMD_PAGE_POOL_CHANGE_NTF,
	NETDEV_CMD_PAGE_POOL_STATS_GET,
	NETDEV_CMD_QUEUE_GET,
	NETDEV_CMD_NAPI_GET,
	NETDEV_CMD_QSTATS_GET,
	NETDEV_CMD_BIND_RX,
	NETDEV_CMD_NAPI_SET,
	NETDEV_CMD_BIND_TX,

	__NETDEV_CMD_MAX,
	NETDEV_CMD_MAX = (__NETDEV_CMD_MAX - 1)
};

#define NETDEV_MCGRP_MGMT	"mgmt"
#define NETDEV_MCGRP_PAGE_POOL	"page-pool"

#endif /* _UAPI_LINUX_NETDEV_H */
