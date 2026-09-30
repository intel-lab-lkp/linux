// SPDX-License-Identifier: GPL-2.0
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include "bpf_kfuncs.h"

/* From include/net/tcp_ecn.h */
#define TCP_ECN_IN_ACCECN_OUT_ACCECN	3
#define TCP_ECN_IN_ACCECN_OUT_ECN	4
#define TCP_ECN_IN_ACCECN_OUT_NOECN	5
#define TCP_ECN_MODE_UNSPEC		255
#define TCP_ACCECN_OPTION_MINIMUM	1
#define TCP_ACCECN_OPTION_FULL		2
#define TCP_ACCECN_OPTION_PERSIST	3
#define TCP_ACCECN_OPTION_UNSPEC	255

extern int bpf_sock_ops_set_ecn_mode(struct bpf_sock_ops_kern *skops,
				     u32 mode) __ksym;
extern int bpf_sock_ops_set_accecn_option(struct bpf_sock_ops_kern *skops,
					  u32 opt) __ksym;

int connect_bad_mode_ret;
int connect_bad_opt_ret;
int connect_set_mode_ret;
int connect_mode_read;
int connect_set_opt_ret;
int connect_opt_read;
int connect_unspec_mode_ret;
int connect_unspec_mode_read;
int estab_set_mode_ret;
int estab_set_opt_ret;
int estab_opt_read;
int listen_set_mode_ret;
int listen_set_opt_ret;
int passive_mode_read;
int passive_opt_read;

SEC("sockops")
int skops_ecn(struct bpf_sock_ops *skops)
{
	struct bpf_sock_ops_kern *skops_kern;
	struct bpf_sock *sk = skops->sk;
	struct tcp_sock *tp;

	if (!sk)
		return 1;

	tp = bpf_skc_to_tcp_sock(sk);
	if (!tp)
		return 1;

	skops_kern = bpf_cast_to_kern_ctx(skops);

	switch (skops->op) {
	case BPF_SOCK_OPS_TCP_CONNECT_CB:
		connect_bad_mode_ret =
			bpf_sock_ops_set_ecn_mode(skops_kern,
						  TCP_ECN_IN_ACCECN_OUT_NOECN + 1);
		connect_bad_opt_ret =
			bpf_sock_ops_set_accecn_option(skops_kern,
						       TCP_ACCECN_OPTION_PERSIST + 1);

		connect_set_mode_ret =
			bpf_sock_ops_set_ecn_mode(skops_kern,
						  TCP_ECN_IN_ACCECN_OUT_ACCECN);
		connect_mode_read = tp->ecn_mode;

		connect_set_opt_ret =
			bpf_sock_ops_set_accecn_option(skops_kern,
						       TCP_ACCECN_OPTION_FULL);
		connect_opt_read = tp->ecn_option;

		/* Leave the socket on the sysctl defaults so the loopback
		 * handshake driven by the test is not altered.
		 */
		connect_unspec_mode_ret =
			bpf_sock_ops_set_ecn_mode(skops_kern, TCP_ECN_MODE_UNSPEC);
		connect_unspec_mode_read = tp->ecn_mode;
		bpf_sock_ops_set_accecn_option(skops_kern,
					       TCP_ACCECN_OPTION_UNSPEC);
		break;
	case BPF_SOCK_OPS_ACTIVE_ESTABLISHED_CB:
		/* The mode is handshake-only; the option is not. */
		estab_set_mode_ret =
			bpf_sock_ops_set_ecn_mode(skops_kern,
						  TCP_ECN_IN_ACCECN_OUT_ACCECN);
		estab_set_opt_ret =
			bpf_sock_ops_set_accecn_option(skops_kern,
						       TCP_ACCECN_OPTION_MINIMUM);
		estab_opt_read = tp->ecn_option;
		break;
	case BPF_SOCK_OPS_TCP_LISTEN_CB:
		listen_set_mode_ret =
			bpf_sock_ops_set_ecn_mode(skops_kern,
						  TCP_ECN_IN_ACCECN_OUT_ECN);
		listen_set_opt_ret =
			bpf_sock_ops_set_accecn_option(skops_kern,
						       TCP_ACCECN_OPTION_PERSIST);
		break;
	case BPF_SOCK_OPS_PASSIVE_ESTABLISHED_CB:
		/* Listener values consulted for SYN negotiation; child starts
		 * with UNSPEC (not inherited).
		 */
		passive_mode_read = tp->ecn_mode;
		passive_opt_read = tp->ecn_option;
		break;
	}

	return 1;
}

char _license[] SEC("license") = "GPL";
