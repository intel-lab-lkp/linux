// SPDX-License-Identifier: GPL-2.0-only
/*
 * Sign hibernation images with a kernel-generated TPM key.
 *
 * When an image is created, the kernel creates a fresh TPM signing key,
 * along with evidence signed by the kernel attestation key that the key
 * was created by the kernel rather than by userspace (see
 * drivers/char/tpm/tpm2-kernel-key.c).  The evidence is stored in the
 * image header, and the key signs the digest of the whole image.  The key
 * cannot be recreated and does not survive a TPM reset, so it can sign
 * nothing else once the system has been powered off.
 *
 * On resume, the evidence is checked as soon as the image header has been
 * read, which yields the public key that the kernel created, and the
 * image is only restored if it was signed with that key.
 */

#define pr_fmt(fmt) "PM: hibernation: " fmt

#include <linux/efi.h>
#include <linux/init.h>
#include <linux/security.h>
#include <linux/tpm.h>

#include "power.h"

#define HIB_TPM_SIG_MAGIC	"LNXHSIG1"

/* The signature page */
struct hib_tpm_sig {
	u8 magic[8];
	u8 x[EC_PT_SZ];
	u8 y[EC_PT_SZ];
	u8 r[EC_PT_SZ];
	u8 s[EC_PT_SZ];
};

static_assert(sizeof(struct hib_tpm_sig) <= HIB_SIG_TRAILER_SIZE);
static_assert(sizeof(struct tpm2_key_provenance) <= HIB_SIG_HEADER_SIZE);

/* The key used to sign the image being created */
static struct tpm2_kernel_key hib_tpm_key;
static bool hib_tpm_key_valid;

/* The public key shown to have been created by the kernel, on restore */
static u8 hib_tpm_x[EC_PT_SZ], hib_tpm_y[EC_PT_SZ];
static bool hib_tpm_header_ok;

static struct tpm_chip *hib_tpm_get_chip(void)
{
	struct tpm_chip *chip = tpm_default_chip();

	if (!chip)
		return NULL;

	if (!(chip->flags & TPM_CHIP_FLAG_TPM2) || tpm_try_get_ops(chip)) {
		put_device(&chip->dev);
		return NULL;
	}

	return chip;
}

static void hib_tpm_put_chip(struct tpm_chip *chip)
{
	tpm_put_ops(chip);
	put_device(&chip->dev);
}

/* TPM errors are positive; report them as a failure to use the TPM */
static int hib_tpm_err(int rc)
{
	return rc > 0 ? -EIO : rc;
}

static void hib_tpm_end(void)
{
	struct tpm_chip *chip;

	if (!hib_tpm_key_valid)
		return;

	chip = hib_tpm_get_chip();
	if (chip) {
		tpm2_kernel_key_destroy(chip, &hib_tpm_key);
		hib_tpm_put_chip(chip);
	}

	memzero_explicit(&hib_tpm_key, sizeof(hib_tpm_key));
	hib_tpm_key_valid = false;
}

static int hib_tpm_begin(void *data, size_t size)
{
	struct tpm2_key_provenance *prov = data;
	struct tpm_chip *chip;
	int rc;

	/* the image may be read more than once */
	hib_tpm_end();

	chip = hib_tpm_get_chip();
	if (!chip)
		return -ENODEV;

	rc = tpm2_kernel_key_create(chip, &hib_tpm_key, prov);
	hib_tpm_put_chip(chip);
	if (rc) {
		pr_err("Failed to create image signing key: %d\n", rc);
		return hib_tpm_err(rc);
	}

	hib_tpm_key_valid = true;
	return 0;
}

static int hib_tpm_sign(const u8 *digest, void *trailer, size_t size)
{
	struct hib_tpm_sig *sig = trailer;
	struct tpm_chip *chip;
	int rc;

	if (!hib_tpm_key_valid)
		return -EINVAL;

	chip = hib_tpm_get_chip();
	if (!chip)
		return -ENODEV;

	memcpy(sig->magic, HIB_TPM_SIG_MAGIC, sizeof(sig->magic));
	memcpy(sig->x, hib_tpm_key.x, sizeof(sig->x));
	memcpy(sig->y, hib_tpm_key.y, sizeof(sig->y));
	rc = tpm2_kernel_key_sign(chip, &hib_tpm_key, digest, sig->r, sig->s);

	/* the key has done its job */
	tpm2_kernel_key_destroy(chip, &hib_tpm_key);
	hib_tpm_key_valid = false;
	hib_tpm_put_chip(chip);

	if (rc) {
		pr_err("Failed to sign image: %d\n", rc);
		return hib_tpm_err(rc);
	}

	return 0;
}

static int hib_tpm_check_header(const void *data, size_t size)
{
	const struct tpm2_key_provenance *prov = data;
	struct tpm_chip *chip;
	int rc;

	hib_tpm_header_ok = false;

	/* PCR 5 can only distinguish the kernel if firmware extends it */
	if (!kernel_tpm_security_available) {
		pr_err("Cannot verify image: TPM security not available\n");
		return -EKEYREJECTED;
	}

	chip = hib_tpm_get_chip();
	if (!chip) {
		pr_err("Cannot verify image: no TPM\n");
		return -ENODEV;
	}

	/*
	 * A virtual TPM, such as one provided by userspace through
	 * /dev/vtpmx, can answer verification with anything it likes.  Only
	 * allow one if explicitly configured to, and never under lockdown.
	 */
	if ((chip->flags & TPM_CHIP_FLAG_VIRTUAL) &&
	    (!IS_ENABLED(CONFIG_TPM_HIBERNATE_INSECURE) ||
	     security_locked_down(LOCKDOWN_HIBERNATION))) {
		hib_tpm_put_chip(chip);
		pr_err("Cannot verify image: TPM is virtual\n");
		return -EKEYREJECTED;
	}

	rc = tpm2_kernel_key_verify(chip, prov, hib_tpm_x, hib_tpm_y);
	hib_tpm_put_chip(chip);
	if (rc) {
		pr_err("Image signing key was not created by the kernel: %d\n",
		       rc);
		return rc > 0 ? -EKEYREJECTED : rc;
	}

	hib_tpm_header_ok = true;
	return 0;
}

static int hib_tpm_verify(const u8 *digest, const void *trailer, size_t size)
{
	const struct hib_tpm_sig *sig = trailer;

	if (!hib_tpm_header_ok)
		return -EKEYREJECTED;

	if (memcmp(sig->magic, HIB_TPM_SIG_MAGIC, sizeof(sig->magic)))
		return -EKEYREJECTED;

	/* the image must be signed with the key the kernel created */
	if (memcmp(sig->x, hib_tpm_x, sizeof(hib_tpm_x)) ||
	    memcmp(sig->y, hib_tpm_y, sizeof(hib_tpm_y)))
		return -EKEYREJECTED;

	return tpm2_kernel_key_verify_signature(hib_tpm_x, hib_tpm_y, digest,
						sig->r, sig->s);
}

const struct hib_sig_ops hib_tpm_sig_ops = {
	.begin		= hib_tpm_begin,
	.sign		= hib_tpm_sign,
	.end		= hib_tpm_end,
	.check_header	= hib_tpm_check_header,
	.verify		= hib_tpm_verify,
};

/**
 * hibernate_tpm_available - Check whether images can be signed.
 *
 * Signing requires a TPM 2.0 and firmware that extends PCR 5 when
 * ExitBootServices() is called.
 */
bool hibernate_tpm_available(void)
{
	struct tpm_chip *chip;
	bool tpm2;

	if (!kernel_tpm_security_available)
		return false;

	chip = tpm_default_chip();
	if (!chip)
		return false;

	tpm2 = chip->flags & TPM_CHIP_FLAG_TPM2;
	put_device(&chip->dev);
	return tpm2;
}
