/* SPDX-License-Identifier: GPL-2.0 */
/*
 * RSA padding templates.
 */

#ifndef _CRYPTO_RSA_PKCS1PAD_H
#define _CRYPTO_RSA_PKCS1PAD_H

/*
 * Hash algorithm name to ASN.1 template mapping.
 */
struct rsa_asn1_template {
	const char *name;
	const u8 *data;
	size_t size;
};

const struct rsa_asn1_template *rsa_lookup_asn1(const char *name);

#endif /* _CRYPTO_RSA_PKCS1PAD_H */
