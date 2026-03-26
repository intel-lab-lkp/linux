// SPDX-License-Identifier: GPL-2.0-only
/*
 * Hornet Linux Security Module
 *
 * Author: Blaise Boscaccy <bboscaccy@linux.microsoft.com>
 *
 * Copyright (C) 2026 Microsoft Corporation
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

/* The only hashing algorithm available is SHA256 due to it be hardcoded
   in the bpf subsystem. */

struct hornet_parse_context {
	int indexes[MAX_USED_MAPS];
	bool skips[MAX_USED_MAPS];
	unsigned char hashes[SHA256_DIGEST_SIZE * MAX_USED_MAPS];
	int hash_count;
};

struct hornet_prog_security_struct {
	bool checked[MAX_USED_MAPS];
	unsigned char hashes[SHA256_DIGEST_SIZE * MAX_USED_MAPS];
};

struct hornet_map_security_struct {
	bool checked;
	int index;
};

struct lsm_blob_sizes hornet_blob_sizes __ro_after_init = {
	.lbs_bpf_map = sizeof(struct hornet_map_security_struct),
	.lbs_bpf_prog = sizeof(struct hornet_prog_security_struct),
};

static inline struct hornet_prog_security_struct *
hornet_bpf_prog_security(struct bpf_prog *prog)
{
	return prog->aux->security + hornet_blob_sizes.lbs_bpf_prog;
}

static inline struct hornet_map_security_struct *
hornet_bpf_map_security(struct bpf_map *map)
{
	return map->security + hornet_blob_sizes.lbs_bpf_map;
}

static int hornet_verify_hashes(struct hornet_maps *maps,
				struct hornet_parse_context *ctx,
				struct bpf_prog *prog)
{
	int map_fd;
	u32 i;
	struct bpf_map *map;
	int err = 0;
	unsigned char hash[SHA256_DIGEST_SIZE];
	struct hornet_prog_security_struct *security = hornet_bpf_prog_security(prog);
	struct hornet_map_security_struct *map_security;

	for (i = 0; i < ctx->hash_count; i++) {
		if (ctx->skips[i])
			continue;

		err = copy_from_bpfptr_offset(&map_fd, maps->fd_array,
					      ctx->indexes[i] * sizeof(map_fd),
					      sizeof(map_fd));
		if (err < 0)
			return LSM_INT_VERDICT_FAULT;

		CLASS(fd, f)(map_fd);
		if (fd_empty(f))
			return LSM_INT_VERDICT_FAULT;
		if (unlikely(fd_file(f)->f_op != &bpf_map_fops))
			return LSM_INT_VERDICT_FAULT;

		map = fd_file(f)->private_data;
		if (!map->frozen)
			return LSM_INT_VERDICT_FAULT;

		map->ops->map_get_hash(map, SHA256_DIGEST_SIZE, hash);

		err = memcmp(hash, &ctx->hashes[i * SHA256_DIGEST_SIZE],
			      SHA256_DIGEST_SIZE);
		if (err)
			return LSM_INT_VERDICT_UNEXPECTED;

		security->checked[i] = true;
		memcpy(&security->hashes[i * SHA256_DIGEST_SIZE], hash, SHA256_DIGEST_SIZE);
		map_security = hornet_bpf_map_security(map);
		map_security->checked = true;
		map_security->index = i;
	}
	return LSM_INT_VERDICT_OK;
}

int hornet_next_map(void *context, size_t hdrlen,
		     unsigned char tag,
		     const void *value, size_t vlen)
{
	struct hornet_parse_context *ctx = (struct hornet_parse_context *)context;

	if (++ctx->hash_count >= MAX_USED_MAPS)
		return -EINVAL;
	return 0;
}

int hornet_map_index(void *context, size_t hdrlen,
		     unsigned char tag,
		     const void *value, size_t vlen)
{
	struct hornet_parse_context *ctx = (struct hornet_parse_context *)context;

	if (vlen > 1)
		return -EINVAL;

	ctx->indexes[ctx->hash_count] = *(u8 *)value;
	return 0;
}

int hornet_map_hash(void *context, size_t hdrlen,
		    unsigned char tag,
		    const void *value, size_t vlen)

{
	struct hornet_parse_context *ctx = (struct hornet_parse_context *)context;

	if (vlen != SHA256_DIGEST_SIZE && vlen != 0)
		return -EINVAL;

	if (vlen) {
		ctx->skips[ctx->hash_count] = false;
		memcpy(&ctx->hashes[ctx->hash_count * SHA256_DIGEST_SIZE], value, vlen);
	} else
		ctx->skips[ctx->hash_count] = true;

	return 0;
}

static int hornet_check_program(struct bpf_prog *prog, union bpf_attr *attr,
				struct bpf_token *token, bool is_kernel,
				enum lsm_integrity_verdict *verdict)
{
	struct hornet_maps maps = {0};
	bpfptr_t usig = make_bpfptr(attr->signature, is_kernel);
	struct pkcs7_message *msg;
	struct hornet_parse_context *ctx;
	void *sig;
	int err;
	const void *authattrs;
	size_t authattrs_len;

	if (!attr->signature) {
		*verdict = LSM_INT_VERDICT_UNSIGNED;
		return 0;
	}

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
		goto cleanup_sig;

	msg = pkcs7_parse_message(sig, attr->signature_size);
	if (IS_ERR(msg)) {
		err = LSM_INT_VERDICT_BADSIG;
		goto cleanup_sig;
	}

	if (verify_pkcs7_message_sig(prog->insnsi, prog->len * sizeof(struct bpf_insn), msg,
				     VERIFY_USE_SECONDARY_KEYRING,
				     VERIFYING_BPF_SIGNATURE,
				     NULL, NULL)) {
		err = LSM_INT_VERDICT_UNKNOWNKEY;
		goto cleanup_msg;
	}

	if (pkcs7_get_authattr(msg, OID_hornet_data,
			       &authattrs, &authattrs_len) == -ENODATA) {
		err = LSM_INT_VERDICT_PARTIALSIG;
		goto cleanup_msg;
	}

	err = asn1_ber_decoder(&hornet_decoder, ctx, authattrs, authattrs_len);
	if (err < 0 || authattrs == NULL) {
		err = LSM_INT_VERDICT_BADSIG;
		goto cleanup_msg;
	}

	err = hornet_verify_hashes(&maps, ctx, prog);

cleanup_msg:
	pkcs7_free_message(msg);
cleanup_sig:
	kfree(sig);
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
	enum lsm_integrity_verdict verdict;
	int result = hornet_check_program(prog, attr, token, is_kernel, &verdict);

	if (result < 0)
		return result;

	return security_bpf_prog_load_post_integrity(prog, attr, token, is_kernel,
						     &hornet_lsmid, verdict);
}

static int hornet_verify_map(struct bpf_prog *prog, int index)
{
	unsigned char hash[SHA256_DIGEST_SIZE];
	int i;
	struct bpf_map *map;
	struct hornet_prog_security_struct *security = hornet_bpf_prog_security(prog);
	struct hornet_map_security_struct *map_security;

	if (!security->checked[index])
		return 0;

	for (i = 0; i < prog->aux->used_map_cnt; i++) {
		map = prog->aux->used_maps[i];
		map_security = hornet_bpf_map_security(map);
		if (map_security->index != index)
			continue;

		if (!map->frozen)
			return -EPERM;

		map->ops->map_get_hash(map, SHA256_DIGEST_SIZE, hash);
		if (memcmp(hash, &security->hashes[index * SHA256_DIGEST_SIZE],
			   SHA256_DIGEST_SIZE) != 0)
			return -EPERM;
		else
			return 0;
	}
	return -EINVAL;
}

static int hornet_check_prog_maps(u32 ufd)
{
	CLASS(fd, f)(ufd);
	struct bpf_prog *prog;
	int i, result = 0;

	if (fd_empty(f))
		return -EBADF;
	if (fd_file(f)->f_op != &bpf_prog_fops)
		return -EINVAL;

	prog = fd_file(f)->private_data;

	mutex_lock(&prog->aux->used_maps_mutex);
	if (!prog->aux->used_map_cnt)
		goto out;

	for (i = 0; i < prog->aux->used_map_cnt; i++) {
		result = hornet_verify_map(prog, i);
		if (result)
			goto out;
	}
out:
	mutex_unlock(&prog->aux->used_maps_mutex);

	return result;
}

static int hornet_bpf(int cmd, union bpf_attr *attr, unsigned int size, bool kernel)
{
	/* in horent_bpf(), anything that had originated from kernel space we assume
	   has already been checked, in some form or another, so we don't bother
	   checking the intergity of any maps. In hornet_bpf_prog_load_integrity(),
	   hornet doesn't make any opinion on that and delegates that to the downstream
	   policy enforcement. */

	if (cmd != BPF_PROG_RUN)
		return 0;
	if (kernel)
		return 0;

	return hornet_check_prog_maps(attr->test.prog_fd);
}

static struct security_hook_list hornet_hooks[] __ro_after_init = {
	LSM_HOOK_INIT(bpf_prog_load_integrity, hornet_bpf_prog_load_integrity),
	LSM_HOOK_INIT(bpf, hornet_bpf),
};

static int __init hornet_init(void)
{
	pr_info("Hornet: eBPF signature verification enabled\n");
	security_add_hooks(hornet_hooks, ARRAY_SIZE(hornet_hooks), &hornet_lsmid);
	return 0;
}

DEFINE_LSM(hornet) = {
	.id = &hornet_lsmid,
	.blobs = &hornet_blob_sizes,
	.init = hornet_init,
};
