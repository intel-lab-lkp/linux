/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Header file for virtio admin operations
 */

#ifndef _LINUX_VIRTIO_ADMIN_H
#define _LINUX_VIRTIO_ADMIN_H

struct virtio_device;
struct virtio_admin_cmd_query_cap_id_result;

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
	(!!(1 & (le64_to_cpu(cap_list->supported_caps[(cap) / 64]) >> (cap) % 64)))

/**
 * virtio_admin_cap_id_list_query - Query the list of available capability IDs
 * @vdev: The virtio device to query
 * @data: Pointer to result structure (must be heap allocated)
 *
 * This function queries the virtio device for the list of available capability
 * IDs that can be used with virtio_admin_cap_get() and virtio_admin_cap_set().
 * The result is stored in the provided data structure.
 *
 * Return: 0 on success, -EOPNOTSUPP if the device doesn't support admin
 * operations or capability queries, or a negative error code on other failures.
 */
int virtio_admin_cap_id_list_query(struct virtio_device *vdev,
				   struct virtio_admin_cmd_query_cap_id_result *data);

/**
 * virtio_admin_cap_get - Get capability data for a specific capability ID
 * @vdev: The virtio device
 * @id: Capability ID to retrieve
 * @caps: Pointer to capability data structure (must be heap allocated)
 * @cap_size: Size of the capability data structure
 *
 * This function retrieves a specific capability from the virtio device.
 * The capability data is stored in the provided buffer. The caller must
 * ensure the buffer is large enough to hold the capability data.
 *
 * Return: 0 on success, -EOPNOTSUPP if the device doesn't support admin
 * operations or capability retrieval, or a negative error code on other failures.
 */
int virtio_admin_cap_get(struct virtio_device *vdev,
			 u16 id,
			 void *caps,
			 size_t cap_size);

/**
 * virtio_admin_cap_set - Set capability data for a specific capability ID
 * @vdev: The virtio device
 * @id: Capability ID to set
 * @caps: Pointer to capability data structure (must be heap allocated)
 * @cap_size: Size of the capability data structure
 *
 * This function sets a specific capability on the virtio device.
 * The capability data is read from the provided buffer and applied
 * to the device. The device may validate the capability data before
 * applying it.
 *
 * Return: 0 on success, -EOPNOTSUPP if the device doesn't support admin
 * operations or capability setting, or a negative error code on other failures.
 */
int virtio_admin_cap_set(struct virtio_device *vdev,
			 u16 id,
			 const void *caps,
			 size_t cap_size);

#endif /* _LINUX_VIRTIO_ADMIN_H */
