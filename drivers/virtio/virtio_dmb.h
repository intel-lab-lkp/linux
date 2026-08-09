/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Device Memory Buffer support for virtio devices.
 */
#ifndef _DRIVERS_VIRTIO_VIRTIO_DMB_H
#define _DRIVERS_VIRTIO_VIRTIO_DMB_H

struct virtio_device;

#if IS_ENABLED(CONFIG_VIRTIO_DMB)

int virtio_dmb_init(struct virtio_device *vdev);
void virtio_dmb_destroy(struct virtio_device *vdev);
void virtio_dmb_note_vqs(struct virtio_device *vdev, unsigned int nvqs);

#else

static inline int virtio_dmb_init(struct virtio_device *vdev)
{
	return 0;
}

static inline void virtio_dmb_destroy(struct virtio_device *vdev)
{
}

static inline void virtio_dmb_note_vqs(struct virtio_device *vdev,
				       unsigned int nvqs)
{
}

#endif /* CONFIG_VIRTIO_DMB */

#endif /* _DRIVERS_VIRTIO_VIRTIO_DMB_H */
