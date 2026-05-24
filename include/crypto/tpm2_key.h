/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_TPM2_KEY_H__
#define __LINUX_TPM2_KEY_H__

#include <linux/oid_registry.h>
#include <linux/slab.h>

#define TPM2_KEY_BYTES_MAX 1024

/*  TPM2 Structures 12.2.4: TPMT_PUBLIC */
struct tpm2_key_desc {
	__be16 type;
	__be16 name_alg;
	__be32 object_attributes;
	__be16 policy_size;
} __packed;

/* Decoded TPM2 ASN.1 key. */
struct tpm2_key {
	u8 data[2 * TPM2_KEY_BYTES_MAX];
	struct tpm2_key_desc *desc;
	u16 priv_len;
	u16 pub_len;
	u32 parent;
	enum OID oid;
	bool empty_auth;
};

struct tpm2_key *tpm2_key_decode(const u8 *src, u32 src_len);

static inline const void *tpm2_key_data(const struct tpm2_key *key)
{
	return &key->data[0];
}

static inline u16 tpm2_key_type(const struct tpm2_key *key)
{
	return be16_to_cpu(key->desc->type);
}

static inline int tpm2_key_policy_size(const struct tpm2_key *key)
{
	return be16_to_cpu(key->desc->policy_size);
}

#endif /* __LINUX_TPM2_KEY_H__ */
