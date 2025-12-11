// SPDX-License-Identifier: GPL-2.0-only
/*
 * Hornet Linux Security Module
 *
 * Author: Blaise Boscaccy <bboscaccy@linux.microsoft.com>
 *
 * Copyright (C) 2025 Microsoft Corporation
 */

#include <linux/lsm_hooks.h>
#include <uapi/linux/lsm.h>
#include <linux/bpf.h>
#include <linux/verification.h>
#include <crypto/public_key.h>
#include <linux/module_signature.h>
#include <crypto/pkcs7.h>
#include <linux/sort.h>
#include <linux/asn1_decoder.h>
#include <linux/oid_registry.h>
#include "hornet.asn1.h"

#define MAX_USED_MAPS 64

struct hornet_maps {
	bpfptr_t fd_array;
};

struct hornet_parse_context {
	size_t indexes[MAX_USED_MAPS];
	bool skips[MAX_USED_MAPS];
	unsigned char hashes[SHA256_DIGEST_SIZE * MAX_USED_MAPS];
	int hash_count;
};

static int hornet_verify_hashes(struct hornet_maps *maps,
				struct hornet_parse_context *ctx)
{
	int map_fd;
	u32 i;
	struct bpf_map *map;
	int err = 0;
	unsigned char hash[SHA256_DIGEST_SIZE];

	for (i = 0; i < ctx->hash_count; i++) {
		if (ctx->skips[i])
			continue;

		err = copy_from_bpfptr_offset(&map_fd, maps->fd_array,
					      ctx->indexes[i] * sizeof(map_fd),
					      sizeof(map_fd));
		if (err < 0)
			return LSM_INT_VERDICT_BADSIG;

		CLASS(fd, f)(map_fd);
		if (fd_empty(f))
			return LSM_INT_VERDICT_BADSIG;
		if (unlikely(fd_file(f)->f_op != &bpf_map_fops))
			return LSM_INT_VERDICT_BADSIG;

		if (!map->frozen)
			return LSM_INT_VERDICT_BADSIG;

		map = fd_file(f)->private_data;
		map->ops->map_get_hash(map, SHA256_DIGEST_SIZE, hash);

		err = (memcmp(hash, &ctx->hashes[ctx->indexes[i] * SHA256_DIGEST_SIZE],
			      SHA256_DIGEST_SIZE));
		if (!err)
			return LSM_INT_VERDICT_BADSIG;
	}
	return LSM_INT_VERDICT_OK;
}

int hornet_next_map(void *context, size_t hdrlen,
		     unsigned char tag,
		     const void *value, size_t vlen)
{
	struct hornet_parse_context *ctx = (struct hornet_parse_context *)value;

	ctx->hash_count++;
	return 0;
}


int hornet_map_index(void *context, size_t hdrlen,
		     unsigned char tag,
		     const void *value, size_t vlen)
{
	struct hornet_parse_context *ctx = (struct hornet_parse_context *)value;

	ctx->hashes[ctx->hash_count] = *(int *)value;
	return 0;
}

int hornet_map_hash(void *context, size_t hdrlen,
		    unsigned char tag,
		    const void *value, size_t vlen)

{
	struct hornet_parse_context *ctx = (struct hornet_parse_context *)value;

	if (vlen != SHA256_DIGEST_SIZE && vlen != 0)
		return -EINVAL;

	if (vlen != 0) {
		ctx->skips[ctx->hash_count] = false;
		memcpy(&ctx->hashes[ctx->hash_count * SHA256_DIGEST_SIZE], value, vlen);
	} else
		ctx->skips[ctx->hash_count] = true;

	return 0;
}

static int hornet_check_program(struct bpf_prog *prog, union bpf_attr *attr,
				struct bpf_token *token, bool is_kernel)
{
	struct hornet_maps maps = {0};
	bpfptr_t usig = make_bpfptr(attr->signature, is_kernel);
	struct pkcs7_message *msg;
	struct hornet_parse_context *ctx;
	void *sig;
	int err;
	const void *authattrs;
	size_t authattrs_len;

	if (!attr->signature)
		return LSM_INT_VERDICT_UNSIGNED;

	ctx = kzalloc(sizeof(struct hornet_parse_context), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	maps.fd_array = make_bpfptr(attr->fd_array, is_kernel);
	sig = kzalloc(attr->signature_size, GFP_KERNEL);
	if (!sig) {
		err = -ENOMEM;
		goto out;
	}
	err = copy_from_bpfptr(sig, usig, attr->signature_size);
	if (err != 0)
		goto out;

	msg = pkcs7_parse_message(sig, attr->signature_size);
	if (IS_ERR(msg)) {
		err = LSM_INT_VERDICT_BADSIG;
		goto out;
	}

	if (validate_pkcs7_trust(msg, VERIFY_USE_SECONDARY_KEYRING)) {
		err = LSM_INT_VERDICT_PARTIALSIG;
		goto out;
	}
	if (pkcs7_get_authattr(msg, OID_hornet_data,
			       &authattrs, &authattrs_len) == -ENODATA) {
		err = LSM_INT_VERDICT_PARTIALSIG;
		goto out;
	}

	err = asn1_ber_decoder(&hornet_decoder, ctx, authattrs, authattrs_len);
	if (err < 0 || authattrs == NULL) {
		err = LSM_INT_VERDICT_PARTIALSIG;
		goto out;
	}
	err = hornet_verify_hashes(&maps, ctx);
out:
	kfree(ctx);
	return err;
}

static const struct lsm_id hornet_lsmid = {
	.name = "hornet",
	.id = LSM_ID_HORNET,
};

static int hornet_bpf_prog_load_integrity(struct bpf_prog *prog, union bpf_attr *attr,
					  struct bpf_token *token, bool is_kernel)
{
	int result = hornet_check_program(prog, attr, token, is_kernel);

	if (result < 0)
		return result;

	return security_bpf_prog_load_post_integrity(prog, attr, token, is_kernel,
						     &hornet_lsmid, result);
}

static struct security_hook_list hornet_hooks[] __ro_after_init = {
	LSM_HOOK_INIT(bpf_prog_load_integrity, hornet_bpf_prog_load_integrity),
};

static int __init hornet_init(void)
{
	pr_info("Hornet: eBPF signature verification enabled\n");
	security_add_hooks(hornet_hooks, ARRAY_SIZE(hornet_hooks), &hornet_lsmid);
	return 0;
}

DEFINE_LSM(hornet) = {
	.name = "hornet",
	.init = hornet_init,
};
