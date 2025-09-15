// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * algif_kpp: User-space interface for KPP algorithms
 *
 * This file provides the user-space API for Key-agreement Protocol Primitive
 * (KPP).
 *
 * Copyright (C) 2025 Rodolfo Giometti <giometti@enneenne.com>
 */

#include <crypto/dh.h>
#include <crypto/internal/kpp.h>
#include <crypto/scatterwalk.h>
#include <crypto/if_alg.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/net.h>
#include <net/sock.h>
#include <crypto/ecdh.h>

struct alg_kpp_ctx {
	struct crypto_kpp *tfm;
	void *peer_pubkey;
	size_t peer_pubkey_len;
};

static int kpp_sendmsg(struct socket *sock, struct msghdr *msg, size_t ignored)
{
	struct sock *sk = sock->sk;
	struct alg_sock *ask = alg_sk(sk);
	struct alg_kpp_ctx *ctx = ask->private;
	size_t pubkey_len, size;
	int ret;

	/* Check the user data for proper length */
	pubkey_len = crypto_kpp_maxsize(ctx->tfm);
	size = iov_iter_count(&msg->msg_iter);
	if (pubkey_len == 0 || pubkey_len != size)
		return -EINVAL;

	/* Read the peer public key */
	if (ctx->peer_pubkey_len > 0)
		kfree(ctx->peer_pubkey);
	ctx->peer_pubkey = kmalloc(pubkey_len, GFP_KERNEL);
	if (!ctx->peer_pubkey)
		return -ENOMEM;
	ctx->peer_pubkey_len = pubkey_len;
	ret = copy_from_iter(ctx->peer_pubkey, pubkey_len, &msg->msg_iter);
	if (ret < 0) {
		kfree(ctx->peer_pubkey);
		ctx->peer_pubkey_len = 0;
	}

	return ret;
}

static int kpp_recvmsg(struct socket *sock, struct msghdr *msg,
		       size_t ignored, int flags)
{
	struct sock *sk = sock->sk;
	struct alg_sock *ask = alg_sk(sk);
	struct alg_kpp_ctx *ctx = ask->private;
	struct kpp_request *req;
	size_t pubkey_len, size;
	struct scatterlist sg_in, sg_out;
	uint8_t *buf;
	int ret;

	pubkey_len = crypto_kpp_maxsize(ctx->tfm);
	if (pubkey_len == 0)
		return -EINVAL;

	/* Check for the user buffer proper length */
	size = iov_iter_count(&msg->msg_iter);
	if (size < pubkey_len)
		return -EINVAL;

	buf = kmalloc(pubkey_len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	/* Allocate request buffer */
	req = kpp_request_alloc(ctx->tfm, GFP_KERNEL);
	if (IS_ERR(req)) {
		ret = PTR_ERR(req);
		goto free_buf;
	}

	/* Generate our public key */
	sg_init_one(&sg_out, buf, pubkey_len);
	kpp_request_set_input(req, NULL, 0);
	kpp_request_set_output(req, &sg_out, pubkey_len);
	ret = crypto_kpp_generate_public_key(req);
	if (ret)
		goto free_req;

	/* Compute the shared secret */
	sg_init_one(&sg_in, ctx->peer_pubkey, ctx->peer_pubkey_len);
	sg_init_one(&sg_out, buf, pubkey_len);
	kpp_request_set_input(req, &sg_in, ctx->peer_pubkey_len);
	kpp_request_set_output(req, &sg_out, pubkey_len);
	ret = crypto_kpp_compute_shared_secret(req);
	if (ret)
		goto free_req;

	/* Drop the current peer's key */
	kfree(ctx->peer_pubkey);
	ctx->peer_pubkey_len = 0;

	/* Return the shared secret to user space */
	ret = copy_to_iter(buf, pubkey_len, &msg->msg_iter);

free_req:
	kpp_request_free(req);
free_buf:
	kfree(buf);

	return ret;
}

static struct proto_ops algif_kpp_ops = {
	.family = PF_ALG,
	.release = af_alg_release,
	.sendmsg = kpp_sendmsg,
	.recvmsg = kpp_recvmsg,
};

static int kpp_check_key(struct socket *sock)
{
	int err = 0;
	struct sock *psk;
	struct alg_sock *pask;
	struct crypto_kpp *tfm;
	struct sock *sk = sock->sk;
	struct alg_sock *ask = alg_sk(sk);

	lock_sock(sk);
	if (!atomic_read(&ask->nokey_refcnt))
		goto unlock_child;

	psk = ask->parent;
	pask = alg_sk(psk);
	tfm = pask->private;

	err = -ENOKEY;
	lock_sock_nested(psk, SINGLE_DEPTH_NESTING);
	if (crypto_kpp_get_flags(tfm) & CRYPTO_TFM_NEED_KEY)
		goto unlock;

	atomic_dec(&pask->nokey_refcnt);
	atomic_set(&ask->nokey_refcnt, 0);

	err = 0;

unlock:
	release_sock(psk);
unlock_child:
	release_sock(sk);

	return err;
}

static int kpp_sendmsg_nokey(struct socket *sock, struct msghdr *msg,
			     size_t size)
{
	int err;

	err = kpp_check_key(sock);
	if (err)
		return err;

	return kpp_sendmsg(sock, msg, size);
}

static int kpp_recvmsg_nokey(struct socket *sock, struct msghdr *msg,
			     size_t ignored, int flags)
{
	int err;

	err = kpp_check_key(sock);
	if (err)
		return err;

	return kpp_recvmsg(sock, msg, ignored, flags);
}

static struct proto_ops algif_kpp_ops_nokey = {
	.family = PF_ALG,
	.release = af_alg_release,
	.sendmsg = kpp_sendmsg_nokey,
	.recvmsg = kpp_recvmsg_nokey,
};

static void *kpp_bind(const char *name, u32 type, u32 mask)
{
	return crypto_alloc_kpp(name, type, mask);
}

static void kpp_release(void *private)
{
	crypto_free_kpp(private);
}

static int kpp_set_secret(void *private, const u8 *key, unsigned int keylen)
{
	return crypto_kpp_set_secret_raw(private, key, keylen);
}

static void kpp_sock_destruct_child(struct sock *sk)
{
	struct alg_sock *ask = alg_sk(sk);
	struct alg_kpp_ctx *ctx = ask->private;

	if (ctx) {
		if (ctx->peer_pubkey_len > 0)
			kfree(ctx->peer_pubkey);
		sock_kfree_s(sk, ctx, sizeof(*ctx));
	}

	af_alg_release_parent(sk);
}

static int kpp_accept_parent_nokey(void *private, struct sock *sk)
{
	struct alg_kpp_ctx *ctx;
	struct alg_sock *ask = alg_sk(sk);
	struct crypto_kpp *tfm = private;

	ctx = sock_kmalloc(sk, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->tfm = tfm;
	ctx->peer_pubkey = NULL;
	ctx->peer_pubkey_len = 0;
	ask->private = ctx;

	sk->sk_destruct = kpp_sock_destruct_child;

	return 0;
}

static int kpp_accept_parent(void *private, struct sock *sk)
{
	struct crypto_kpp *tfm = private;

	if (crypto_kpp_get_flags(tfm) & CRYPTO_TFM_NEED_KEY)
		return -ENOKEY;

	return kpp_accept_parent_nokey(private, sk);
}

static const struct af_alg_type algif_type_kpp = {
	.bind = kpp_bind,
	.release = kpp_release,
	.setkey = kpp_set_secret,
	.accept = kpp_accept_parent,
	.accept_nokey = kpp_accept_parent_nokey,

	.ops = &algif_kpp_ops,
	.ops_nokey = &algif_kpp_ops_nokey,

	.name = "kpp",
	.owner = THIS_MODULE
};

static int __init algif_kpp_init(void)
{
	return af_alg_register_type(&algif_type_kpp);
}

static void __exit algif_kpp_exit(void)
{
	int err = af_alg_unregister_type(&algif_type_kpp);

	WARN_ON_ONCE(err);
}

module_init(algif_kpp_init);
module_exit(algif_kpp_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Rodolfo Giometti <giometti@enneenne.com>");
MODULE_DESCRIPTION("KPP kernel crypto API user space interface");
