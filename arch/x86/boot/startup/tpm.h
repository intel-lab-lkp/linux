/* SPDX-License-Identifier: GPL-2.0 */
/*
 * TPM early extend header file.
 *
 * Copyright (c) 2026 Apertus Solutions, LLC
 * Copyright (c) 2026, Oracle and/or its affiliates.
 */

#ifndef BOOT_COMPRESSED_TPM_H
#define BOOT_COMPRESSED_TPM_H

enum early_tis_defaults {
	TPM_TIMEOUT		= 5, /* ms */
	TIS_DURATION		= 120000, /* 120 secs in ms */
};

enum tpm_family {
	TPM_FAMILY_INVALID	= 0,
	TPM_FAMILY_12		= 1,
	TPM_FAMILY_20		= 2
};

struct tpm_chip {
	enum tpm_family family;
	u64 baseaddr;
	int locality;
	int did;
	int vid;

	/* in ms */
	ktime_t timeout_a;
	ktime_t timeout_b;
	ktime_t timeout_c;
	ktime_t timeout_d;
};

bool tpm_tis_check_locality(struct tpm_chip *chip, int loc);
void tpm_tis_release_locality(struct tpm_chip *chip);
int tpm_tis_request_locality(struct tpm_chip *chip, int loc);
void tpm_tis_disable_interrupts(struct tpm_chip *chip);
int tpm1_pcr_extend(struct tpm_chip *chip, u32 pcr_idx, const u8 *hash);
int tpm2_pcr_extend(struct tpm_chip *chip, u32 pcr_idx,
		    struct tpm_digest *digests, u32 digest_count);
int early_tpm_init(struct tpm_chip *chip, u64 baseaddr);
int early_tpm_fini(struct tpm_chip *chip);

#endif /* BOOT_COMPRESSED_TPM_H */
