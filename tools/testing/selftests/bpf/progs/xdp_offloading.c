// SPDX-License-Identifier: GPL-2.0
#include <vmlinux.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>

#define IFNAMSIZ 16

/* using a special ifname to filter unrelated traffic */
const __u8 target_ifname[IFNAMSIZ];

/* test outputs: these counters should be 0 to pass tests */
int64_t invalid_skb = 0;

extern int bpf_xdp_disable_gro(struct xdp_md *xdp) __ksym;

/*
 * Observing: after XDP disables GRO, gro_disabled bit should be set
 * and gso_size should be 0.
 */
SEC("tp_btf/netif_receive_skb")
int BPF_PROG(observe_skb_gro_disabled, struct sk_buff *skb)
{
	struct skb_shared_info *shinfo =
		(struct skb_shared_info *)(skb->head + skb->end);
	char devname[IFNAMSIZ];
	int gso_size;

	__builtin_memcpy(devname, skb->dev->name, IFNAMSIZ);
	if (bpf_strncmp(devname, IFNAMSIZ, (const char *)target_ifname))
		return 0;

	if (!skb->gro_disabled)
		__sync_fetch_and_add(&invalid_skb, 1);

	gso_size = BPF_CORE_READ(shinfo, gso_size);
	if (gso_size)
		__sync_fetch_and_add(&invalid_skb, 1);

	return 0;
}

SEC("xdp")
int xdp_disable_gro(struct xdp_md *xdp)
{
	bpf_xdp_disable_gro(xdp);
	return XDP_PASS;
}

char _license[] SEC("license") = "GPL";
