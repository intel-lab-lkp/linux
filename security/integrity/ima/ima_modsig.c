// SPDX-License-Identifier: GPL-2.0+
/*
 * IMA support for appraising module-style appended signatures.
 *
 * Copyright (C) 2019  IBM Corporation
 *
 * Author:
 * Thiago Jung Bauermann <bauerman@linux.ibm.com>
 */

#include <linux/types.h>
#include <linux/module_signature.h>
#include <keys/asymmetric-type.h>
#include <crypto/pkcs7.h>

#include "ima.h"

struct modsig {
	struct pkcs7_message *pkcs7_msg;

	/* The file content was supplied datached. */
	bool detached;

	/*
	 * This is what will go to the measurement list if the template requires
	 * storing the signature.
	 */
	int raw_pkcs7_len;
	u8 raw_pkcs7[] __counted_by(raw_pkcs7_len);
};

/*
 * ima_read_modsig - Read modsig from buf.
 *
 * Return: 0 on success, error code otherwise.
 */
int ima_read_modsig(enum ima_hooks func, const void *buf, loff_t buf_len,
		    struct modsig **modsig)
{
	const size_t marker_len = strlen(MODULE_SIGNATURE_MARKER);
	const struct module_signature *sig;
	struct modsig *hdr;
	size_t sig_len;
	const void *p;
	int rc;

	if (buf_len <= marker_len + sizeof(*sig))
		return -ENOENT;

	p = buf + buf_len - marker_len;
	if (memcmp(p, MODULE_SIGNATURE_MARKER, marker_len))
		return -ENOENT;

	buf_len -= marker_len;
	sig = (const struct module_signature *)(p - sizeof(*sig));

	rc = mod_check_sig(sig, buf_len, func_tokens[func]);
	if (rc)
		return rc;

	sig_len = be32_to_cpu(sig->sig_len);
	buf_len -= sig_len + sizeof(*sig);

	/* Allocate sig_len additional bytes to hold the raw PKCS#7 data. */
	hdr = kzalloc_flex(*hdr, raw_pkcs7, sig_len);
	if (!hdr)
		return -ENOMEM;

	hdr->raw_pkcs7_len = sig_len;
	hdr->pkcs7_msg = pkcs7_parse_message(buf + buf_len, sig_len);
	if (IS_ERR(hdr->pkcs7_msg)) {
		rc = PTR_ERR(hdr->pkcs7_msg);
		kfree(hdr);
		return rc;
	}

	memcpy(hdr->raw_pkcs7, buf + buf_len, sig_len);

	*modsig = hdr;

	return 0;
}

/**
 * ima_collect_modsig - Supply the file content without the appended signature.
 * @modsig: parsed module signature
 * @buf: data to verify the signature on
 * @size: data size
 *
 * Since the modsig is part of the file contents, the hash used in its signature
 * isn't the same one ordinarily calculated by IMA. Therefore PKCS7 code
 * calculates a separate one for each signer when it's first needed.
 */
void ima_collect_modsig(struct modsig *modsig, const void *buf, loff_t size)
{
	/*
	 * Provide the file contents (minus the appended sig) so that the PKCS7
	 * code can calculate the file hash.
	 */
	size -= modsig->raw_pkcs7_len + strlen(MODULE_SIGNATURE_MARKER) +
		sizeof(struct module_signature);
	modsig->detached = !pkcs7_supply_detached_data(modsig->pkcs7_msg,
						       buf, size);
}

int ima_modsig_verify(struct key *keyring, const struct modsig *modsig)
{
	return verify_pkcs7_message_sig(NULL, 0, modsig->pkcs7_msg, keyring,
					VERIFYING_MODULE_SIGNATURE, NULL, NULL);
}

unsigned int ima_get_modsig_signer_count(const struct modsig *modsig)
{
	return pkcs7_get_signer_count(modsig->pkcs7_msg);
}

/*
 * ima_get_modsig_digest - Get the content digest of one signer of the modsig.
 *
 * See pkcs7_get_digest() for the results.
 */
int ima_get_modsig_digest(const struct modsig *modsig, unsigned int index,
			  enum hash_algo *algo, const u8 **digest,
			  u32 *digest_size, const char **sig_algo)
{
	if (!modsig->detached)
		return -EBADMSG;

	return pkcs7_get_digest(modsig->pkcs7_msg, index, digest, digest_size,
				algo, sig_algo);
}

int ima_get_raw_modsig(const struct modsig *modsig, const void **data,
		       u32 *data_len)
{
	*data = &modsig->raw_pkcs7;
	*data_len = modsig->raw_pkcs7_len;

	return 0;
}

void ima_free_modsig(struct modsig *modsig)
{
	if (!modsig)
		return;

	pkcs7_free_message(modsig->pkcs7_msg);
	kfree(modsig);
}
