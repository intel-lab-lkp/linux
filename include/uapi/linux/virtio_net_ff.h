/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note
 *
 * Header file for virtio_net flow filters
 */
#ifndef _LINUX_VIRTIO_NET_FF_H
#define _LINUX_VIRTIO_NET_FF_H

#include <linux/types.h>
#include <linux/kernel.h>

#define VIRTIO_NET_FF_RESOURCE_CAP 0x800
#define VIRTIO_NET_FF_SELECTOR_CAP 0x801
#define VIRTIO_NET_FF_ACTION_CAP 0x802

#define VIRTIO_NET_RESOURCE_OBJ_FF_GROUP 0x0200
#define VIRTIO_NET_RESOURCE_OBJ_FF_CLASSIFIER 0x0201
#define VIRTIO_NET_RESOURCE_OBJ_FF_RULE 0x0202

struct virtio_net_ff_cap_data {
	__le32 groups_limit;
	__le32 classifiers_limit;
	__le32 rules_limit;
	__le32 rules_per_group_limit;
	__u8 last_rule_priority;
	__u8 selectors_per_classifier_limit;
};

struct virtio_net_ff_selector {
	__u8 type;
	__u8 flags;
	__u8 reserved[2];
	__u8 length;
	__u8 reserved1[3];
	__u8 mask[];
};

#define VIRTIO_NET_FF_MASK_TYPE_ETH  1
#define VIRTIO_NET_FF_MASK_TYPE_IPV4 2
#define VIRTIO_NET_FF_MASK_TYPE_IPV6 3
#define VIRTIO_NET_FF_MASK_TYPE_TCP  4
#define VIRTIO_NET_FF_MASK_TYPE_UDP  5
#define VIRTIO_NET_FF_MASK_TYPE_MAX  VIRTIO_NET_FF_MASK_TYPE_UDP

struct virtio_net_ff_cap_mask_data {
	__u8 count;
	__u8 reserved[7];
	struct virtio_net_ff_selector selectors[];
};
#define VIRTIO_NET_FF_MASK_F_PARTIAL_MASK (1 << 0)

#define VIRTIO_NET_FF_ACTION_DROP 1
#define VIRTIO_NET_FF_ACTION_RX_VQ 2
#define VIRTIO_NET_FF_ACTION_MAX  VIRTIO_NET_FF_ACTION_RX_VQ
struct virtio_net_ff_actions {
	__u8 count;
	__u8 reserved[7];
	__u8 actions[];
};

struct virtio_net_resource_obj_ff_group {
	__le16 group_priority;
};

struct virtio_net_resource_obj_ff_classifier {
	__u8 count;
	__u8 reserved[7];
	struct virtio_net_ff_selector selectors[];
};

struct virtio_net_resource_obj_ff_rule {
	__le32 group_id;
	__le32 classifier_id;
	__u8 rule_priority;
	__u8 key_length; /* length of key in bytes */
	__u8 action;
	__u8 reserved;
	__le16 vq_index;
	__u8 reserved1[2];
	__u8 keys[];
};

#endif
