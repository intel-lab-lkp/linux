// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/virtio.h>
#include "virtio_pci_common.h"

static u16 vp_modern_avq_num(struct virtio_pci_modern_device *mdev)
{
	struct virtio_pci_modern_common_cfg __iomem *cfg;

	cfg = (struct virtio_pci_modern_common_cfg __iomem *)mdev->common;
	return vp_ioread16(&cfg->admin_queue_num);
}

static u16 vp_modern_avq_index(struct virtio_pci_modern_device *mdev)
{
	struct virtio_pci_modern_common_cfg __iomem *cfg;

	cfg = (struct virtio_pci_modern_common_cfg __iomem *)mdev->common;
	return vp_ioread16(&cfg->admin_queue_index);
}

int vp_create_avq(struct virtio_device *vdev)
{
	struct virtio_pci_device *vp_dev = to_vp_device(vdev);
	struct virtio_avq *avq;
	struct virtqueue *vq;
	u16 admin_q_num;

	if (!virtio_has_feature(vdev, VIRTIO_F_ADMIN_VQ))
		return 0;

	admin_q_num = vp_modern_avq_num(&vp_dev->mdev);
	if (!admin_q_num)
		return -EINVAL;

	vp_dev->admin = kzalloc(sizeof(*vp_dev->admin), GFP_KERNEL);
	if (!vp_dev->admin)
		return -ENOMEM;

	avq = vp_dev->admin;
	avq->vq_index = vp_modern_avq_index(&vp_dev->mdev);
	sprintf(avq->name, "avq.%u", avq->vq_index);
	vq = vp_dev->setup_vq(vp_dev, &vp_dev->admin->info, avq->vq_index, NULL,
			      avq->name, NULL, VIRTIO_MSI_NO_VECTOR);
	if (IS_ERR(vq)) {
		dev_err(&vdev->dev, "failed to setup admin virtqueue");
		kfree(vp_dev->admin);
		return PTR_ERR(vq);
	}

	vp_dev->admin->info.vq = vq;
	vp_modern_set_queue_enable(&vp_dev->mdev, avq->info.vq->index, true);
	return 0;
}

void vp_destroy_avq(struct virtio_device *vdev)
{
	struct virtio_pci_device *vp_dev = to_vp_device(vdev);

	if (!vp_dev->admin)
		return;

	vp_dev->del_vq(&vp_dev->admin->info);
	kfree(vp_dev->admin);
}
