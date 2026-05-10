// SPDX-License-Identifier: GPL-2.0

#ifdef CONFIG_VIRTIO
#include <linux/virtio_config.h>

__rust_helper bool
rust_helper_virtio_has_feature(const struct virtio_device *vdev,
			       unsigned int fbit)
{
	return virtio_has_feature(vdev, fbit);
}
__rust_helper void rust_helper_virtio_get_features(struct virtio_device *vdev,
						   u64 *features_out)
{
	return virtio_get_features(vdev, features_out);
}

__rust_helper int rust_helper_virtio_find_vqs(struct virtio_device *vdev,
					      unsigned int nvqs,
					      struct virtqueue *vqs[],
					      struct virtqueue_info vqs_info[],
					      struct irq_affinity *desc)
{
	return virtio_find_vqs(vdev, nvqs, vqs, vqs_info, desc);
}

__rust_helper void rust_helper_virtio_device_ready(struct virtio_device *dev)
{
	return virtio_device_ready(dev);
}

__rust_helper bool
rust_helper_virtio_is_little_endian(struct virtio_device *vdev)
{
	return virtio_is_little_endian(vdev);
}
#endif /* CONFIG_VIRTIO */
