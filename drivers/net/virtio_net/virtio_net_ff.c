// SPDX-License-Identifier: GPL-2.0-only

#include <linux/virtio_admin.h>
#include <linux/virtio.h>
#include <net/ipv6.h>
#include <net/ip.h>
#include "virtio_net_ff.h"

static size_t get_mask_size(u16 type)
{
	switch (type) {
	case VIRTIO_NET_FF_MASK_TYPE_ETH:
		return sizeof(struct ethhdr);
	case VIRTIO_NET_FF_MASK_TYPE_IPV4:
		return sizeof(struct iphdr);
	case VIRTIO_NET_FF_MASK_TYPE_IPV6:
		return sizeof(struct ipv6hdr);
	case VIRTIO_NET_FF_MASK_TYPE_TCP:
		return sizeof(struct tcphdr);
	case VIRTIO_NET_FF_MASK_TYPE_UDP:
		return sizeof(struct udphdr);
	}

	return 0;
}

void virtnet_ff_init(struct virtnet_ff *ff, struct virtio_device *vdev)
{
	struct virtio_admin_cmd_query_cap_id_result *cap_id_list __free(kfree) = NULL;
	size_t ff_mask_size = sizeof(struct virtio_net_ff_cap_mask_data) +
			      sizeof(struct virtio_net_ff_selector) *
			      VIRTIO_NET_FF_MASK_TYPE_MAX;
	struct virtio_net_ff_selector *sel;
	int err;
	int i;

	cap_id_list = kzalloc(sizeof(*cap_id_list), GFP_KERNEL);
	if (!cap_id_list)
		return;

	err = virtio_device_cap_id_list_query(vdev, cap_id_list);
	if (err)
		return;

	if (!(VIRTIO_CAP_IN_LIST(cap_id_list,
				 VIRTIO_NET_FF_RESOURCE_CAP) &&
	      VIRTIO_CAP_IN_LIST(cap_id_list,
				 VIRTIO_NET_FF_SELECTOR_CAP) &&
	      VIRTIO_CAP_IN_LIST(cap_id_list,
				 VIRTIO_NET_FF_ACTION_CAP)))
		return;

	ff->ff_caps = kzalloc(sizeof(*ff->ff_caps), GFP_KERNEL);
	if (!ff->ff_caps)
		return;

	err = virtio_device_cap_get(vdev,
				    VIRTIO_NET_FF_RESOURCE_CAP,
				    ff->ff_caps,
				    sizeof(*ff->ff_caps));

	if (err)
		goto err_ff;

	/* VIRTIO_NET_FF_MASK_TYPE start at 1 */
	for (i = 1; i <= VIRTIO_NET_FF_MASK_TYPE_MAX; i++)
		ff_mask_size += get_mask_size(i);

	ff->ff_mask = kzalloc(ff_mask_size, GFP_KERNEL);
	if (!ff->ff_mask)
		goto err_ff;

	err = virtio_device_cap_get(vdev,
				    VIRTIO_NET_FF_SELECTOR_CAP,
				    ff->ff_mask,
				    ff_mask_size);

	if (err)
		goto err_ff_mask;

	ff->ff_actions = kzalloc(sizeof(*ff->ff_actions) +
					VIRTIO_NET_FF_ACTION_MAX,
					GFP_KERNEL);
	if (!ff->ff_actions)
		goto err_ff_mask;

	err = virtio_device_cap_get(vdev,
				    VIRTIO_NET_FF_ACTION_CAP,
				    ff->ff_actions,
				    sizeof(*ff->ff_actions) + VIRTIO_NET_FF_ACTION_MAX);

	if (err)
		goto err_ff_action;

	err = virtio_device_cap_set(vdev,
				    VIRTIO_NET_FF_RESOURCE_CAP,
				    ff->ff_caps,
				    sizeof(*ff->ff_caps));
	if (err)
		goto err_ff_action;

	ff_mask_size = sizeof(struct virtio_net_ff_cap_mask_data);
	sel = &ff->ff_mask->selectors[0];

	for (int i = 0; i < ff->ff_mask->count; i++) {
		ff_mask_size += sizeof(struct virtio_net_ff_selector) + sel->length;
		sel = (struct virtio_net_ff_selector *)((u8 *)sel + sizeof(*sel) + sel->length);
	}

	err = virtio_device_cap_set(vdev,
				    VIRTIO_NET_FF_SELECTOR_CAP,
				    ff->ff_mask,
				    ff_mask_size);
	if (err)
		goto err_ff_action;

	err = virtio_device_cap_set(vdev,
				    VIRTIO_NET_FF_ACTION_CAP,
				    ff->ff_actions,
				    sizeof(*ff->ff_actions) + VIRTIO_NET_FF_ACTION_MAX);
	if (err)
		goto err_ff_action;

	ff->vdev = vdev;
	ff->ff_supported = true;

	return;

err_ff_action:
	kfree(ff->ff_actions);
err_ff_mask:
	kfree(ff->ff_mask);
err_ff:
	kfree(ff->ff_caps);
}

void virtnet_ff_cleanup(struct virtnet_ff *ff)
{
	if (!ff->ff_supported)
		return;

	kfree(ff->ff_actions);
	kfree(ff->ff_mask);
	kfree(ff->ff_caps);
}
