// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * An asymmetric TPM2 key subtype.
 */

#include <crypto/hash_info.h>
#include <crypto/internal/ecc.h>
#include <crypto/public_key.h>
#include <crypto/tpm2_key.h>
#include <keys/asymmetric-parser.h>
#include <keys/asymmetric-subtype.h>
#include <linux/asn1_encoder.h>
#include <linux/keyctl.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/tpm.h>
#include <linux/unaligned.h>

#undef pr_fmt
#define pr_fmt(fmt) "tpm2_asymmetric: "fmt

/* TPM2 Structures 12.2.3.5: TPMS_RSA_PARMS */
struct tpm2_asymmetric_rsa_parms {
	__be16 symmetric;
	__be16 scheme;
	__be16 key_bits;
	__be32 exponent;
	__be16 modulus_size;
} __packed;

/* TPM2 Structures 12.2.3.6: TPMS_ECC_PARMS */
struct tpm2_asymmetric_ecc_parms {
	__be16 symmetric;
	__be16 scheme;
	__be16 ecc;
	__be16 kdf;
};

static const void *tpm2_asymmetric_parms(const struct tpm2_key *key)
{
	return &key->data[key->priv_len + 2 + sizeof(*key->desc)];
}

static u16 tpm2_asymmetric_rsa_mod_size(const struct tpm2_key *key)
{
	const struct tpm2_asymmetric_rsa_parms *p = tpm2_asymmetric_parms(key);

	return be16_to_cpu(p->modulus_size);
}

static const u8 *tpm2_asymmetric_ecc_x(const struct tpm2_key *key)
{
	return tpm2_asymmetric_parms(key) + sizeof(struct tpm2_asymmetric_ecc_parms);
}

static const u8 *tpm2_asymmetric_ecc_y(const struct tpm2_key *key)
{
	const u8 *x = tpm2_asymmetric_ecc_x(key);
	u16 x_size = get_unaligned_be16(&x[0]);

	return &x[2 + x_size];
}

static unsigned int tpm2_asymmetric_ecc_key_bits(u16 ecc)
{
	switch (ecc) {
	case TPM2_ECC_NIST_P256:
		return 256;
	case TPM2_ECC_NIST_P384:
		return 384;
	case TPM2_ECC_NIST_P521:
		return 521;
	default:
		return 0;
	}
}

static int tpm2_asymmetric_hash_lookup(const char *hash_algo,
				       int *hash_id, int *tpm_hash)
{
	int id, alg;

	if (!hash_algo)
		return -EINVAL;

	id = match_string(hash_algo_name, HASH_ALGO__LAST, hash_algo);
	if (id < 0)
		return -ENOPKG;

	alg = tpm2_find_hash_alg(id);
	if (alg < 0)
		return -ENOPKG;

	if (hash_id)
		*hash_id = id;

	if (tpm_hash)
		*tpm_hash = alg;

	return 0;
}

static int tpm2_asymmetric_signature_scheme(const struct tpm2_key *key,
					    const char *encoding,
					    const char *hash_algo,
					    u16 *scheme,
					    int *tpm_hash)
{
	if (!encoding)
		return -ENOPKG;

	switch (tpm2_key_type(key)) {
	case TPM_ALG_RSA:
		if (strcmp(encoding, "pkcs1") != 0)
			return -ENOPKG;
		*scheme = TPM_ALG_RSASSA;
		break;
	case TPM_ALG_ECC:
		if (strcmp(encoding, "x962") != 0)
			return -ENOPKG;
		*scheme = TPM_ALG_ECDSA;
		break;
	default:
		return -EOPNOTSUPP;
	}

	return tpm2_asymmetric_hash_lookup(hash_algo, NULL, tpm_hash);
}

/*
 * Load a TPM2 key blob into the TPM.
 *
 * On success, @buf is initialized and the authorization session is kept open.
 * On failure, @buf is destroyed and the authorization session is closed.
 */
static int tpm2_asymmetric_load(struct tpm_chip *chip, struct tpm2_key *key,
				struct tpm_buf *buf, u32 *handle_out)
{
	int ret;

	ret = tpm2_start_auth_session(chip);
	if (ret)
		return ret;

	ret = tpm_buf_init(buf, TPM2_ST_SESSIONS, TPM2_CC_LOAD);
	if (ret < 0)
		goto err_auth;

	ret = tpm_buf_append_name(chip, buf, key->parent, NULL);
	if (ret)
		goto err_buf;
	tpm_buf_append_hmac_session(chip, buf, TPM2_SA_CONTINUE_SESSION |
				    TPM2_SA_ENCRYPT, NULL, 0);
	tpm_buf_append(buf, &key->data[0], key->priv_len + key->pub_len);
	if (buf->flags & TPM_BUF_OVERFLOW) {
		ret = -E2BIG;
		goto err_buf;
	}
	ret = tpm_buf_fill_hmac_session(chip, buf);
	if (ret)
		goto err_buf;
	ret = tpm_transmit_cmd(chip, buf, 4, "TPM2_CC_LOAD");
	ret = tpm_buf_check_hmac_response(chip, buf, ret);
	if (ret) {
		ret = -EIO;
		goto err_buf;
	}

	*handle_out = be32_to_cpup((__be32 *)&buf->data[TPM_HEADER_SIZE]);
	return 0;

err_buf:
	tpm_buf_destroy(buf);

err_auth:
	tpm2_end_auth_session(chip);
	return ret;
}

static void tpm2_asymmetric_key_destroy(void *payload0, void *payload3)
{
	kfree(payload0);
}

/*
 * Encrypt using TPM2_RSA_Encrypt with RSAES (PKCS#1 v1.5) scheme.
 */
static int tpm2_asymmetric_rsa_encrypt(struct tpm_chip *chip,
				       struct tpm2_key *key,
				       struct kernel_pkey_params *params,
				       const void *in, void *out)
{
	u32 key_handle = 0;
	struct tpm_buf buf;
	u16 ciphertext_len;
	u16 scheme;
	u8 *pos;
	int ret;

	if (!params->encoding)
		return -EINVAL;

	if (strcmp(params->encoding, "pkcs1") == 0)
		scheme = TPM_ALG_RSAES;
	else if (strcmp(params->encoding, "raw") == 0)
		scheme = TPM_ALG_NULL;
	else
		return -ENOPKG;

	ret = tpm_try_get_ops(chip);
	if (ret)
		return ret;

	ret = tpm2_asymmetric_load(chip, key, &buf, &key_handle);
	if (ret)
		goto err_ops;

	tpm2_end_auth_session(chip);
	tpm_buf_destroy(&buf);

	ret = tpm_buf_init(&buf, TPM2_ST_NO_SESSIONS, TPM2_CC_RSA_ENCRYPT);
	if (ret)
		goto err_key;

	tpm_buf_append_u32(&buf, key_handle);

	tpm_buf_append_u16(&buf, params->in_len);
	tpm_buf_append(&buf, in, params->in_len);

	tpm_buf_append_u16(&buf, scheme);

	tpm_buf_append_u16(&buf, 0);

	ret = tpm_transmit_cmd(chip, &buf, 4, "TPM2_RSA_Encrypt");
	if (ret) {
		ret = -EIO;
		goto err_buf;
	}

	pos = buf.data + TPM_HEADER_SIZE;
	ciphertext_len = be16_to_cpup((__be16 *)pos);
	pos += 2;
	if (pos + ciphertext_len > buf.data + buf.length) {
		ret = -EIO;
		goto err_buf;
	}

	if (params->out_len < ciphertext_len) {
		ret = -EMSGSIZE;
		goto err_buf;
	}

	memcpy(out, pos, ciphertext_len);
	ret = ciphertext_len;

err_buf:
	tpm_buf_destroy(&buf);

err_key:
	tpm2_flush_context(chip, key_handle);

err_ops:
	tpm_put_ops(chip);
	return ret;
}

/*
 * Convert a TPM2B_PUBLIC_KEY_RSA response into a raw RSA signature.
 */
static int tpm2_asymmetric_rsa_parse_signature(struct tpm_buf *buf,
					       off_t *offset,
					       struct kernel_pkey_params *params,
					       void *out)
{
	u16 sig_len;

	sig_len = tpm_buf_read_u16(buf, offset);
	if (buf->flags & TPM_BUF_BOUNDARY_ERROR)
		return -EIO;
	if (*offset + sig_len > buf->length)
		return -EIO;
	if (sig_len > params->out_len)
		return -EMSGSIZE;

	memcpy(out, &buf->data[*offset], sig_len);
	return sig_len;
}

/*
 * Convert a TPMT_SIGNATURE ECDSA R/S response into DER SEQUENCE form.
 */
static int tpm2_asymmetric_ecc_parse_signature(struct tpm_buf *buf, off_t *offset,
					       struct kernel_pkey_params *params,
					       void *out)
{
	u8 der[2 * (2 + ECC_MAX_BYTES + 1)];
	u8 *encoded, *ptr;
	const u8 *s;
	u16 r_size;
	u16 s_size;

	r_size = tpm_buf_read_u16(buf, offset);
	if (buf->flags & TPM_BUF_BOUNDARY_ERROR)
		return -EIO;
	if (r_size == 0 || r_size > ECC_MAX_BYTES ||
	    *offset + r_size + 2 > buf->length)
		return -EIO;

	s_size = get_unaligned_be16(&buf->data[*offset + r_size]);
	s = &buf->data[*offset + r_size + 2];
	if (s_size == 0 || s_size > ECC_MAX_BYTES ||
	    *offset + r_size + 2 + s_size > buf->length)
		return -EIO;

	ptr = der;
	ptr = asn1_encode_integer_bytes(ptr, der + sizeof(der),
					&buf->data[*offset], r_size);
	ptr = asn1_encode_integer_bytes(ptr, der + sizeof(der), s, s_size);
	if (IS_ERR(ptr))
		return PTR_ERR(ptr);

	encoded = asn1_encode_sequence(out, (u8 *)out + params->out_len,
				       der, ptr - der);
	if (IS_ERR(encoded))
		return PTR_ERR(encoded) == -EINVAL ? -EMSGSIZE : PTR_ERR(encoded);

	return encoded - (u8 *)out;
}

static int tpm2_asymmetric_parse_signature(struct tpm_buf *buf,
					   u16 scheme, int tpm_hash,
					   struct kernel_pkey_params *params,
					   void *out)
{
	off_t offset = TPM_HEADER_SIZE + 4;
	u16 hash_alg;
	u16 sig_alg;

	sig_alg = tpm_buf_read_u16(buf, &offset);
	hash_alg = tpm_buf_read_u16(buf, &offset);
	if (buf->flags & TPM_BUF_BOUNDARY_ERROR)
		return -EIO;
	if (sig_alg != scheme || hash_alg != tpm_hash)
		return -EIO;

	switch (scheme) {
	case TPM_ALG_RSASSA:
		return tpm2_asymmetric_rsa_parse_signature(buf, &offset, params, out);
	case TPM_ALG_ECDSA:
		return tpm2_asymmetric_ecc_parse_signature(buf, &offset, params, out);
	default:
		return -EOPNOTSUPP;
	}
}

/*
 * Sign a digest using TPM2_Sign.
 */
static int tpm2_asymmetric_sign(struct tpm_chip *chip, struct tpm2_key *key,
				struct kernel_pkey_params *params,
				const void *in, void *out)
{
	struct tpm_buf buf;
	u32 key_handle = 0;
	int tpm_hash;
	u16 scheme;
	int ret;

	ret = tpm2_asymmetric_signature_scheme(key, params->encoding,
					       params->hash_algo, &scheme,
					       &tpm_hash);
	if (ret)
		return ret;

	ret = tpm_try_get_ops(chip);
	if (ret)
		return ret;

	ret = tpm2_asymmetric_load(chip, key, &buf, &key_handle);
	if (ret)
		goto err_ops;

	tpm_buf_reset(&buf, TPM2_ST_SESSIONS, TPM2_CC_SIGN);
	ret = tpm_buf_append_name(chip, &buf, key_handle, NULL);
	if (ret)
		goto err_key;
	tpm_buf_append_hmac_session(chip, &buf, TPM2_SA_DECRYPT, NULL, 0);

	/* digest (TPM2B_DIGEST) */
	tpm_buf_append_u16(&buf, params->in_len);
	tpm_buf_append(&buf, in, params->in_len);

	/* inScheme (TPMT_SIG_SCHEME) */
	tpm_buf_append_u16(&buf, scheme);
	tpm_buf_append_u16(&buf, tpm_hash);

	/* validation (TPMT_TK_HASHCHECK): NULL ticket */
	tpm_buf_append_u16(&buf, TPM2_ST_HASHCHECK);
	tpm_buf_append_u32(&buf, TPM2_RH_NULL);
	tpm_buf_append_u16(&buf, 0);

	if (buf.flags & TPM_BUF_OVERFLOW) {
		tpm2_end_auth_session(chip);
		ret = -E2BIG;
		goto err_key;
	}
	ret = tpm_buf_fill_hmac_session(chip, &buf);
	if (ret)
		goto err_key;
	ret = tpm_transmit_cmd(chip, &buf, 4, "TPM2_Sign");
	ret = tpm_buf_check_hmac_response(chip, &buf, ret);
	if (ret) {
		ret = -EIO;
		goto err_key;
	}

	ret = tpm2_asymmetric_parse_signature(&buf, scheme, tpm_hash, params, out);

err_key:
	tpm2_flush_context(chip, key_handle);
	tpm_buf_destroy(&buf);

err_ops:
	tpm_put_ops(chip);
	return ret;
}

/*
 * Decrypt using TPM2_RSA_Decrypt with RSAES-PKCS1-v1_5 scheme.
 */
static int tpm2_asymmetric_rsa_decrypt(struct tpm_chip *chip,
				       struct tpm2_key *key,
				       struct kernel_pkey_params *params,
				       const void *in, void *out)
{
	u32 key_handle = 0;
	struct tpm_buf buf;
	u16 decrypted_len;
	off_t offset;
	int ret;

	if (!params->encoding || strcmp(params->encoding, "pkcs1") != 0)
		return -ENOPKG;

	ret = tpm_try_get_ops(chip);
	if (ret)
		return ret;

	ret = tpm2_asymmetric_load(chip, key, &buf, &key_handle);
	if (ret)
		goto err_ops;

	tpm_buf_reset(&buf, TPM2_ST_SESSIONS, TPM2_CC_RSA_DECRYPT);
	ret = tpm_buf_append_name(chip, &buf, key_handle, NULL);
	if (ret)
		goto err_key;
	tpm_buf_append_hmac_session(chip, &buf, TPM2_SA_DECRYPT, NULL, 0);
	tpm_buf_append_u16(&buf, params->in_len);
	tpm_buf_append(&buf, in, params->in_len);
	tpm_buf_append_u16(&buf, TPM_ALG_RSAES);
	tpm_buf_append_u16(&buf, 0);
	if (buf.flags & TPM_BUF_OVERFLOW) {
		tpm2_end_auth_session(chip);
		ret = -E2BIG;
		goto err_key;
	}
	ret = tpm_buf_fill_hmac_session(chip, &buf);
	if (ret)
		goto err_key;
	ret = tpm_transmit_cmd(chip, &buf, 4, "TPM2_RSA_DECRYPT");
	ret = tpm_buf_check_hmac_response(chip, &buf, ret);
	if (ret) {
		ret = -EIO;
		goto err_key;
	}

	offset = TPM_HEADER_SIZE + 4;
	decrypted_len = tpm_buf_read_u16(&buf, &offset);
	if (buf.flags & TPM_BUF_BOUNDARY_ERROR) {
		ret = -EIO;
		goto err_key;
	}
	if (offset + decrypted_len > buf.length) {
		ret = -EIO;
		goto err_key;
	}

	if (params->out_len < decrypted_len) {
		ret = -EMSGSIZE;
		goto err_key;
	}

	memcpy(out, &buf.data[offset], decrypted_len);
	ret = decrypted_len;

err_key:
	tpm2_flush_context(chip, key_handle);
	tpm_buf_destroy(&buf);

err_ops:
	tpm_put_ops(chip);
	return ret;
}

/*
 * Verify an RSA signature using TPM2_VerifySignature with RSASSA scheme.
 */
static int tpm2_asymmetric_rsa_verify(const struct key *key,
				      const struct public_key_signature *sig)
{
	struct tpm2_key *tpm2_key = key->payload.data[asym_crypto];
	struct tpm_chip *chip;
	struct tpm_buf buf;
	u32 key_handle = 0;
	int tpm_hash;
	int ret;

	if (!sig->m)
		return -ENOPKG;

	if (!sig->encoding || strcmp(sig->encoding, "pkcs1") != 0)
		return -ENOPKG;

	if (!sig->hash_algo)
		return -EINVAL;

	chip = tpm_default_chip();

	if (!chip)
		return -ENODEV;

	ret = tpm2_asymmetric_hash_lookup(sig->hash_algo, NULL, &tpm_hash);
	if (ret)
		goto err_chip;

	ret = tpm_try_get_ops(chip);
	if (ret)
		goto err_chip;

	ret = tpm2_asymmetric_load(chip, tpm2_key, &buf, &key_handle);
	if (ret)
		goto err_ops;

	tpm2_end_auth_session(chip);
	tpm_buf_destroy(&buf);

	ret = tpm_buf_init(&buf, TPM2_ST_NO_SESSIONS,
			   TPM2_CC_VERIFY_SIGNATURE);
	if (ret)
		goto err_key;

	tpm_buf_append_u32(&buf, key_handle);

	tpm_buf_append_u16(&buf, sig->m_size);
	tpm_buf_append(&buf, sig->m, sig->m_size);

	tpm_buf_append_u16(&buf, TPM_ALG_RSASSA);
	tpm_buf_append_u16(&buf, tpm_hash);
	tpm_buf_append_u16(&buf, sig->s_size);
	tpm_buf_append(&buf, sig->s, sig->s_size);

	ret = tpm_transmit_cmd(chip, &buf, 0, "TPM2_VerifySignature");
	if (ret)
		ret = -EKEYREJECTED;

	tpm_buf_destroy(&buf);

err_key:
	tpm2_flush_context(chip, key_handle);

err_ops:
	tpm_put_ops(chip);

err_chip:
	put_device(&chip->dev);
	return ret;
}

static int tpm2_asymmetric_rsa_query(const struct kernel_pkey_params *params,
				     struct kernel_pkey_query *info)
{
	const struct tpm2_key *key = params->key->payload.data[asym_crypto];
	u16 max_data_size = TPM2_MAX_DIGEST_SIZE;
	const u16 mod_size = tpm2_asymmetric_rsa_mod_size(key);
	int hash_id, ret;

	if (!params->encoding)
		return -EINVAL;

	memset(info, 0, sizeof(*info));
	info->key_size = mod_size * 8;

	if (strcmp(params->encoding, "pkcs1") == 0) {
		if (params->hash_algo) {
			ret = tpm2_asymmetric_hash_lookup(params->hash_algo, &hash_id, NULL);
			if (ret)
				return ret;
			max_data_size = hash_digest_size[hash_id];
		}

		info->max_data_size = max_data_size;
		info->max_sig_size = mod_size;
		info->max_enc_size = mod_size;
		info->max_dec_size = mod_size;
		info->supported_ops = KEYCTL_SUPPORTS_SIGN |
				      KEYCTL_SUPPORTS_VERIFY |
				      KEYCTL_SUPPORTS_ENCRYPT |
				      KEYCTL_SUPPORTS_DECRYPT;
		return 0;
	}

	if (strcmp(params->encoding, "raw") == 0) {
		info->max_data_size = mod_size;
		info->max_enc_size = mod_size;
		info->max_dec_size = mod_size;
		info->supported_ops = KEYCTL_SUPPORTS_ENCRYPT;
		return 0;
	}

	return -ENOPKG;
}

static int tpm2_asymmetric_rsa_validate(const struct tpm2_key *key)
{
	const struct tpm2_asymmetric_rsa_parms *p = tpm2_asymmetric_parms(key);
	u16 key_bits;
	u16 mod_size;

	if (tpm2_key_policy_size(key) != 0)
		return -EBADMSG;

	if (key->pub_len < 2 + sizeof(*key->desc) + sizeof(*p))
		return -EBADMSG;

	if (be16_to_cpu(p->symmetric) != TPM_ALG_NULL)
		return -EBADMSG;

	if (be16_to_cpu(p->scheme) != TPM_ALG_NULL)
		return -EBADMSG;

	key_bits = be16_to_cpu(p->key_bits);
	if (key_bits != 2048 && key_bits != 3072 && key_bits != 4096)
		return -EBADMSG;

	if (be32_to_cpu(p->exponent) != 0x00000000 &&
	    be32_to_cpu(p->exponent) != 0x00010001)
		return -EBADMSG;

	mod_size = tpm2_asymmetric_rsa_mod_size(key);
	if (mod_size != key_bits / 8)
		return -EBADMSG;

	if (key->pub_len < 2 + sizeof(*key->desc) + sizeof(*p) + mod_size)
		return -EBADMSG;

	return 0;
}

static unsigned int tpm2_asymmetric_der_len_size(unsigned int len)
{
	if (len < 128)
		return 1;
	if (len <= 255)
		return 2;
	return 3;
}

/*
 * Parse a DER-encoded ECDSA signature: SEQUENCE { INTEGER r, INTEGER s }.
 *
 * On success, @r/@r_len and @s/@s_len point into @der with leading zero
 * pads stripped.
 */
static int tpm2_asymmetric_ecc_parse_der_signature(const u8 *der, u32 der_len,
						   const u8 **r, u16 *r_len,
						   const u8 **s, u16 *s_len)
{
	const u8 *end = der + der_len;
	u32 seq_len, int_len;
	const u8 *p = der;

	if (p >= end || *p++ != 0x30)
		return -EBADMSG;

	if (p >= end)
		return -EBADMSG;
	if (*p < 0x80) {
		seq_len = *p++;
	} else if (*p == 0x81) {
		if (++p >= end)
			return -EBADMSG;
		seq_len = *p++;
	} else {
		return -EBADMSG;
	}

	if (p + seq_len > end)
		return -EBADMSG;
	end = p + seq_len;

	/* INTEGER r */
	if (p >= end || *p++ != 0x02)
		return -EBADMSG;
	if (p >= end)
		return -EBADMSG;
	int_len = *p++;
	if (int_len == 0 || int_len >= 0x80 || p + int_len > end)
		return -EBADMSG;
	while (int_len > 1 && *p == 0x00) {
		p++;
		int_len--;
	}
	*r = p;
	*r_len = int_len;
	p += int_len;

	/* INTEGER s */
	if (p >= end || *p++ != 0x02)
		return -EBADMSG;
	if (p >= end)
		return -EBADMSG;
	int_len = *p++;
	if (int_len == 0 || int_len >= 0x80 || p + int_len > end)
		return -EBADMSG;
	while (int_len > 1 && *p == 0x00) {
		p++;
		int_len--;
	}
	*s = p;
	*s_len = int_len;
	p += int_len;

	if (p != end)
		return -EBADMSG;

	return 0;
}

/*
 * Verify an ECDSA signature using TPM2_VerifySignature.
 *
 * A DER-encoded signature is parsed into (r, s) components for the TPM command.
 */
static int tpm2_asymmetric_ecc_verify(const struct key *key,
				      const struct public_key_signature *sig)
{
	struct tpm2_key *tpm2_key = key->payload.data[asym_crypto];
	struct tpm_chip *chip;
	const u8 *r, *s_data;
	struct tpm_buf buf;
	u32 key_handle = 0;
	u16 r_len, s_len;
	int tpm_hash;
	int ret;

	if (!sig->m)
		return -ENOPKG;

	if (!sig->encoding || strcmp(sig->encoding, "x962") != 0)
		return -ENOPKG;

	if (!sig->hash_algo)
		return -EINVAL;

	chip = tpm_default_chip();

	if (!chip)
		return -ENODEV;

	ret = tpm2_asymmetric_hash_lookup(sig->hash_algo, NULL, &tpm_hash);
	if (ret)
		goto err_chip;

	ret = tpm2_asymmetric_ecc_parse_der_signature(sig->s, sig->s_size,
						      &r, &r_len, &s_data,
						      &s_len);
	if (ret)
		goto err_chip;

	ret = tpm_try_get_ops(chip);
	if (ret)
		goto err_chip;

	ret = tpm2_asymmetric_load(chip, tpm2_key, &buf, &key_handle);
	if (ret)
		goto err_ops;

	tpm2_end_auth_session(chip);
	tpm_buf_destroy(&buf);

	ret = tpm_buf_init(&buf, TPM2_ST_NO_SESSIONS,
			   TPM2_CC_VERIFY_SIGNATURE);
	if (ret)
		goto err_key;

	tpm_buf_append_u32(&buf, key_handle);

	/* digest (TPM2B_DIGEST) */
	tpm_buf_append_u16(&buf, sig->m_size);
	tpm_buf_append(&buf, sig->m, sig->m_size);

	/* signature (TPMT_SIGNATURE): ECDSA with the given hash */
	tpm_buf_append_u16(&buf, TPM_ALG_ECDSA);
	tpm_buf_append_u16(&buf, tpm_hash);

	/* signatureR (TPM2B_ECC_PARAMETER) */
	tpm_buf_append_u16(&buf, r_len);
	tpm_buf_append(&buf, r, r_len);

	/* signatureS (TPM2B_ECC_PARAMETER) */
	tpm_buf_append_u16(&buf, s_len);
	tpm_buf_append(&buf, s_data, s_len);

	ret = tpm_transmit_cmd(chip, &buf, 0, "TPM2_VerifySignature");
	if (ret)
		ret = -EKEYREJECTED;

	tpm_buf_destroy(&buf);

err_key:
	tpm2_flush_context(chip, key_handle);

err_ops:
	tpm_put_ops(chip);

err_chip:
	put_device(&chip->dev);
	return ret;
}

static int tpm2_asymmetric_ecc_query(const struct kernel_pkey_params *params,
				     struct kernel_pkey_query *info)
{
	const struct tpm2_key *key = params->key->payload.data[asym_crypto];
	const struct tpm2_asymmetric_ecc_parms *p = tpm2_asymmetric_parms(key);
	unsigned int int_len, seq_payload;
	const u8 *x;
	u16 ecc, n;
	int ret;

	ecc = be16_to_cpu(p->ecc);
	x = tpm2_asymmetric_ecc_x(key);
	n = get_unaligned_be16(&x[0]);
	int_len = n + 1;

	if (!params->encoding || strcmp(params->encoding, "x962") != 0)
		return -ENOPKG;

	ret = tpm2_asymmetric_hash_lookup(params->hash_algo, NULL, NULL);
	if (ret)
		return ret;

	/*
	 * SEQUENCE { INTEGER (<=n+1 bytes), INTEGER (<=n+1 bytes) }
	 */
	seq_payload = 2 * (1 + tpm2_asymmetric_der_len_size(int_len) + int_len);

	memset(info, 0, sizeof(*info));
	info->key_size = tpm2_asymmetric_ecc_key_bits(ecc);
	info->max_sig_size = 1 + tpm2_asymmetric_der_len_size(seq_payload) + seq_payload;
	info->max_data_size = TPM2_MAX_DIGEST_SIZE;
	info->supported_ops = KEYCTL_SUPPORTS_SIGN | KEYCTL_SUPPORTS_VERIFY;

	return 0;
}

static int tpm2_asymmetric_ecc_validate(const struct tpm2_key *key)
{
	const struct tpm2_asymmetric_ecc_parms *p = tpm2_asymmetric_parms(key);
	size_t min_len = 2 + sizeof(*key->desc) + sizeof(*p);
	u16 x_size, y_size;
	const u8 *x, *y;

	if (tpm2_key_policy_size(key) != 0)
		return -EBADMSG;

	if (key->pub_len < min_len + 2)
		return -EBADMSG;

	if (be16_to_cpu(p->symmetric) != TPM_ALG_NULL)
		return -EBADMSG;

	if (be16_to_cpu(p->scheme) != TPM_ALG_NULL)
		return -EBADMSG;

	if (be16_to_cpu(p->ecc) != TPM2_ECC_NIST_P256 &&
	    be16_to_cpu(p->ecc) != TPM2_ECC_NIST_P384 &&
	    be16_to_cpu(p->ecc) != TPM2_ECC_NIST_P521)
		return -EBADMSG;

	if (be16_to_cpu(p->kdf) != TPM_ALG_NULL)
		return -EBADMSG;

	x = tpm2_asymmetric_ecc_x(key);
	x_size = get_unaligned_be16(&x[0]);
	if (x_size > ECC_MAX_BYTES)
		return -EBADMSG;

	if (key->pub_len < min_len + 2 + x_size + 2)
		return -EBADMSG;

	y = tpm2_asymmetric_ecc_y(key);
	y_size = get_unaligned_be16(&y[0]);
	if (y_size > ECC_MAX_BYTES)
		return -EBADMSG;

	if (key->pub_len < min_len + 2 + x_size + 2 + y_size)
		return -EBADMSG;

	if (x_size != y_size)
		return -EBADMSG;

	return 0;
}

static const char *tpm2_asymmetric_ecc_name(const struct tpm2_key *key)
{
	const struct tpm2_asymmetric_ecc_parms *p;

	p = tpm2_asymmetric_parms(key);

	switch (be16_to_cpu(p->ecc)) {
	case TPM2_ECC_NIST_P256:
		return "ecdsa-nist-p256";
	case TPM2_ECC_NIST_P384:
		return "ecdsa-nist-p384";
	case TPM2_ECC_NIST_P521:
		return "ecdsa-nist-p521";
	default:
		return "ecdsa";
	}
}

static void tpm2_asymmetric_describe(const struct key *asymmetric_key,
				     struct seq_file *m)
{
	const struct tpm2_key *key;

	key = asymmetric_key->payload.data[asym_crypto];
	if (!key)
		return;

	switch (tpm2_key_type(key)) {
	case TPM_ALG_RSA:
		seq_puts(m, "tpm2.rsa");
		break;
	case TPM_ALG_ECC:
		seq_printf(m, "tpm2.%s", tpm2_asymmetric_ecc_name(key));
		break;
	default:
		seq_puts(m, "tpm2.unknown");
		break;
	}
}

static int tpm2_asymmetric_query(const struct kernel_pkey_params *params,
				 struct kernel_pkey_query *info)
{
	struct tpm2_key *key = params->key->payload.data[asym_crypto];

	switch (tpm2_key_type(key)) {
	case TPM_ALG_RSA:
		return tpm2_asymmetric_rsa_query(params, info);
	case TPM_ALG_ECC:
		return tpm2_asymmetric_ecc_query(params, info);
	default:
		return -EOPNOTSUPP;
	}
}

static int tpm2_asymmetric_eds_op(struct kernel_pkey_params *params,
				  const void *in, void *out)
{
	struct tpm2_key *key = params->key->payload.data[asym_crypto];
	struct tpm_chip *chip;
	int ret;

	chip = tpm_default_chip();
	if (!chip)
		return -ENODEV;

	switch (params->op) {
	case kernel_pkey_encrypt:
		if (tpm2_key_type(key) != TPM_ALG_RSA) {
			ret = -EOPNOTSUPP;
			break;
		}
		ret = tpm2_asymmetric_rsa_encrypt(chip, key, params, in, out);
		break;
	case kernel_pkey_decrypt:
		if (tpm2_key_type(key) != TPM_ALG_RSA) {
			ret = -EOPNOTSUPP;
			break;
		}
		ret = tpm2_asymmetric_rsa_decrypt(chip, key, params, in, out);
		break;
	case kernel_pkey_sign:
		ret = tpm2_asymmetric_sign(chip, key, params, in, out);
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}

	put_device(&chip->dev);
	return ret;
}

static int tpm2_asymmetric_verify_signature(const struct key *asymmetric_key,
					    const struct public_key_signature *sig)
{
	struct tpm2_key *key = asymmetric_key->payload.data[asym_crypto];

	switch (tpm2_key_type(key)) {
	case TPM_ALG_RSA:
		return tpm2_asymmetric_rsa_verify(asymmetric_key, sig);
	case TPM_ALG_ECC:
		return tpm2_asymmetric_ecc_verify(asymmetric_key, sig);
	default:
		return -EOPNOTSUPP;
	}
}

static struct asymmetric_key_subtype tpm2_asymmetric_subtype = {
	.owner			= THIS_MODULE,
	.name			= "tpm2_asymmetric_key",
	.name_len		= sizeof("tpm2_asymmetric_key") - 1,
	.describe		= tpm2_asymmetric_describe,
	.destroy		= tpm2_asymmetric_key_destroy,
	.query			= tpm2_asymmetric_query,
	.eds_op			= tpm2_asymmetric_eds_op,
	.verify_signature	= tpm2_asymmetric_verify_signature,
};

static int tpm2_asymmetric_preparse(struct key_preparsed_payload *prep)
{
	struct tpm2_key *key __free(kfree) = NULL;
	int ret;

	key = tpm2_key_decode(prep->data, prep->datalen);
	if (IS_ERR(key)) {
		ret = PTR_ERR(key);
		key = NULL;
		return ret;
	}

	if (key->oid != OID_TPMLoadableKey)
		return -EBADMSG;

	switch (tpm2_key_type(key)) {
	case TPM_ALG_RSA:
		ret = tpm2_asymmetric_rsa_validate(key);
		break;
	case TPM_ALG_ECC:
		ret = tpm2_asymmetric_ecc_validate(key);
		break;
	default:
		ret = -EBADMSG;
		break;
	}

	if (ret < 0)
		return ret;

	__module_get(tpm2_asymmetric_subtype.owner);

	prep->payload.data[asym_subtype] = &tpm2_asymmetric_subtype;
	prep->payload.data[asym_key_ids] = NULL;
	prep->payload.data[asym_crypto] = no_free_ptr(key);
	prep->payload.data[asym_auth] = NULL;
	prep->quotalen = 100;

	return 0;
}

static struct asymmetric_key_parser tpm2_asymmetric_parser = {
	.owner	= THIS_MODULE,
	.name	= "tpm2_asymmetric_parser",
	.parse	= tpm2_asymmetric_preparse,
};

static int __init tpm2_asymmetric_init(void)
{
	return register_asymmetric_key_parser(&tpm2_asymmetric_parser);
}

static void __exit tpm2_asymmetric_exit(void)
{
	unregister_asymmetric_key_parser(&tpm2_asymmetric_parser);
}

module_init(tpm2_asymmetric_init);
module_exit(tpm2_asymmetric_exit);

MODULE_DESCRIPTION("Asymmetric TPM2 key");
MODULE_LICENSE("GPL");
