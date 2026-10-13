// SPDX-License-Identifier: GPL-2.0
#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>

__u32 ts_id_cb_count;
__u32 ts_id_cb_syn;
__u32 ts_id_cb_bits;
__u32 ts_id_reply;
__u32 ts_id_decline;	/* set by userspace to exercise the decline path */

SEC("sockops")
int tcp_ts_id_sockops(struct bpf_sock_ops *skops)
{
	if (skops->op == BPF_SOCK_OPS_TCP_LISTEN_CB) {
		bpf_sock_ops_cb_flags_set(skops,
					  BPF_SOCK_OPS_TCP_TS_ID_CB_FLAG);
		return 1;
	}

	if (skops->op != BPF_SOCK_OPS_TCP_TS_ID_CB)
		return 1;

	ts_id_cb_count++;
	ts_id_cb_syn = !!(skops->skb_tcp_flags & (1U << 1));
	ts_id_cb_bits = skops->args[1];
	/* -1 does not fit in Arg2 bits, so the kernel assigns no ID. */
	skops->reply = ts_id_decline ? -1 : ts_id_reply;

	return 1;
}

char _license[] SEC("license") = "GPL";
