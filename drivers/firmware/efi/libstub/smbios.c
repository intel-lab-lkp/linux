// SPDX-License-Identifier: GPL-2.0-only
// Copyright 2022 Google LLC
// Author: Ard Biesheuvel <ardb@google.com>

#include <linux/efi.h>

#include "efistub.h"

typedef union efi_smbios_protocol efi_smbios_protocol_t;

union efi_smbios_protocol {
	struct {
		efi_status_t (__efiapi *add)(efi_smbios_protocol_t *, efi_handle_t,
					     u16 *, struct efi_smbios_record *);
		efi_status_t (__efiapi *update_string)(efi_smbios_protocol_t *, u16 *,
						       unsigned long *, u8 *);
		efi_status_t (__efiapi *remove)(efi_smbios_protocol_t *, u16);
		efi_status_t (__efiapi *get_next)(efi_smbios_protocol_t *, u16 *, u8 *,
						  struct efi_smbios_record **,
						  efi_handle_t *);

		u8 major_version;
		u8 minor_version;
	};
	struct {
		u32 add;
		u32 update_string;
		u32 remove;
		u32 get_next;

		u8 major_version;
		u8 minor_version;
	} mixed_mode;
};

static bool verify_ep_checksum(const struct smbios_entry_point *ep)
{
	const u8 *ptr = (u8 *)ep;
	u8 sum = 0;
	int i;

	for (i = 0; i < ep->ep_length; i++)
		sum += ptr[i];

	return sum == 0;
}

static bool verify_ep_int_checksum(const struct smbios_entry_point *ep)
{
	const u8 *ptr = (u8 *)&ep->int_anchor;
	u8 sum = 0;
	int i;

	for (i = 0; i < 15; i++)
		sum += ptr[i];

	return sum == 0;
}

static bool verify_ep_integrity(const struct smbios_entry_point *ep)
{
	if (memcmp(ep->anchor, "_SM_", sizeof(ep->anchor)) != 0)
		return false;

	if (memcmp(ep->int_anchor, "_DMI_", sizeof(ep->int_anchor)) != 0)
		return false;

	if (!verify_ep_checksum(ep) || !verify_ep_int_checksum(ep))
		return false;

	return true;
}

static const struct efi_smbios_record *search_record(void *table, u32 length,
						     u8 type)
{
	const u8 *p, *end;

	p = (u8 *)table;
	end = p + length;

	while (p + sizeof(struct efi_smbios_record) < end) {
		const struct efi_smbios_record *hdr =
			(struct efi_smbios_record *)p;
		const u8 *next;

		if (hdr->type == type)
			return hdr;

		/* Type 127 = End-of-Table */
		if (hdr->type == 0x7F)
			return NULL;

		/* Jumping to the unformed section */
		next = p + hdr->length;

		/* Unformed section ends with 0000h */
		while ((next[0] != 0 || next[1] != 0) && next + 1 < end)
			next++;

		next += 2;
		p = next;
	}

	return NULL;
}

static const struct efi_smbios_record *get_table_record(u8 type)
{
	const struct smbios_entry_point *ep;

	ep = get_efi_config_table(SMBIOS_TABLE_GUID);
	if (!ep)
		return NULL;

	if (!verify_ep_integrity(ep))
		return NULL;

	return search_record((void *)(unsigned long)ep->st_address,
		ep->st_length, type);
}

const struct efi_smbios_record *efi_get_smbios_record(u8 type)
{
	struct efi_smbios_record *record;
	efi_smbios_protocol_t *smbios;
	efi_status_t status;
	u16 handle = 0xfffe;

	status = efi_bs_call(locate_protocol, &EFI_SMBIOS_PROTOCOL_GUID, NULL,
			     (void **)&smbios) ?:
		 efi_call_proto(smbios, get_next, &handle, &type, &record, NULL);
	if (status == EFI_SUCCESS)
		return record;

	efi_info(
		"Cannot access SMBIOS protocol (status 0x%lx), parsing table directly\n",
		status
	);

	return get_table_record(type);
}

const u8 *__efi_get_smbios_string(const struct efi_smbios_record *record,
				  const u8 *offset)
{
	const u8 *strtable;

	if (!record)
		return NULL;

	strtable = (u8 *)record + record->length;
	for (int i = 1; i < *offset; i++) {
		int len = strlen(strtable);

		if (!len)
			return NULL;
		strtable += len + 1;
	}
	return strtable;
}
