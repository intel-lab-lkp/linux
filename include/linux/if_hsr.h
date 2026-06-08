/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_IF_HSR_H_
#define _LINUX_IF_HSR_H_

#include <linux/types.h>

struct net_device;

/* used to differentiate various protocols */
enum hsr_version {
	HSR_V0 = 0,
	HSR_V1,
	PRP_V1,
};

enum hsr_port_type {
	HSR_PT_NONE = 0,	/* Must be 0, used by framereg */
	HSR_PT_SLAVE_A,
	HSR_PT_SLAVE_B,
	HSR_PT_INTERLINK,
	HSR_PT_MASTER,
	HSR_PT_PORTS,	/* This must be the last item in the enum */
};

/* HSR Tag.
 * As defined in IEC-62439-3:2010, the HSR tag is really { ethertype = 0x88FB,
 * path, LSDU_size, sequence Nr }. But we let eth_header() create { h_dest,
 * h_source, h_proto = 0x88FB }, and add { path, LSDU_size, sequence Nr,
 * encapsulated protocol } instead.
 *
 * Field names as defined in the IEC:2010 standard for HSR.
 */
struct hsr_tag {
	__be16		path_and_LSDU_size;
	__be16		sequence_nr;
	__be16		encap_proto;
} __packed;

#define HSR_HLEN	6

/**
 * struct hsr_lre_stats - Kernel-internal IEC-62439-3 LRE counter set.
 *
 * This is the buffer type written by ndo_get_offload_stats() when called
 * with attr_id == IFLA_STATS_LINK_XSTATS on an HSR slave device.  Each
 * field maps to one HSR_XSTATS_* netlink attribute.  Fields that the
 * offload driver does not support must be left at the initialised value of
 * ~0ULL; the HSR layer will skip those when building the netlink reply.
 *
 * Per-port suffix: _a = port A (slave 1 / LAN-A),
 *                  _b = port B (slave 2 / LAN-B),
 *                  _c = interlink / application interface.
 *
 * @cnt_tx_a: lreCntTxA - sent HSR/PRP tagged frames on port A.
 * @cnt_tx_b: lreCntTxB - sent HSR/PRP tagged frames on port B.
 * @cnt_tx_c: lreCntTxC - sent HSR/PRP tagged frames on port C.
 * @cnt_rx_a: lreCntRxA - received HSR/PRP tagged frames on port A.
 * @cnt_rx_b: lreCntRxB - received HSR/PRP tagged frames on port B.
 * @cnt_rx_c: lreCntRxC - received HSR/PRP tagged frames on port C.
 * @cnt_err_wrong_lan_a: lreCntErrWrongLanA - wrong LAN ID frames on port A.
 * @cnt_err_wrong_lan_b: lreCntErrWrongLanB - wrong LAN ID frames on port B.
 * @cnt_err_wrong_lan_c: lreCntErrWrongLanC - wrong LAN ID frames on port C.
 * @cnt_errors_a: lreCntErrorsA - received frames with errors on port A.
 * @cnt_errors_b: lreCntErrorsB - received frames with errors on port B.
 * @cnt_errors_c: lreCntErrorsC - received frames with errors on port C.
 * @cnt_unique_a: lreCntUniqueA - frames received without duplicate on port A.
 * @cnt_unique_b: lreCntUniqueB - frames received without duplicate on port B.
 * @cnt_unique_c: lreCntUniqueC - frames received without duplicate on port C.
 * @cnt_duplicate_a: lreCntDuplicateA - frames with one duplicate on port A.
 * @cnt_duplicate_b: lreCntDuplicateB - frames with one duplicate on port B.
 * @cnt_duplicate_c: lreCntDuplicateC - frames with one duplicate on port C.
 * @cnt_multi_a: lreCntMultiA - frames with more than one duplicate on port A.
 * @cnt_multi_b: lreCntMultiB - frames with more than one duplicate on port B.
 * @cnt_multi_c: lreCntMultiC - frames with more than one duplicate on port C.
 * @cnt_own_rx_a: lreCntOwnRxA - own-address frames received on port A.
 * @cnt_own_rx_b: lreCntOwnRxB - own-address frames received on port B.
 */
struct hsr_lre_stats {
	u64 cnt_tx_a, cnt_tx_b, cnt_tx_c;
	u64 cnt_rx_a, cnt_rx_b, cnt_rx_c;
	u64 cnt_err_wrong_lan_a, cnt_err_wrong_lan_b, cnt_err_wrong_lan_c;
	u64 cnt_errors_a, cnt_errors_b, cnt_errors_c;
	u64 cnt_unique_a, cnt_unique_b, cnt_unique_c;
	u64 cnt_duplicate_a, cnt_duplicate_b, cnt_duplicate_c;
	u64 cnt_multi_a, cnt_multi_b, cnt_multi_c;
	u64 cnt_own_rx_a, cnt_own_rx_b;
};

#if IS_ENABLED(CONFIG_HSR)
extern bool is_hsr_master(struct net_device *dev);
extern int hsr_get_version(struct net_device *dev, enum hsr_version *ver);
struct net_device *hsr_get_port_ndev(struct net_device *ndev,
				     enum hsr_port_type pt);
int hsr_get_port_type(struct net_device *hsr_dev, struct net_device *dev,
		      enum hsr_port_type *type);
#else
static inline bool is_hsr_master(struct net_device *dev)
{
	return false;
}
static inline int hsr_get_version(struct net_device *dev,
				  enum hsr_version *ver)
{
	return -EINVAL;
}

static inline struct net_device *hsr_get_port_ndev(struct net_device *ndev,
						   enum hsr_port_type pt)
{
	return ERR_PTR(-EINVAL);
}

static inline int hsr_get_port_type(struct net_device *hsr_dev,
				    struct net_device *dev,
				    enum hsr_port_type *type)
{
	return -EINVAL;
}
#endif /* CONFIG_HSR */

#endif /*_LINUX_IF_HSR_H_*/
