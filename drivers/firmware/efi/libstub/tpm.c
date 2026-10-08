// SPDX-License-Identifier: GPL-2.0
/*
 * TPM handling.
 *
 * Copyright (C) 2016 CoreOS, Inc
 * Copyright (C) 2017 Google, Inc.
 *     Matthew Garrett <mjg59@google.com>
 *     Thiebaud Weksteen <tweek@google.com>
 */
#include <linux/efi.h>
#include <linux/tpm_eventlog.h>
#include <crypto/sha1.h>
#include <crypto/sha2.h>
#include <asm/efi.h>

#include "efistub.h"

#ifdef CONFIG_RESET_ATTACK_MITIGATION
static const efi_char16_t efi_MemoryOverWriteRequest_name[] =
	L"MemoryOverwriteRequestControl";

#define MEMORY_ONLY_RESET_CONTROL_GUID \
	EFI_GUID(0xe20939be, 0x32d4, 0x41be, 0xa1, 0x50, 0x89, 0x7f, 0x85, 0xd4, 0x98, 0x29)

/*
 * Enable reboot attack mitigation. This requests that the firmware clear the
 * RAM on next reboot before proceeding with boot, ensuring that any secrets
 * are cleared. If userland has ensured that all secrets have been removed
 * from RAM before reboot it can simply reset this variable.
 */
void efi_enable_reset_attack_mitigation(void)
{
	u8 val = 1;
	efi_guid_t var_guid = MEMORY_ONLY_RESET_CONTROL_GUID;
	efi_status_t status;
	unsigned long datasize = 0;

	status = get_efi_var(efi_MemoryOverWriteRequest_name, &var_guid,
			     NULL, &datasize, NULL);

	if (status == EFI_NOT_FOUND)
		return;

	set_efi_var(efi_MemoryOverWriteRequest_name, &var_guid,
		    EFI_VARIABLE_NON_VOLATILE |
		    EFI_VARIABLE_BOOTSERVICE_ACCESS |
		    EFI_VARIABLE_RUNTIME_ACCESS, sizeof(val), &val);
}

#endif

static void efi_retrieve_tcg2_eventlog(int version, efi_physical_addr_t log_location,
				       efi_physical_addr_t log_last_entry,
				       efi_bool_t truncated,
				       struct efi_tcg2_final_events_table *final_events_table)
{
	efi_guid_t linux_eventlog_guid = LINUX_EFI_TPM_EVENT_LOG_GUID;
	efi_status_t status;
	struct linux_efi_tpm_eventlog *log_tbl = NULL;
	unsigned long first_entry_addr, last_entry_addr;
	size_t log_size, last_entry_size;
	u32 final_events_size = 0;

	first_entry_addr = (unsigned long) log_location;

	/*
	 * We populate the EFI table even if the logs are empty.
	 */
	if (!log_last_entry) {
		log_size = 0;
	} else {
		last_entry_addr = (unsigned long) log_last_entry;
		/*
		 * get_event_log only returns the address of the last entry.
		 * We need to calculate its size to deduce the full size of
		 * the logs.
		 *
		 * CC Event log also uses TCG2 format, handle it same as TPM2.
		 */
		if (version > EFI_TCG2_EVENT_LOG_FORMAT_TCG_1_2) {
			/*
			 * The TCG2 log format has variable length entries,
			 * and the information to decode the hash algorithms
			 * back into a size is contained in the first entry -
			 * pass a pointer to the final entry (to calculate its
			 * size) and the first entry (so we know how long each
			 * digest is)
			 */
			last_entry_size =
				__calc_tpm2_event_size((void *)last_entry_addr,
						    (void *)(long)log_location,
						    false);
		} else {
			last_entry_size = sizeof(struct tcpa_event) +
			   ((struct tcpa_event *) last_entry_addr)->event_size;
		}
		log_size = log_last_entry - log_location + last_entry_size;
	}

	/* Allocate space for the logs and copy them. */
	status = efi_bs_call(allocate_pool, EFI_ACPI_RECLAIM_MEMORY,
			     sizeof(*log_tbl) + log_size, (void **)&log_tbl);

	if (status != EFI_SUCCESS) {
		efi_err("Unable to allocate memory for event log\n");
		return;
	}

	/*
	 * Figure out whether any events have already been logged to the
	 * final events structure, and if so how much space they take up
	 */
	if (final_events_table && final_events_table->nr_events) {
		struct tcg_pcr_event2_head *header;
		u32 offset;
		void *data;
		u32 event_size;
		int i = final_events_table->nr_events;

		data = (void *)final_events_table;
		offset = sizeof(final_events_table->version) +
			sizeof(final_events_table->nr_events);

		while (i > 0) {
			header = data + offset + final_events_size;
			event_size = __calc_tpm2_event_size(header,
						   (void *)(long)log_location,
						   false);
			/* If calc fails this is a malformed log */
			if (!event_size)
				break;
			final_events_size += event_size;
			i--;
		}
	}

	memset(log_tbl, 0, sizeof(*log_tbl) + log_size);
	log_tbl->size = log_size;
	log_tbl->final_events_preboot_size = final_events_size;
	log_tbl->version = version;
	memcpy(log_tbl->log, (void *) first_entry_addr, log_size);

	status = efi_bs_call(install_configuration_table,
			     &linux_eventlog_guid, log_tbl);
	if (status != EFI_SUCCESS)
		goto err_free;
	return;

err_free:
	efi_bs_call(free_pool, log_tbl);
}

void efi_retrieve_eventlog(void)
{
	struct efi_tcg2_final_events_table *final_events_table = NULL;
	efi_physical_addr_t log_location = 0, log_last_entry = 0;
	efi_guid_t tpm2_guid = EFI_TCG2_PROTOCOL_GUID;
	int version = EFI_TCG2_EVENT_LOG_FORMAT_TCG_2;
	efi_tcg2_protocol_t *tpm2 = NULL;
	efi_bool_t truncated;
	efi_status_t status;

	status = efi_bs_call(locate_protocol, &tpm2_guid, NULL, (void **)&tpm2);
	if (status == EFI_SUCCESS) {
		status = efi_call_proto(tpm2, get_event_log, version, &log_location,
					&log_last_entry, &truncated);

		if (status != EFI_SUCCESS || !log_location) {
			version = EFI_TCG2_EVENT_LOG_FORMAT_TCG_1_2;
			status = efi_call_proto(tpm2, get_event_log, version,
						&log_location, &log_last_entry,
						&truncated);
		} else {
			final_events_table =
				get_efi_config_table(EFI_TCG2_FINAL_EVENTS_TABLE_GUID);
		}
	} else {
		efi_guid_t cc_guid = EFI_CC_MEASUREMENT_PROTOCOL_GUID;
		efi_cc_protocol_t *cc = NULL;

		status = efi_bs_call(locate_protocol, &cc_guid, NULL, (void **)&cc);
		if (status != EFI_SUCCESS)
			return;

		version = EFI_CC_EVENT_LOG_FORMAT_TCG_2;
		status = efi_call_proto(cc, get_event_log, version, &log_location,
					&log_last_entry, &truncated);

		final_events_table =
			get_efi_config_table(EFI_CC_FINAL_EVENTS_TABLE_GUID);
	}

	if (status != EFI_SUCCESS || !log_location)
		return;

	efi_retrieve_tcg2_eventlog(version, log_location, log_last_entry,
				   truncated, final_events_table);
}

#ifdef CONFIG_KERNEL_TPM_SECURITY
#define PCR5_INDEX	5

static const char pcr5_event[] = "Linux kernel TPM NVIndex support";
static efi_guid_t pcr5_guid = LINUX_EFI_PCR5_LOG_GUID;

static const struct {
	u16	hash_alg;
	u16	digest_size;
} pcr5_banks[] = {
	{ TPM_ALG_SHA1,		SHA1_DIGEST_SIZE },
	{ TPM_ALG_SHA256,	SHA256_DIGEST_SIZE },
};

struct tpm2_pcr_read_cmd {
	__be16	tag;
	__be32	size;
	__be32	cc;
	__be32	count;
	__be16	hash;
	u8	size_of_select;
	u8	select[3];
} __packed;

/* digest is sized for the largest bank; smaller digests are shorter */
struct tpm2_pcr_read_rsp {
	__be16	tag;
	__be32	size;
	__be32	rc;
	__be32	update_counter;
	__be32	count;
	__be16	hash;
	u8	size_of_select;
	u8	select[3];
	__be32	digest_count;
	__be16	digest_size;
	u8	digest[TPM2_MAX_DIGEST_SIZE];
} __packed;

static efi_status_t efi_tpm_extend_pcr5(efi_tcg2_protocol_t *tcg2)
{
	struct efi_tcg2_event *evt __free(efi_pool) = NULL;
	u32 size = sizeof(*evt) + sizeof(pcr5_event) - 1;
	efi_status_t status;

	status = efi_bs_call(allocate_pool, EFI_LOADER_DATA, size,
			     (void **)&evt);
	if (status != EFI_SUCCESS)
		return status;

	*evt = (struct efi_tcg2_event){
		.event_size			= size,
		.event_header.header_size	= sizeof(evt->event_header),
		.event_header.header_version	= EFI_TCG2_EVENT_HEADER_VERSION,
		.event_header.pcr_index		= PCR5_INDEX,
		.event_header.event_type	= EV_EFI_ACTION,
	};
	memcpy(evt + 1, pcr5_event, sizeof(pcr5_event) - 1);

	return efi_call_proto(tcg2, hash_log_extend_event, 0,
			      (unsigned long)pcr5_event,
			      sizeof(pcr5_event) - 1, evt);
}

static efi_status_t efi_tpm_read_pcr5(efi_tcg2_protocol_t *tcg2,
				      u16 hash_alg, u16 digest_size,
				      struct tpm2_pcr_read_rsp *rsp)
{
	struct tpm2_pcr_read_cmd cmd = {
		.tag		= cpu_to_be16(TPM2_ST_NO_SESSIONS),
		.size		= cpu_to_be32(sizeof(cmd)),
		.cc		= cpu_to_be32(TPM2_CC_PCR_READ),
		.count		= cpu_to_be32(1),
		.hash		= cpu_to_be16(hash_alg),
		.size_of_select	= sizeof(cmd.select),
		.select		= { BIT(PCR5_INDEX) },
	};
	u32 rsp_size = sizeof(*rsp) - sizeof(rsp->digest) + digest_size;
	efi_status_t status;

	status = efi_call_proto(tcg2, submit_command, sizeof(cmd), (u8 *)&cmd,
				sizeof(*rsp), (u8 *)rsp);
	if (status != EFI_SUCCESS)
		return status;

	/*
	 * A TPM without the requested bank active returns an empty
	 * selection and no digests, so check that we got back exactly
	 * what we asked for.
	 */
	if (be32_to_cpu(rsp->size) != rsp_size || rsp->rc ||
	    be32_to_cpu(rsp->count) != 1 ||
	    be16_to_cpu(rsp->hash) != hash_alg ||
	    rsp->size_of_select != sizeof(rsp->select) ||
	    rsp->select[0] != BIT(PCR5_INDEX) ||
	    be32_to_cpu(rsp->digest_count) != 1 ||
	    be16_to_cpu(rsp->digest_size) != digest_size)
		return EFI_NOT_FOUND;

	return EFI_SUCCESS;
}

/*
 * Extend a fixed value into PCR 5 and publish the resulting value of each
 * supported PCR bank in a configuration table, so that the kernel can
 * later verify that the firmware extended PCR 5 when ExitBootServices()
 * was called.
 */
void efi_tpm_record_pcr5(void)
{
	efi_guid_t tcg2_guid = EFI_TCG2_PROTOCOL_GUID;
	struct linux_efi_pcr5_log *log;
	struct tpm2_pcr_read_rsp rsp;
	efi_tcg2_protocol_t *tcg2 = NULL;
	efi_status_t status;
	int i;

	status = efi_bs_call(locate_protocol, &tcg2_guid, NULL, (void **)&tcg2);
	if (status != EFI_SUCCESS || !tcg2)
		return;

	status = efi_tpm_extend_pcr5(tcg2);
	if (status != EFI_SUCCESS) {
		efi_warn("Failed to extend PCR 5: 0x%lx\n", status);
		return;
	}

	status = efi_bs_call(allocate_pool, EFI_ACPI_RECLAIM_MEMORY,
			     struct_size(log, digests, ARRAY_SIZE(pcr5_banks)),
			     (void **)&log);
	if (status != EFI_SUCCESS) {
		efi_err("Unable to allocate memory for PCR 5 log\n");
		return;
	}

	memset(log, 0, struct_size(log, digests, ARRAY_SIZE(pcr5_banks)));
	for (i = 0; i < ARRAY_SIZE(pcr5_banks); i++) {
		struct linux_efi_pcr5_digest *d = &log->digests[log->count];

		/* inactive banks are simply not recorded */
		status = efi_tpm_read_pcr5(tcg2, pcr5_banks[i].hash_alg,
					   pcr5_banks[i].digest_size, &rsp);
		if (status != EFI_SUCCESS)
			continue;

		d->hash_alg = pcr5_banks[i].hash_alg;
		d->digest_size = pcr5_banks[i].digest_size;
		memcpy(d->digest, rsp.digest, d->digest_size);
		log->count++;
	}

	if (!log->count) {
		efi_warn("Failed to read PCR 5\n");
		goto err_free;
	}

	status = efi_bs_call(install_configuration_table, &pcr5_guid, log);
	if (status != EFI_SUCCESS) {
		efi_err("Unable to install PCR 5 log table\n");
		goto err_free;
	}
	return;

err_free:
	efi_bs_call(free_pool, log);
}
#endif
