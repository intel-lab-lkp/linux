// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/kernel.h>
#include <linux/key.h>
#include <keys/asymmetric-type.h>
#include <keys/system_keyring.h>
#include <linux/slab.h>
#include <crypto/sha2.h>
#include "x509_parser.h"

int x509_load_certificate_list(const u8 cert_list[],
			       const unsigned long list_size,
			       const struct key *keyring)
{
	key_ref_t key;
	const u8 *p, *end;
	size_t plen;

	p = cert_list;
	end = p + list_size;
	while (p < end) {
		/* Each cert begins with an ASN.1 SEQUENCE tag and must be more
		 * than 256 bytes in size.
		 */
		if (end - p < 4)
			goto dodgy_cert;
		if (p[0] != 0x30 ||
		    p[1] != 0x82)
			goto dodgy_cert;
		plen = (p[2] << 8) | p[3];
		plen += 4;
		if (plen > end - p)
			goto dodgy_cert;

		key = key_create_or_update(make_key_ref(keyring, 1),
					   "asymmetric",
					   NULL,
					   p,
					   plen,
					   ((KEY_POS_ALL & ~KEY_POS_SETATTR) |
					   KEY_USR_VIEW | KEY_USR_READ),
					   KEY_ALLOC_NOT_IN_QUOTA |
					   KEY_ALLOC_BUILT_IN |
					   KEY_ALLOC_BYPASS_RESTRICTION);
		if (IS_ERR(key)) {
			pr_err("Problem loading in-kernel X.509 certificate (%ld)\n",
			       PTR_ERR(key));
		} else {
			pr_notice("Loaded X.509 cert '%s'\n",
				  key_ref_to_ptr(key)->description);
			key_ref_put(key);
		}
		p += plen;
	}

	return 0;

dodgy_cert:
	pr_err("Problem parsing in-kernel X.509 certificate list\n");
	return 0;
}
EXPORT_SYMBOL_GPL(x509_load_certificate_list);

#ifdef CONFIG_SYSTEM_BLACKLIST_KEYRING
static const void *crl_entry_issuer(const struct x509_crl_context *crl,
				    const struct x509_revoked_entry *entry,
				    size_t *size)
{
	if (crl->indirect_crl && entry->raw_issuer) {
		*size = entry->raw_issuer_size;
		return entry->raw_issuer;
	}
	*size = crl->raw_issuer_size;
	return crl->raw_issuer;
}

int x509_load_crl_list(const u8 crl_list[], const unsigned long list_size)
{
	const u8 *p = crl_list, *end = p + list_size;

	while (p < end) {
		size_t plen;
		struct x509_crl_context *crl_ctx;
		struct x509_revoked_entry *entry, *tmp;

		/* Need at least: tag(1) + length(1) = 2 bytes */
		if (end - p < 2 || p[0] != 0x30)
			goto dodgy_crl;

		if (p[1] == 0x82) {
			if (end - p < 4)
				goto dodgy_crl;
			plen = ((p[2] << 8) | p[3]) + 4;
		} else if (p[1] == 0x81) {
			if (end - p < 3)
				goto dodgy_crl;
			plen = p[2] + 3;
		} else {
			goto dodgy_crl;
		}

		if (plen > end - p)
			goto dodgy_crl;

		crl_ctx = x509_crl_parse(p, plen);
		if (IS_ERR(crl_ctx)) {
			pr_err("Problem parsing CRL (%ld)\n", PTR_ERR(crl_ctx));
			goto next_crl;
		}
		list_for_each_entry_safe(entry, tmp, &crl_ctx->revoked_list, list) {
			struct asymmetric_key_id *kid;
			const void *issuer;
			size_t issuer_size;

			issuer = crl_entry_issuer(crl_ctx, entry, &issuer_size);
			kid = asymmetric_key_generate_id(entry->serial,
							 entry->serial_size,
							 issuer, issuer_size);
			if (!IS_ERR(kid)) {
				u8 digest[SHA256_DIGEST_SIZE];

				sha256(kid->data, kid->len, digest);
				mark_hash_blacklisted(digest, SHA256_DIGEST_SIZE,
						      BLACKLIST_HASH_X509_CRL);
				kfree(kid);
			}
			list_del(&entry->list);
			kfree(entry->serial);
			kfree(entry);
		}
		x509_crl_free(crl_ctx);
next_crl:
		p += plen;
	}
	return 0;

dodgy_crl:
	pr_err("Problem parsing CRL list\n");
	return -EINVAL;
}
EXPORT_SYMBOL_GPL(x509_load_crl_list);
#endif
