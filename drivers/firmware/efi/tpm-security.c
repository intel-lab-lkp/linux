// SPDX-License-Identifier: GPL-2.0
/*
 * Verify that the firmware measured the ExitBootServices() invocation
 * into PCR 5, by comparing the current value of PCR 5 against the value
 * recorded by the EFI stub immediately before it called
 * ExitBootServices().
 */

#define pr_fmt(fmt) "efi: " fmt

#include <linux/efi.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/tpm.h>

#define PCR5_INDEX	5

unsigned long __initdata efi_pcr5_log = EFI_INVALID_TABLE_ADDR;
bool kernel_tpm_security_available __ro_after_init;

static int __init efi_tpm_check_pcr5(void)
{
	struct linux_efi_pcr5_log *log;
	unsigned int recorded = 0, changed = 0;
	struct tpm_chip *chip;
	size_t size;
	u32 count;
	int i, rc;

	if (efi_pcr5_log == EFI_INVALID_TABLE_ADDR)
		return 0;

	log = memremap(efi_pcr5_log, sizeof(*log), MEMREMAP_WB);
	if (!log) {
		pr_err("Failed to map PCR 5 log\n");
		return 0;
	}
	count = log->count;
	memunmap(log);

	size = struct_size(log, digests, count);
	log = memremap(efi_pcr5_log, size, MEMREMAP_WB);
	if (!log) {
		pr_err("Failed to map PCR 5 log\n");
		return 0;
	}

	chip = tpm_default_chip();
	if (!chip) {
		pr_warn("No TPM available to verify PCR 5\n");
		goto out;
	}

	if (!tpm_is_tpm2(chip)) {
		pr_warn("PCR 5 log requires a TPM 2.0\n");
		goto out_put;
	}

	for (i = 0; i < count; i++) {
		struct linux_efi_pcr5_digest *d = &log->digests[i];
		struct tpm_digest digest = { .alg_id = d->hash_alg };

		if (d->digest_size > sizeof(d->digest)) {
			pr_err("Invalid PCR 5 log entry for bank 0x%04x\n",
			       d->hash_alg);
			continue;
		}

		rc = tpm_pcr_read(chip, PCR5_INDEX, &digest);
		if (rc) {
			pr_err("Failed to read PCR 5 bank 0x%04x: %d\n",
			       d->hash_alg, rc);
			continue;
		}

		recorded++;
		if (memcmp(digest.digest, d->digest, d->digest_size))
			changed++;
	}

	if (!recorded)
		goto out_put;

	if (!changed)
		pr_err(FW_BUG "Firmware failed to extend PCR 5 for ExitBootServices\n");
	else if (changed != recorded)
		pr_err(FW_BUG "Firmware only extended one PCR bank in ExitBootServices\n");

	if (changed)
		kernel_tpm_security_available = true;

out_put:
	put_device(&chip->dev);
out:
	memunmap(log);
	return 0;
}
late_initcall(efi_tpm_check_pcr5);
