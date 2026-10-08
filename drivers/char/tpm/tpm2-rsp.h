/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Bounds-checked reader for data returned by the TPM.  Reading past the
 * end of the data sets an error flag and returns zeros, so a sequence of
 * reads can be checked once at the end.
 */
#ifndef __TPM2_RSP_H__
#define __TPM2_RSP_H__

#include <linux/tpm.h>
#include <linux/unaligned.h>

struct tpm2_rsp {
	const u8 *data;
	u32 len;
	u32 off;
	bool err;
};

static inline void tpm2_rsp_init_data(struct tpm2_rsp *r, const u8 *data,
				      u32 len)
{
	r->data = data;
	r->len = len;
	r->off = 0;
	r->err = false;
}

/* Initialise a reader over the parameters of the response held in @buf */
static inline void tpm2_rsp_init(struct tpm2_rsp *r, struct tpm_buf *buf)
{
	struct tpm_header *head = (struct tpm_header *)buf->data;

	r->data = buf->data;
	r->len = min_t(u32, be32_to_cpu(head->length), TPM_BUFSIZE);
	r->off = TPM_HEADER_SIZE;
	r->err = r->len < TPM_HEADER_SIZE;
}

static inline const u8 *tpm2_rsp_bytes(struct tpm2_rsp *r, u32 count)
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

static inline u8 tpm2_rsp_u8(struct tpm2_rsp *r)
{
	const u8 *p = tpm2_rsp_bytes(r, sizeof(u8));

	return p ? *p : 0;
}

static inline u16 tpm2_rsp_u16(struct tpm2_rsp *r)
{
	const u8 *p = tpm2_rsp_bytes(r, sizeof(u16));

	return p ? get_unaligned_be16(p) : 0;
}

static inline u32 tpm2_rsp_u32(struct tpm2_rsp *r)
{
	const u8 *p = tpm2_rsp_bytes(r, sizeof(u32));

	return p ? get_unaligned_be32(p) : 0;
}

/* Read a TPM2B, returning its contents and setting @len */
static inline const u8 *tpm2_rsp_tpm2b(struct tpm2_rsp *r, u16 *len)
{
	*len = tpm2_rsp_u16(r);
	return tpm2_rsp_bytes(r, *len);
}

/* Read a TPM2B_ECC_PARAMETER, left-padding it to EC_PT_SZ bytes */
static inline void tpm2_rsp_ecc_param(struct tpm2_rsp *r, u8 *out)
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

/*
 * Parse a TPM2B_PUBLIC for an ECDSA-SHA256 P-256 signing key with no
 * symmetric algorithm, KDF or policy, checking that it has exactly the
 * object attributes @attrs, and return its public point.
 */
static inline int tpm2_rsp_ecdsa_public(struct tpm2_rsp *r, u32 attrs,
					u8 *x, u8 *y)
{
	u16 size = tpm2_rsp_u16(r);
	u32 end = r->off + size;

	if (tpm2_rsp_u16(r) != TPM_ALG_ECC ||
	    tpm2_rsp_u16(r) != TPM_ALG_SHA256 ||
	    tpm2_rsp_u32(r) != attrs ||
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

#endif
