/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Header file for virtio admin operations
 */
#include <uapi/linux/virtio_pci.h>
#include <uapi/linux/virtio_net_ff.h>

#ifndef _LINUX_VIRTIO_ADMIN_H
#define _LINUX_VIRTIO_ADMIN_H

struct virtio_device;

/**
 * VIRTIO_CAP_IN_LIST - Check if a capability is supported in the capability list
 * @cap_list: Pointer to capability list structure containing supported_caps array
 * @cap: Capability ID to check
 *
 * The cap_list contains a supported_caps array of little-endian 64-bit integers
 * where each bit represents a capability. Bit 0 of the first element represents
 * capability ID 0, bit 1 represents capability ID 1, and so on.
 *
 * Return: 1 if capability is supported, 0 otherwise
 */
#define VIRTIO_CAP_IN_LIST(cap_list, cap) \
	(!!(1 & (le64_to_cpu(cap_list->supported_caps[cap / 64]) >> cap % 64)))

/**
 * struct virtio_admin_ops - Operations for virtio admin functionality
 *
 * This structure contains function pointers for performing administrative
 * operations on virtio devices. All data and caps pointers must be allocated
 * on the heap by the caller.
 */
struct virtio_admin_ops {
	/**
	 * @cap_id_list_query: Query the list of supported capability IDs
	 * @vdev: The virtio device to query
	 * @data: Pointer to result structure (must be heap allocated)
	 * Return: 0 on success, negative error code on failure
	 */
	int (*cap_id_list_query)(struct virtio_device *vdev,
				 struct virtio_admin_cmd_query_cap_id_result *data);
	/**
	 * @cap_get: Get capability data for a specific capability ID
	 * @vdev: The virtio device
	 * @id: Capability ID to retrieve
	 * @caps: Pointer to capability data structure (must be heap allocated)
	 * @cap_size: Size of the capability data structure
	 * Return: 0 on success, negative error code on failure
	 */
	int (*cap_get)(struct virtio_device *vdev,
		       u16 id,
		       void *caps,
		       size_t cap_size);
	/**
	 * @cap_set: Set capability data for a specific capability ID
	 * @vdev: The virtio device
	 * @id: Capability ID to set
	 * @caps: Pointer to capability data structure (must be heap allocated)
	 * @cap_size: Size of the capability data structure
	 * Return: 0 on success, negative error code on failure
	 */
	int (*cap_set)(struct virtio_device *vdev,
		       u16 id,
		       const void *caps,
		       size_t cap_size);
	/**
	 * @object_create: Create a new object of specified type
	 * @virtio_dev: The virtio device
	 * @obj_type: Type of object to create
	 * @obj_id: ID to assign to the created object
	 * @group_type: Type of group the object belongs to
	 * @group_member_id: Member ID within the group
	 * @obj_specific_data: Object-specific data (must be heap allocated)
	 * @obj_specific_data_size: Size of the object-specific data
	 * Returns: 0 on success, negative error code on failure
	 */
	int (*object_create)(struct virtio_device *virtio_dev,
			     u16 obj_type,
			     u32 obj_id,
			     u16 group_type,
			     u64 group_member_id,
			     const void *obj_specific_data,
			     size_t obj_specific_data_size);
	/**
	 * @object_destroy: Destroy an existing object
	 * @virtio_dev: The virtio device
	 * @obj_type: Type of object to destroy
	 * @obj_id: ID of the object to destroy
	 * @group_type: Type of group the object belongs to
	 * @group_member_id: Member ID within the group
	 * Returns: 0 on success, negative error code on failure
	 */
	int (*object_destroy)(struct virtio_device *virtio_dev,
			      u16 obj_type,
			      u32 obj_id,
			      u16 group_type,
			      u64 group_member_id);
};

#endif /* _LINUX_VIRTIO_ADMIN_H */
