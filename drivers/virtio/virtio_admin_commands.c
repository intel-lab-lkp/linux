// SPDX-License-Identifier: GPL-2.0-only

#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/virtio_admin.h>
#include <uapi/linux/virtio_pci.h>

int virtio_admin_cap_id_list_query(struct virtio_device *vdev,
				   struct virtio_admin_cmd_query_cap_id_result *data)
{
	struct virtio_admin_cmd cmd = {};
	struct scatterlist result_sg;

	if (!vdev->config->admin_cmd_exec)
		return -EOPNOTSUPP;

	sg_init_one(&result_sg, data, sizeof(*data));
	cmd.opcode = cpu_to_le16(VIRTIO_ADMIN_CMD_CAP_ID_LIST_QUERY);
	cmd.group_type = cpu_to_le16(VIRTIO_ADMIN_GROUP_TYPE_SELF);
	cmd.result_sg = &result_sg;

	return vdev->config->admin_cmd_exec(vdev, &cmd);
}
EXPORT_SYMBOL_GPL(virtio_admin_cap_id_list_query);

int virtio_admin_cap_get(struct virtio_device *vdev,
			 u16 id,
			 void *caps,
			 size_t cap_size)
{
	struct virtio_admin_cmd_cap_get_data *data;
	struct virtio_admin_cmd cmd = {};
	struct scatterlist result_sg;
	struct scatterlist data_sg;
	int err;

	if (!vdev->config->admin_cmd_exec)
		return -EOPNOTSUPP;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->id = cpu_to_le16(id);
	sg_init_one(&data_sg, data, sizeof(*data));
	sg_init_one(&result_sg, caps, cap_size);
	cmd.opcode = cpu_to_le16(VIRTIO_ADMIN_CMD_DEVICE_CAP_GET);
	cmd.group_type = cpu_to_le16(VIRTIO_ADMIN_GROUP_TYPE_SELF);
	cmd.data_sg = &data_sg;
	cmd.result_sg = &result_sg;

	err = vdev->config->admin_cmd_exec(vdev, &cmd);
	kfree(data);

	return err;
}
EXPORT_SYMBOL_GPL(virtio_admin_cap_get);

int virtio_admin_cap_set(struct virtio_device *vdev,
			 u16 id,
			 const void *caps,
			 size_t cap_size)
{
	struct virtio_admin_cmd_cap_set_data *data;
	struct virtio_admin_cmd cmd = {};
	struct scatterlist data_sg;
	size_t data_size;
	int err;

	if (!vdev->config->admin_cmd_exec)
		return -EOPNOTSUPP;

	data_size = sizeof(*data) + cap_size;
	data = kzalloc(data_size, GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->id = cpu_to_le16(id);
	memcpy(data->cap_specific_data, caps, cap_size);
	sg_init_one(&data_sg, data, data_size);
	cmd.opcode = cpu_to_le16(VIRTIO_ADMIN_CMD_DRIVER_CAP_SET);
	cmd.group_type = cpu_to_le16(VIRTIO_ADMIN_GROUP_TYPE_SELF);
	cmd.data_sg = &data_sg;
	cmd.result_sg = NULL;

	err = vdev->config->admin_cmd_exec(vdev, &cmd);
	kfree(data);

	return err;
}
EXPORT_SYMBOL_GPL(virtio_admin_cap_set);

int virtio_admin_obj_create(struct virtio_device *vdev,
			    u16 obj_type,
			    u32 obj_id,
			    u16 group_type,
			    u64 group_member_id,
			    const void *obj_specific_data,
			    size_t obj_specific_data_size)
{
	size_t data_size = sizeof(struct virtio_admin_cmd_resource_obj_create_data);
	struct virtio_admin_cmd_resource_obj_create_data *obj_create_data;
	struct virtio_admin_cmd cmd = {};
	struct scatterlist data_sg;
	void *data;
	int err;

	if (!vdev->config->admin_cmd_exec)
		return -EOPNOTSUPP;

	data_size += obj_specific_data_size;
	data = kzalloc(data_size, GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	obj_create_data = data;
	obj_create_data->hdr.type = cpu_to_le16(obj_type);
	obj_create_data->hdr.id = cpu_to_le32(obj_id);
	memcpy(obj_create_data->resource_obj_specific_data, obj_specific_data,
	       obj_specific_data_size);
	sg_init_one(&data_sg, data, data_size);

	cmd.opcode = cpu_to_le16(VIRTIO_ADMIN_CMD_RESOURCE_OBJ_CREATE);
	cmd.group_type = cpu_to_le16(group_type);
	cmd.group_member_id = cpu_to_le64(group_member_id);
	cmd.data_sg = &data_sg;

	err = vdev->config->admin_cmd_exec(vdev, &cmd);
	kfree(data);

	return err;
}
EXPORT_SYMBOL_GPL(virtio_admin_obj_create);

int virtio_admin_obj_destroy(struct virtio_device *vdev,
			     u16 obj_type,
			     u32 obj_id,
			     u16 group_type,
			     u64 group_member_id)
{
	struct virtio_admin_cmd_resource_obj_cmd_hdr *data;
	struct virtio_admin_cmd cmd = {};
	struct scatterlist data_sg;
	int err;

	if (!vdev->config->admin_cmd_exec)
		return -EOPNOTSUPP;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->type = cpu_to_le16(obj_type);
	data->id = cpu_to_le32(obj_id);
	sg_init_one(&data_sg, data, sizeof(*data));
	cmd.opcode = cpu_to_le16(VIRTIO_ADMIN_CMD_RESOURCE_OBJ_DESTROY);
	cmd.group_type = cpu_to_le16(group_type);
	cmd.group_member_id = cpu_to_le64(group_member_id);
	cmd.data_sg = &data_sg;

	err = vdev->config->admin_cmd_exec(vdev, &cmd);
	kfree(data);

	WARN_ON_ONCE(err);

	return err;
}
EXPORT_SYMBOL_GPL(virtio_admin_obj_destroy);
