/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note
 *
 * Header file for virtio_net flow filters
 */
#ifndef _LINUX_VIRTIO_NET_FF_H
#define _LINUX_VIRTIO_NET_FF_H

#include <linux/types.h>

#define VIRTIO_NET_FF_RESOURCE_CAP 0x800
#define VIRTIO_NET_FF_SELECTOR_CAP 0x801
#define VIRTIO_NET_FF_ACTION_CAP 0x802

/**
 * struct virtio_net_ff_cap_data - Flow filter resource capability limits
 * @groups_limit: maximum number of flow filter groups supported by the device
 * @classifiers_limit: maximum number of classifiers supported by the device
 * @rules_limit: maximum number of rules supported device-wide across all groups
 * @rules_per_group_limit: maximum number of rules allowed in a single group
 * @last_rule_priority: priority value associated with the lowest-priority rule
 * @selectors_per_classifier_limit: maximum selectors allowed in one classifier
 */
struct virtio_net_ff_cap_data {
	__le32 groups_limit;
	__le32 classifiers_limit;
	__le32 rules_limit;
	__le32 rules_per_group_limit;
	__u8 last_rule_priority;
	__u8 selectors_per_classifier_limit;
	__u8 reserved[2];
};

/**
 * struct virtio_net_ff_selector - Selector mask descriptor
 * @type: selector type, one of VIRTIO_NET_FF_MASK_TYPE_* constants
 * @flags: selector flags, see VIRTIO_NET_FF_MASK_F_* constants
 * @reserved: must be set to 0 by the driver and ignored by the device
 * @length: size in bytes of @mask
 * @reserved1: must be set to 0 by the driver and ignored by the device
 * @mask: variable-length mask payload for @type, length given by @length
 *
 * A selector describes a header mask that a classifier can apply. The format
 * of @mask depends on @type.
 */
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

/**
 * struct virtio_net_ff_cap_mask_data - Supported selector mask formats
 * @count: number of entries in @selectors
 * @reserved: must be set to 0 by the driver and ignored by the device
 * @selectors: packed array of struct virtio_net_ff_selectors.
 */
struct virtio_net_ff_cap_mask_data {
	__u8 count;
	__u8 reserved[7];
	__u8 selectors[];
};
#define VIRTIO_NET_FF_MASK_F_PARTIAL_MASK (1 << 0)

#define VIRTIO_NET_FF_ACTION_DROP 1
#define VIRTIO_NET_FF_ACTION_RX_VQ 2
#define VIRTIO_NET_FF_ACTION_MAX  VIRTIO_NET_FF_ACTION_RX_VQ
/**
 * struct virtio_net_ff_actions - Supported flow actions
 * @count: number of supported actions in @actions
 * @reserved: must be set to 0 by the driver and ignored by the device
 * @actions: array of action identifiers (VIRTIO_NET_FF_ACTION_*)
 */
struct virtio_net_ff_actions {
	__u8 count;
	__u8 reserved[7];
	__u8 actions[];
};
#endif
