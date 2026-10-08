// SPDX-License-Identifier: GPL-2.0-only
/*
 * Kernel attestation key (AK) support.
 *
 * The kernel AK is a restricted ECDSA P-256 signing key created as a
 * primary key in the owner hierarchy from a fixed template.  Primary keys
 * are derived from the hierarchy seed and the template, so on a given TPM
 * the same AK is generated every time until the owner seed changes, which
 * happens when the TPM is cleared.
 *
 * The AK can be used to sign the digest of the kernel's audit session,
 * allowing the log returned by tpm2_get_audit_log() to be verified.
 */

#include <linux/slab.h>
#include <linux/unaligned.h>
#include "tpm.h"

/*
 * Restricted signing key whose private part never leaves the TPM.  NO_DA
 * as the key has an empty auth value.
 */
#define TPM2_OA_KERNEL_AK ( \
	TPM2_OA_FIXED_TPM | \
	TPM2_OA_FIXED_PARENT | \
	TPM2_OA_SENSITIVE_DATA_ORIGIN | \
	TPM2_OA_USER_WITH_AUTH | \
	TPM2_OA_NO_DA | \
	TPM2_OA_RESTRICTED | \
	TPM2_OA_SIGN)

/*
 * Placed in the unique field of the template.  Changing it changes the
 * AK derived from the owner seed.
 */
static const u8 tpm2_kernel_ak_seed[EC_PT_SZ] =
	"Linux kernel attestation key v1";

/* Bounds-checked reader for TPM response data */
struct tpm2_rsp {
	const u8 *data;
	u32 len;
	u32 off;
	bool err;
};

static const u8 *tpm2_rsp_bytes(struct tpm2_rsp *r, u32 count)
{
	const u8 *p;

	if (r->err || r->len - r->off < count) {
		r->err = true;
		return NULL;
	}

	p = &r->data[r->off];
	r->off += count;
	return p;
}

static u16 tpm2_rsp_u16(struct tpm2_rsp *r)
{
	const u8 *p = tpm2_rsp_bytes(r, sizeof(u16));

	return p ? get_unaligned_be16(p) : 0;
}

static u32 tpm2_rsp_u32(struct tpm2_rsp *r)
{
	const u8 *p = tpm2_rsp_bytes(r, sizeof(u32));

	return p ? get_unaligned_be32(p) : 0;
}

/* Initialise a reader over the response held in @buf */
static void tpm2_rsp_init(struct tpm2_rsp *r, struct tpm_buf *buf)
{
	struct tpm_header *head = (struct tpm_header *)buf->data;

	r->data = buf->data;
	r->len = min_t(u32, be32_to_cpu(head->length), TPM_BUFSIZE);
	r->off = TPM_HEADER_SIZE;
	r->err = r->len < TPM_HEADER_SIZE;
}

/* Read a TPM2B_ECC_PARAMETER, left-padding it to EC_PT_SZ bytes */
static void tpm2_rsp_ecc_param(struct tpm2_rsp *r, u8 *out)
{
	u16 len = tpm2_rsp_u16(r);
	const u8 *p;

	if (len > EC_PT_SZ) {
		r->err = true;
		return;
	}

	p = tpm2_rsp_bytes(r, len);
	if (!p)
		return;

	memset(out, 0, EC_PT_SZ - len);
	memcpy(out + EC_PT_SZ - len, p, len);
}

/* Parse and validate the TPM2B_PUBLIC of the kernel AK */
static int tpm2_parse_kernel_ak_public(struct tpm2_rsp *r, u8 *x, u8 *y)
{
	u16 size = tpm2_rsp_u16(r);
	u32 end = r->off + size;

	if (tpm2_rsp_u16(r) != TPM_ALG_ECC ||
	    tpm2_rsp_u16(r) != TPM_ALG_SHA256 ||
	    tpm2_rsp_u32(r) != TPM2_OA_KERNEL_AK ||
	    tpm2_rsp_u16(r) != 0 ||			/* authPolicy */
	    tpm2_rsp_u16(r) != TPM_ALG_NULL ||		/* symmetric */
	    tpm2_rsp_u16(r) != TPM_ALG_ECDSA ||		/* scheme */
	    tpm2_rsp_u16(r) != TPM_ALG_SHA256 ||	/* scheme hash */
	    tpm2_rsp_u16(r) != TPM2_ECC_NIST_P256 ||
	    tpm2_rsp_u16(r) != TPM_ALG_NULL)		/* kdf */
		return -EINVAL;

	tpm2_rsp_ecc_param(r, x);
	tpm2_rsp_ecc_param(r, y);

	if (r->err || r->off != end)
		return -EINVAL;

	return 0;
}

/**
 * tpm2_create_kernel_ak() - create the kernel attestation key
 * @chip:	the TPM chip
 * @handle:	set to the transient handle of the AK on success
 * @x:		if not NULL, filled with the X coordinate of the AK public key
 * @y:		if not NULL, filled with the Y coordinate of the AK public key
 *
 * Creates the kernel AK as a primary key in the owner hierarchy using a
 * fixed template, so the same key is returned on every call until the
 * TPM is cleared.  The owner hierarchy must have an empty auth value.
 * The caller must hold the chip's ops lock and is responsible for
 * flushing @handle with tpm2_flush_context().
 *
 * Return:
 * * 0		- OK
 * * -errno	- A system error
 * * TPM_RC	- A TPM error
 */
int tpm2_create_kernel_ak(struct tpm_chip *chip, u32 *handle, u8 *x, u8 *y)
{
	struct tpm_buf *template __free(kfree) = NULL;
	struct tpm_buf *buf __free(kfree) = NULL;
	u8 ak_x[EC_PT_SZ], ak_y[EC_PT_SZ];
	struct tpm2_rsp r;
	u32 ak;
	int rc;

	buf = kzalloc(TPM_BUFSIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	template = kzalloc(TPM_BUFSIZE, GFP_KERNEL);
	if (!template)
		return -ENOMEM;

	tpm_buf_init(buf, TPM_BUFSIZE);
	tpm_buf_reset(buf, TPM2_ST_SESSIONS, TPM2_CC_CREATE_PRIMARY);
	tpm_buf_init_sized(template, TPM_BUFSIZE);

	/* key type */
	tpm_buf_append_u16(template, TPM_ALG_ECC);
	/* name algorithm */
	tpm_buf_append_u16(template, TPM_ALG_SHA256);
	/* object properties */
	tpm_buf_append_u32(template, TPM2_OA_KERNEL_AK);
	/* auth policy (empty) */
	tpm_buf_append_u16(template, 0);
	/* symmetric algorithm (none for a signing key) */
	tpm_buf_append_u16(template, TPM_ALG_NULL);
	/* signing scheme */
	tpm_buf_append_u16(template, TPM_ALG_ECDSA);
	tpm_buf_append_u16(template, TPM_ALG_SHA256);
	/* ECC curve */
	tpm_buf_append_u16(template, TPM2_ECC_NIST_P256);
	/* KDF scheme */
	tpm_buf_append_u16(template, TPM_ALG_NULL);
	/* unique: the fixed seed as X, empty Y */
	tpm_buf_append_u16(template, sizeof(tpm2_kernel_ak_seed));
	tpm_buf_append(template, tpm2_kernel_ak_seed,
		       sizeof(tpm2_kernel_ak_seed));
	tpm_buf_append_u16(template, 0);

	/* primary handle */
	tpm_buf_append_handle(buf, TPM2_RH_OWNER);
	tpm_buf_append_auth(chip, buf, NULL, 0);

	/* sensitive create: empty auth and data */
	tpm_buf_append_u16(buf, 4);
	tpm_buf_append_u16(buf, 0);
	tpm_buf_append_u16(buf, 0);

	/* the public template */
	tpm_buf_append(buf, template->data, template->length);

	/* outside info (empty) */
	tpm_buf_append_u16(buf, 0);

	/* creation PCR (none) */
	tpm_buf_append_u32(buf, 0);

	if (buf->flags & TPM_BUF_INVALID || template->flags & TPM_BUF_INVALID)
		return -EINVAL;

	rc = tpm_transmit_cmd(chip, buf, 0, "creating kernel AK");
	if (rc)
		return rc;

	tpm2_rsp_init(&r, buf);
	ak = tpm2_rsp_u32(&r);
	/* parameterSize */
	tpm2_rsp_u32(&r);
	if (r.err)
		return -EINVAL;

	rc = tpm2_parse_kernel_ak_public(&r, ak_x, ak_y);
	if (rc) {
		dev_err(&chip->dev, "unexpected kernel AK public area\n");
		tpm2_flush_context(chip, ak);
		return rc;
	}

	if (x)
		memcpy(x, ak_x, EC_PT_SZ);
	if (y)
		memcpy(y, ak_y, EC_PT_SZ);
	*handle = ak;

	return 0;
}
EXPORT_SYMBOL_GPL(tpm2_create_kernel_ak);

/**
 * tpm2_get_signed_audit_digest() - get the audit session digest signed by the AK
 * @chip:	the TPM chip
 * @nonce:	qualifying data to include in the attestation (may be NULL)
 * @nonce_len:	length of @nonce
 * @audit:	filled with the signed attestation and the AK public key
 *
 * Creates the kernel AK and uses TPM2_GetSessionAuditDigest to have it
 * sign the digest of the active audit session.  The session itself is not
 * used to authorize the command, so the audit digest and the log returned
 * by tpm2_get_audit_log() are unaffected.  The endorsement and owner
 * hierarchies must have empty auth values.  The caller must hold the
 * chip's ops lock and must release @audit with tpm2_free_signed_audit().
 *
 * Return:
 * * 0		- OK
 * * -EINVAL	- No audit session is active, or the response was malformed
 * * -errno	- A system error
 * * TPM_RC	- A TPM error
 */
int tpm2_get_signed_audit_digest(struct tpm_chip *chip, const u8 *nonce,
				 u16 nonce_len,
				 struct tpm2_signed_audit *audit)
{
	struct tpm_buf *buf __free(kfree) = NULL;
	u32 session, ak, param_size, end;
	const u8 *attest;
	struct tpm2_rsp r;
	u16 attest_len;
	int rc;

	memset(audit, 0, sizeof(*audit));

	if (nonce_len > TPM2_MAX_DIGEST_SIZE)
		return -EINVAL;

	rc = tpm2_audit_session_handle(chip, &session);
	if (rc)
		return rc;

	buf = kzalloc(TPM_BUFSIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	rc = tpm2_create_kernel_ak(chip, &ak, audit->ak_x, audit->ak_y);
	if (rc)
		return rc;

	tpm_buf_init(buf, TPM_BUFSIZE);
	tpm_buf_reset(buf, TPM2_ST_SESSIONS, TPM2_CC_GET_SESSION_AUDIT_DIGEST);

	/* privacyAdminHandle, signHandle, sessionHandle */
	tpm_buf_append_handle(buf, TPM2_RH_ENDORSEMENT);
	tpm_buf_append_handle(buf, ak);
	tpm_buf_append_handle(buf, session);

	/* empty password authorizations for the endorsement hierarchy and AK */
	tpm_buf_append_auth(chip, buf, NULL, 0);
	tpm_buf_append_auth(chip, buf, NULL, 0);

	/* qualifyingData */
	tpm_buf_append_u16(buf, nonce_len);
	if (nonce_len)
		tpm_buf_append(buf, nonce, nonce_len);

	/* inScheme: use the AK's scheme */
	tpm_buf_append_u16(buf, TPM_ALG_NULL);

	if (buf->flags & TPM_BUF_INVALID) {
		rc = -EINVAL;
		goto out;
	}

	rc = tpm_transmit_cmd(chip, buf, 0, "getting session audit digest");
	if (rc)
		goto out;

	rc = -EINVAL;
	tpm2_rsp_init(&r, buf);
	param_size = tpm2_rsp_u32(&r);
	end = r.off + param_size;

	/* auditInfo */
	attest_len = tpm2_rsp_u16(&r);
	attest = tpm2_rsp_bytes(&r, attest_len);
	if (!attest)
		goto out;

	/* TPMS_ATTEST begins with magic and type */
	if (attest_len < sizeof(u32) + sizeof(u16) ||
	    get_unaligned_be32(attest) != TPM2_GENERATED_VALUE ||
	    get_unaligned_be16(attest + sizeof(u32)) !=
	    TPM2_ST_ATTEST_SESSION_AUDIT)
		goto out;

	/* signature */
	if (tpm2_rsp_u16(&r) != TPM_ALG_ECDSA ||
	    tpm2_rsp_u16(&r) != TPM_ALG_SHA256)
		goto out;
	tpm2_rsp_ecc_param(&r, audit->sig_r);
	tpm2_rsp_ecc_param(&r, audit->sig_s);

	if (r.err || r.off != end)
		goto out;

	audit->attest = kmemdup(attest, attest_len, GFP_KERNEL);
	if (!audit->attest) {
		rc = -ENOMEM;
		goto out;
	}
	audit->attest_len = attest_len;
	rc = 0;

out:
	if (rc < 0 && rc != -ENOMEM)
		dev_err(&chip->dev, "failed to get session audit digest: %d\n",
			rc);
	tpm2_flush_context(chip, ak);
	return rc;
}
EXPORT_SYMBOL_GPL(tpm2_get_signed_audit_digest);

/**
 * tpm2_free_signed_audit() - release a signed audit digest
 * @audit:	the structure filled by tpm2_get_signed_audit_digest()
 */
void tpm2_free_signed_audit(struct tpm2_signed_audit *audit)
{
	kfree(audit->attest);
	audit->attest = NULL;
	audit->attest_len = 0;
}
EXPORT_SYMBOL_GPL(tpm2_free_signed_audit);
