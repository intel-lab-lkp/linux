#include "derived.h"
#include <linux/tpm.h>
#include <linux/tpm_command.h>

#include <crypto/internal/hash.h>
#include <crypto/sha2.h>

/* create hierarchy with a custom unique value */
#define TPM2_KERNEL_HIERARCHY "kernel"

static struct tpm_chip *chip = NULL;

static int tpm2_hash_init(struct shash_desc *desc)
{
	struct tpm_buf *buf = shash_desc_ctx(desc);
	int ret = tpm_buf_init(buf, TPM2_ST_NO_SESSIONS, TPM2_CC_HASH);
	if (ret)
		return ret;

	/* provisional size value for data size to be hashed */
	tpm_buf_append_u16(buf, 0);

	return 0;
}

static int tpm2_hash_update(struct shash_desc *desc, const u8 *data,
			    unsigned int len)
{
	struct tpm_buf *buf = shash_desc_ctx(desc);

	tpm_buf_append(buf, data, len);

	if (buf->flags & TPM_BUF_OVERFLOW) {
		tpm_buf_destroy(buf);
		return -ENOMEM;
	}

	return 0;
}

static inline void tpm2_buf_append_null_auth(struct tpm_buf *buf)
{
	tpm_buf_append_u32(buf, 9);
	tpm_buf_append_u32(buf, TPM2_RS_PW);
	tpm_buf_append_u16(buf, 0); /* nonce len */
	tpm_buf_append_u8(buf, 0); /* attributes */
	tpm_buf_append_u16(buf, 0); /* hmac len */
}

static int tpm2_hmac_create_primary(u32 *handle)
{
	struct tpm_buf buf;

	int ret = tpm_buf_init(&buf, TPM2_ST_SESSIONS, TPM2_CC_CREATE_PRIMARY);
	if (ret)
		return ret;

	/* owner hierarchy */
	tpm_buf_append_u32(&buf, TPM2_RH_OWNER);

	tpm2_buf_append_null_auth(&buf);

	/* tpm2 sensitive */
	tpm_buf_append_u16(&buf, 4);
	tpm_buf_append_u16(&buf, 0);
	tpm_buf_append_u16(&buf, 0);

	/* tpm2 public */
	tpm_buf_append_u16(&buf, 16 + sizeof(TPM2_KERNEL_HIERARCHY) - 1);
	tpm_buf_append_u16(&buf, TPM_ALG_KEYEDHASH);
	tpm_buf_append_u16(&buf, TPM_ALG_SHA256);
	tpm_buf_append_u32(&buf, TPM2_OA_FIXED_TPM | TPM2_OA_FIXED_PARENT |
					 TPM2_OA_SENSITIVE_DATA_ORIGIN |
					 TPM2_OA_USER_WITH_AUTH |
					 TPM2_OA_RESTRICTED |
					 TPM2_OA_SIGN); /* attr */
	tpm_buf_append_u16(&buf, 0); /* auth policy */
	tpm_buf_append_u16(&buf, TPM_ALG_HMAC);
	tpm_buf_append_u16(&buf, TPM_ALG_SHA256);
	tpm_buf_append_u16(&buf,
			   sizeof(TPM2_KERNEL_HIERARCHY) - 1); /* unique len */
	tpm_buf_append(&buf, TPM2_KERNEL_HIERARCHY,
		       sizeof(TPM2_KERNEL_HIERARCHY) - 1); /* unique */

	/* outside info */
	tpm_buf_append_u16(&buf, 0);

	/* pcr selection */
	tpm_buf_append_u32(&buf, 0);

	if (buf.flags & TPM_BUF_OVERFLOW) {
		ret = -ENOMEM;
		goto free_buf;
	}

	ret = tpm_transmit_cmd(chip, &buf, 4,
			       "create primary kernel hmac hierarchy");
	if (ret < 0)
		goto free_buf;

	if (ret > 0) {
		ret = tpm2_rc_value(ret) == TPM2_RC_OBJECT_MEMORY ? -ENOMEM :
								    -EPERM;
		goto free_buf;
	}

	*handle = be32_to_cpup((__be32 *)&buf.data[TPM_HEADER_SIZE]);

free_buf:
	tpm_buf_destroy(&buf);
	return ret;
}

static int tpm2_sign(u32 handle, const u8 *tpm2b_digest, size_t digest_len,
		     const u8 *ticket, size_t ticket_len, u8 *out)
{
	struct tpm_buf buf;

	int ret = tpm_buf_init(&buf, TPM2_ST_SESSIONS, TPM2_CC_SIGN);
	if (ret)
		return ret;

	/* signing key handle */
	tpm_buf_append_u32(&buf, handle);

	tpm2_buf_append_null_auth(&buf);

	/* digest to sign */
	tpm_buf_append(&buf, tpm2b_digest, digest_len);

	/* sig scheme */
	tpm_buf_append_u16(&buf, TPM_ALG_HMAC);
	tpm_buf_append_u16(&buf, TPM_ALG_SHA256);

	/* validation (needed for restricted keys) */
	tpm_buf_append(&buf, ticket, ticket_len);

	if (buf.flags & TPM_BUF_OVERFLOW) {
		ret = -ENOMEM;
		goto free_buf;
	}

	ret = tpm_transmit_cmd(chip, &buf, 4 + 2 + 2 + SHA256_DIGEST_SIZE,
			       "sign data");
	if (ret < 0)
		goto free_buf;

	if (ret > 0) {
		ret = tpm2_rc_value(ret) == TPM2_RC_OBJECT_MEMORY ? -ENOMEM :
								    -EPERM;
		goto free_buf;
	}

	/* check resp len */
	if (be32_to_cpup((__be32 *)&buf.data[TPM_HEADER_SIZE]) !=
	    2 + 2 + SHA256_DIGEST_SIZE) {
		ret = -EFAULT;
		goto free_buf;
	}

	memcpy(out, &buf.data[TPM_HEADER_SIZE + 4 + 2 + 2], SHA256_DIGEST_SIZE);
	memzero_explicit(&buf.data[TPM_HEADER_SIZE + 4 + 2 + 2],
			 SHA256_DIGEST_SIZE);

free_buf:
	tpm_buf_destroy(&buf);
	return ret;
}

static int tpm2_hash_final(struct shash_desc *desc, u8 *out)
{
	u32 handle;
	int ret;
	size_t digest_len;
	struct tpm_buf *buf = shash_desc_ctx(desc);

	/* adjust the input data length */
	*((__be16 *)&buf->data[TPM_HEADER_SIZE]) =
		cpu_to_be16(tpm_buf_length(buf) - TPM_HEADER_SIZE - 2);

	tpm_buf_append_u16(buf, TPM_ALG_SHA256);
	tpm_buf_append_u32(buf, TPM2_RH_OWNER);

	if (buf->flags & TPM_BUF_OVERFLOW) {
		ret = -ENOMEM;
		goto free_buf;
	}

	ret = tpm_try_get_ops(chip);
	if (ret)
		goto free_buf;

	ret = tpm_transmit_cmd(chip, buf, 2 + SHA256_DIGEST_SIZE, "hash data");
	if (ret < 0)
		goto put_ops;

	if (ret > 0) {
		ret = -EPERM;
		goto put_ops;
	}

	ret = tpm2_hmac_create_primary(&handle);
	if (ret)
		goto put_ops;

	digest_len = be16_to_cpup((__be16 *)&buf->data[TPM_HEADER_SIZE]) + 2;
	ret = tpm2_sign(handle, &buf->data[TPM_HEADER_SIZE], digest_len,
			&buf->data[TPM_HEADER_SIZE + digest_len],
			tpm_buf_length(buf) - TPM_HEADER_SIZE - digest_len,
			out);

	tpm2_flush_context(chip, handle);

put_ops:
	tpm_put_ops(chip);
free_buf:
	tpm_buf_destroy(buf);
	return ret;
}

static struct shash_alg alg = {
	.digestsize = SHA256_DIGEST_SIZE,
	.init = tpm2_hash_init,
	.update = tpm2_hash_update,
	.final = tpm2_hash_final,
	.descsize = sizeof(struct tpm_buf),
	.base = {
		.cra_name = "sha256",
		.cra_driver_name = TPM2_HASH_IMPL_NAME,
		.cra_priority = 0,
		.cra_blocksize = SHA256_BLOCK_SIZE,
		.cra_flags = CRYPTO_ALG_INTERNAL,
		.cra_module = THIS_MODULE,
	}
};

int register_tpm2_shash(void)
{
	int ret;

	chip = tpm_default_chip();
	if (!chip)
		return -ENODEV;

	ret = crypto_register_shash(&alg);
	if (ret)
		put_device(&chip->dev);

	return ret;
}

void unregister_tpm2_shash(void)
{
	crypto_unregister_shash(&alg);
	if (chip)
		put_device(&chip->dev);
}
