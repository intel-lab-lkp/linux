// SPDX-License-Identifier: GPL-2.0

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include "bpf_misc.h"

__u64 flags;

SEC("tc")
__description("skb_ext slice is invalidated by bpf_clone_redirect")
__failure
int skb_ext_stale_slice_after_clone_redirect(struct __sk_buff *ctx)
{
	struct bpf_dynptr meta;
	__u8 *slice;

	if (bpf_dynptr_from_skb_ext(ctx, 0, BPF_SKB_EXT_F_CREATE, &meta))
		return 0;

	slice = bpf_dynptr_slice_rdwr(&meta, 0, NULL, 8);
	if (!slice)
		return 0;

	bpf_clone_redirect(ctx, 1, 0);

	/* Stale: the clone shares the ext block the slice points into. */
	*slice = 0;

	return 0;
}

SEC("tc")
__description("skb_ext slice is invalidated when ext is re-opened with F_CREATE")
__failure
int skb_ext_stale_slice_after_recreate(struct __sk_buff *ctx)
{
	struct bpf_dynptr d1, d2;
	__u8 *slice;

	if (bpf_dynptr_from_skb_ext(ctx, 0, 0, &d1))
		return 0;

	slice = bpf_dynptr_slice(&d1, 0, NULL, 8);
	if (!slice)
		return 0;

	/* May COW the ext block, leaving the slice pointing at the old one. */
	if (bpf_dynptr_from_skb_ext(ctx, 0, BPF_SKB_EXT_F_CREATE, &d2))
		return 0;

	return *slice;
}

SEC("tp_btf/kfree_skb")
__description("F_CREATE is rejected in tracing programs")
__failure __msg("is not allowed in lsm/tracing programs")
int BPF_PROG(tp_skb_ext_create, struct sk_buff *skb)
{
	struct bpf_dynptr meta;

	if (bpf_dynptr_from_skb_ext((struct __sk_buff *)skb, 0,
				    BPF_SKB_EXT_F_CREATE, &meta))
		return 0;

	return 0;
}

SEC("tp_btf/kfree_skb")
__description("non-constant flags are rejected in tracing programs")
__failure __msg("must be a known constant")
int BPF_PROG(tp_skb_ext_var_flags, struct sk_buff *skb)
{
	struct bpf_dynptr meta;

	if (bpf_dynptr_from_skb_ext((struct __sk_buff *)skb, 0, flags, &meta))
		return 0;

	return 0;
}

SEC("tc")
__description("non-constant flags are rejected")
__failure __msg("must be a known constant")
int skb_ext_var_flags(struct __sk_buff *ctx)
{
	struct bpf_dynptr meta;

	if (bpf_dynptr_from_skb_ext(ctx, 0, flags, &meta))
		return 0;

	return 0;
}

SEC("lsm/inet_conn_established")
__description("F_CREATE is rejected in LSM programs")
__failure __msg("is not allowed in lsm/tracing programs")
int BPF_PROG(lsm_skb_ext_create, struct sock *sk, struct sk_buff *skb)
{
	struct bpf_dynptr meta;

	if (bpf_dynptr_from_skb_ext((struct __sk_buff *)skb, 0,
				    BPF_SKB_EXT_F_CREATE, &meta))
		return 0;

	return 0;
}

SEC("lsm/inet_conn_established")
__description("non-constant flags are rejected in LSM programs")
__failure __msg("must be a known constant")
int BPF_PROG(lsm_skb_ext_var_flags, struct sock *sk, struct sk_buff *skb)
{
	struct bpf_dynptr meta;

	if (bpf_dynptr_from_skb_ext((struct __sk_buff *)skb, 0, flags, &meta))
		return 0;

	return 0;
}

char _license[] SEC("license") = "GPL";
