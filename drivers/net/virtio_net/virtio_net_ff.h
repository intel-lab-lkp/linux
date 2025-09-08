/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Header file for virtio_net flow filters
 */
#include <linux/virtio_admin.h>

#ifndef _VIRTIO_NET_FF_H
#define _VIRTIO_NET_FF_H

struct virtnet_ff {
	struct virtio_device *vdev;
	bool ff_supported;
	struct virtio_net_ff_cap_data *ff_caps;
	struct virtio_net_ff_cap_mask_data *ff_mask;
	struct virtio_net_ff_actions *ff_actions;
};

void virtnet_ff_init(struct virtnet_ff *ff, struct virtio_device *vdev);

void virtnet_ff_cleanup(struct virtnet_ff *ff);

#endif /* _VIRTIO_NET_FF_H */
