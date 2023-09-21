// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/*
 * Copyright (c) 2023, NVIDIA CORPORATION & AFFILIATES. All rights reserved
 */

#include "cmd.h"

int virtiovf_cmd_list_query(struct pci_dev *pdev, u8 *buf, int buf_size)
{
	struct virtio_device *virtio_dev = virtio_pci_vf_get_pf_dev(pdev);
	struct scatterlist out_sg;
	struct virtio_admin_cmd cmd = {};

	if (!virtio_dev)
		return -ENOTCONN;

	sg_init_one(&out_sg, buf, buf_size);
	cmd.opcode = VIRTIO_ADMIN_CMD_LIST_QUERY;
	cmd.group_type = VIRTIO_ADMIN_GROUP_TYPE_SRIOV;
	cmd.result_sg = &out_sg;

	return virtio_admin_cmd_exec(virtio_dev, &cmd);
}

int virtiovf_cmd_list_use(struct pci_dev *pdev, u8 *buf, int buf_size)
{
	struct virtio_device *virtio_dev = virtio_pci_vf_get_pf_dev(pdev);
	struct scatterlist in_sg;
	struct virtio_admin_cmd cmd = {};

	if (!virtio_dev)
		return -ENOTCONN;

	sg_init_one(&in_sg, buf, buf_size);
	cmd.opcode = VIRTIO_ADMIN_CMD_LIST_USE;
	cmd.group_type = VIRTIO_ADMIN_GROUP_TYPE_SRIOV;
	cmd.data_sg = &in_sg;

	return virtio_admin_cmd_exec(virtio_dev, &cmd);
}

int virtiovf_cmd_lr_write(struct virtiovf_pci_core_device *virtvdev, u16 opcode,
			  u8 offset, u8 size, u8 *buf)
{
	struct virtio_device *virtio_dev =
		virtio_pci_vf_get_pf_dev(virtvdev->core_device.pdev);
	struct virtio_admin_cmd_legacy_wr_data *in;
	struct scatterlist in_sg;
	struct virtio_admin_cmd cmd = {};
	int ret;

	if (!virtio_dev)
		return -ENOTCONN;

	in = kzalloc(sizeof(*in) + size, GFP_KERNEL);
	if (!in)
		return -ENOMEM;

	in->offset = offset;
	memcpy(in->registers, buf, size);
	sg_init_one(&in_sg, in, sizeof(*in) + size);
	cmd.opcode = opcode;
	cmd.group_type = VIRTIO_ADMIN_GROUP_TYPE_SRIOV;
	cmd.group_member_id = virtvdev->vf_id + 1;
	cmd.data_sg = &in_sg;
	ret = virtio_admin_cmd_exec(virtio_dev, &cmd);

	kfree(in);
	return ret;
}

int virtiovf_cmd_lr_read(struct virtiovf_pci_core_device *virtvdev, u16 opcode,
			 u8 offset, u8 size, u8 *buf)
{
	struct virtio_device *virtio_dev =
		virtio_pci_vf_get_pf_dev(virtvdev->core_device.pdev);
	struct virtio_admin_cmd_legacy_rd_data *in;
	struct scatterlist in_sg, out_sg;
	struct virtio_admin_cmd cmd = {};
	int ret;

	if (!virtio_dev)
		return -ENOTCONN;

	in = kzalloc(sizeof(*in), GFP_KERNEL);
	if (!in)
		return -ENOMEM;

	in->offset = offset;
	sg_init_one(&in_sg, in, sizeof(*in));
	sg_init_one(&out_sg, buf, size);
	cmd.opcode = opcode;
	cmd.group_type = VIRTIO_ADMIN_GROUP_TYPE_SRIOV;
	cmd.data_sg = &in_sg;
	cmd.result_sg = &out_sg;
	cmd.group_member_id = virtvdev->vf_id + 1;
	ret = virtio_admin_cmd_exec(virtio_dev, &cmd);

	kfree(in);
	return ret;
}

int virtiovf_cmd_lq_read_notify(struct virtiovf_pci_core_device *virtvdev,
				u8 req_bar_flags, u8 *bar, u64 *bar_offset)
{
	struct virtio_device *virtio_dev =
		virtio_pci_vf_get_pf_dev(virtvdev->core_device.pdev);
	struct virtio_admin_cmd_notify_info_result *out;
	struct scatterlist out_sg;
	struct virtio_admin_cmd cmd = {};
	int ret;

	if (!virtio_dev)
		return -ENOTCONN;

	out = kzalloc(sizeof(*out), GFP_KERNEL);
	if (!out)
		return -ENOMEM;

	sg_init_one(&out_sg, out, sizeof(*out));
	cmd.opcode = VIRTIO_ADMIN_CMD_LEGACY_NOTIFY_INFO;
	cmd.group_type = VIRTIO_ADMIN_GROUP_TYPE_SRIOV;
	cmd.result_sg = &out_sg;
	cmd.group_member_id = virtvdev->vf_id + 1;
	ret = virtio_admin_cmd_exec(virtio_dev, &cmd);
	if (!ret) {
		struct virtio_admin_cmd_notify_info_data *entry;
		int i;

		ret = -ENOENT;
		for (i = 0; i < VIRTIO_ADMIN_CMD_MAX_NOTIFY_INFO; i++) {
			entry = &out->entries[i];
			if (entry->flags == VIRTIO_ADMIN_CMD_NOTIFY_INFO_FLAGS_END)
				break;
			if (entry->flags != req_bar_flags)
				continue;
			*bar = entry->bar;
			*bar_offset = le64_to_cpu(entry->offset);
			ret = 0;
			break;
		}
	}

	kfree(out);
	return ret;
}
