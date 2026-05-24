// SPDX-License-Identifier: GPL-2.0-only

#include <crypto/tpm2_key.h>
#include <linux/oid_registry.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/unaligned.h>
#include "tpm2_key.asn1.h"

#undef pr_fmt
#define pr_fmt(fmt) "tpm2_key: "fmt

struct tpm2_key_decoder_context {
	u32 parent;
	const u8 *pub;
	u32 pub_len;
	const u8 *priv;
	u32 priv_len;
	enum OID oid;
	bool empty_auth;
};

int tpm2_key_get_parent(void *context, size_t hdrlen,
			unsigned char tag,
			const void *value, size_t vlen)
{
	struct tpm2_key_decoder_context *decoder = context;
	const u8 *v = value;
	int i;

	decoder->parent = 0;
	for (i = 0; i < vlen; i++) {
		decoder->parent <<= 8;
		decoder->parent |= v[i];
	}

	return 0;
}

int tpm2_key_get_type(void *context, size_t hdrlen,
		      unsigned char tag,
		      const void *value, size_t vlen)
{
	struct tpm2_key_decoder_context *decoder = context;

	decoder->oid = look_up_OID(value, vlen);
	return 0;
}

int tpm2_key_get_empty_auth(void *context, size_t hdrlen,
			    unsigned char tag,
			    const void *value, size_t vlen)
{
	struct tpm2_key_decoder_context *decoder = context;
	const u8 *bool_value = value;

	if (!value || vlen != 1)
		return -EBADMSG;

	decoder->empty_auth = bool_value[0] != 0;
	return 0;
}

static inline bool tpm2_key_is_valid(const void *value, size_t vlen)
{
	if (vlen < 2 || vlen > TPM2_KEY_BYTES_MAX)
		return false;

	if (get_unaligned_be16(value) != vlen - 2)
		return false;

	return true;
}

int tpm2_get_public(void *context, size_t hdrlen, unsigned char tag,
		    const void *value, size_t vlen)
{
	struct tpm2_key_decoder_context *decoder = context;

	if (!tpm2_key_is_valid(value, vlen))
		return -EBADMSG;

	if (sizeof(struct tpm2_key_desc) > vlen - 2)
		return -EBADMSG;

	decoder->pub = value;
	decoder->pub_len = vlen;
	return 0;
}

int tpm2_get_private(void *context, size_t hdrlen, unsigned char tag,
		     const void *value, size_t vlen)
{
	struct tpm2_key_decoder_context *decoder = context;

	if (!tpm2_key_is_valid(value, vlen))
		return -EBADMSG;

	decoder->priv = value;
	decoder->priv_len = vlen;
	return 0;
}

/**
 * tpm2_key_decode() - Decode TPM2 ASN.1 key
 * @src:	ASN.1 source.
 * @src_len:	ASN.1 source length.
 *
 * Decodes the TPM2 ASN.1 key and validates that the public key data has all
 * the shared fields of TPMT_PUBLIC. This is full coverage of the memory that
 * can be validated before doing any key type specific validation.
 *
 * Return:
 * - TPM2 ASN.1 key on success.
 * - -EBADMSG when decoding fails.
 * - -ENOMEM when OOM while allocating struct tpm2_key.
 */
struct tpm2_key *tpm2_key_decode(const u8 *src, u32 src_len)
{
	struct tpm2_key_decoder_context decoder;
	struct tpm2_key *key;
	u8 *data;
	int ret;

	memset(&decoder, 0, sizeof(decoder));
	ret = asn1_ber_decoder(&tpm2_key_decoder, &decoder, src, src_len);
	if (ret < 0) {
		if (ret != -EBADMSG)
			pr_info("Decoder error %d\n", ret);

		return ERR_PTR(-EBADMSG);
	}

	key = kzalloc(sizeof(*key), GFP_KERNEL);
	if (!key)
		return ERR_PTR(-ENOMEM);

	data = &key->data[0];
	memcpy(&data[0], decoder.priv, decoder.priv_len);
	memcpy(&data[decoder.priv_len], decoder.pub, decoder.pub_len);

	key->oid = decoder.oid;
	key->priv_len = decoder.priv_len;
	key->pub_len = decoder.pub_len;
	key->parent = decoder.parent;
	key->desc = (struct tpm2_key_desc *)&data[decoder.priv_len + 2];
	key->empty_auth = decoder.empty_auth;
	return key;
}
EXPORT_SYMBOL_GPL(tpm2_key_decode);
