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

#define VIRTIO_AVQ_SGS_MAX	4

int vp_avq_cmd_exec(struct virtio_device *vdev, struct virtio_admin_cmd *cmd)
{
	struct scatterlist *sgs[VIRTIO_AVQ_SGS_MAX], hdr, stat;
	struct virtio_pci_device *vp_dev = to_vp_device(vdev);
	struct virtio_admin_cmd_status *va_status;
	unsigned int out_num = 0, in_num = 0;
	struct virtio_admin_cmd_hdr *va_hdr;
	struct virtqueue *avq;
	u16 status;
	int ret;

	avq = vp_dev->admin ? vp_dev->admin->info.vq : NULL;
	if (!avq)
		return -EOPNOTSUPP;

	va_status = kzalloc(sizeof(*va_status), GFP_KERNEL);
	if (!va_status)
		return -ENOMEM;

	va_hdr = kzalloc(sizeof(*va_hdr), GFP_KERNEL);
	if (!va_hdr) {
		ret = -ENOMEM;
		goto err_alloc;
	}

	va_hdr->opcode = cmd->opcode;
	va_hdr->group_type = cmd->group_type;
	va_hdr->group_member_id = cmd->group_member_id;

	/* Add header */
	sg_init_one(&hdr, va_hdr, sizeof(*va_hdr));
	sgs[out_num] = &hdr;
	out_num++;

	if (cmd->data_sg) {
		sgs[out_num] = cmd->data_sg;
		out_num++;
	}

	/* Add return status */
	sg_init_one(&stat, va_status, sizeof(*va_status));
	sgs[out_num + in_num] = &stat;
	in_num++;

	if (cmd->result_sg) {
		sgs[out_num + in_num] = cmd->result_sg;
		in_num++;
	}

	ret = virtqueue_exec_cmd(avq, sgs, out_num, in_num, sgs, GFP_KERNEL);
	if (ret) {
		dev_err(&vdev->dev,
			"Failed to execute command on admin vq: %d\n.", ret);
		goto err_cmd_exec;
	}

	status = le16_to_cpu(va_status->status);
	if (status != VIRTIO_ADMIN_STATUS_OK) {
		dev_err(&vdev->dev,
			"admin command error: status(%#x) qualifier(%#x)\n",
			status, le16_to_cpu(va_status->status_qualifier));
		ret = -status;
	}

err_cmd_exec:
	kfree(va_hdr);
err_alloc:
	kfree(va_status);
	return ret;
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
