// SPDX-License-Identifier: GPL-2.0

#include <linux/efi.h>
#include <linux/errno.h>
#include <linux/unaligned.h>

#include "efistub.h"

struct efi_hd_dev_path {
	struct efi_generic_dev_path header;
	u32 partition_number;
	u64 partition_start;
	u64 partition_size;
	efi_guid_t signature;
	u8 partition_format;
	u8 signature_type;
} __packed;

#define EFI_HD_PARTITION_FORMAT_GPT	2
#define EFI_HD_SIGNATURE_TYPE_GUID	2

static efi_char16_t efi_bli_nibble_to_hex(u8 nibble)
{
	static const char hex[16] __nonstring = "0123456789abcdef";

	return hex[nibble];
}

static void efi_bli_guid_to_str(const efi_guid_t *guid, efi_char16_t *out)
{
	static const u8 guid_index[UUID_SIZE] = {
		3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15,
	};

	for (int i = 0; i < ARRAY_SIZE(guid_index); i++) {
		u8 byte = guid->b[guid_index[i]];

		*out++ = efi_bli_nibble_to_hex(byte >> 4);
		*out++ = efi_bli_nibble_to_hex(byte & 0xf);

		switch (i) {
		case 3:
		case 5:
		case 7:
		case 9:
			*out++ = L'-';
		}
	}

	*out = L'\0';
}

static int efi_bli_dev_path_part_uuid(const efi_device_path_protocol_t *path,
				      efi_char16_t *partuuid)
{
	const efi_device_path_protocol_t *node;
	const struct efi_hd_dev_path *hd_node;
	u16 node_len;

	for (node = path;
	     node->type != EFI_DEV_END_PATH && node->type != EFI_DEV_END_PATH2;
	     node = (const void *)node + node_len) {
		node_len = get_unaligned_le16(&node->length);

		if (node_len < sizeof(*node))
			return -EINVAL;

		if (node->type != EFI_DEV_MEDIA ||
		    node->sub_type != EFI_DEV_MEDIA_HARD_DRIVE)
			continue;

		if (node_len < sizeof(*hd_node))
			return -EINVAL;

		hd_node = (const struct efi_hd_dev_path *)node;
		if (hd_node->partition_format != EFI_HD_PARTITION_FORMAT_GPT ||
		    hd_node->signature_type != EFI_HD_SIGNATURE_TYPE_GUID)
			continue;

		efi_bli_guid_to_str(&hd_node->signature, partuuid);
		return 0;
	}

	return -ENOENT;
}

static void efi_bli_populate_loader_part_uuid(efi_loaded_image_t *image)
{
	static efi_guid_t device_path_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
	static efi_guid_t loader_entry_guid = LINUX_EFI_LOADER_ENTRY_GUID;
	efi_char16_t partuuid[UUID_STRING_LEN + 1];
	unsigned long size = 0;
	void *path;

	if (get_efi_var(L"LoaderDevicePartUUID", &loader_entry_guid,
			NULL, &size, NULL) != EFI_NOT_FOUND)
		return;

	if (efi_bs_call(handle_protocol, efi_table_attr(image, device_handle),
			&device_path_guid, &path) != EFI_SUCCESS)
		return;

	if (efi_bli_dev_path_part_uuid(path, partuuid))
		return;

	set_efi_var(L"LoaderDevicePartUUID", &loader_entry_guid,
		    EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
		    sizeof(partuuid), partuuid);
}

void efi_bli_set_variables(efi_loaded_image_t *image)
{
	efi_bli_populate_loader_part_uuid(image);
}
