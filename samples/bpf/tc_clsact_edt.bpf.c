#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

#define ETH_P_IP	0x0800
#define TC_ACT_OK	0
#define NS_PER_SEC	1000000000ULL

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__type(key, __u32);
	__type(value, __u64);
	__uint(pinning, LIBBPF_PIN_BY_NAME);
	__uint(max_entries, 16);
} rate_map SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 16);
	__type(key, u32);
	__type(value, u64);
	__uint(pinning, LIBBPF_PIN_BY_NAME);
} tstamp_map SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 16);
	__type(key, u32);
	__type(value, u64);
	__uint(pinning, LIBBPF_PIN_BY_NAME);
} comp_map SEC(".maps");

u64 last_ktime = 0;

SEC("classifier")
int prog(struct __sk_buff *skb)
{
	void *data_end = (void *)(unsigned long long)skb->data_end;
	u64 *rate, *tstamp, delay_ns, tstamp_comp, tstamp_new, *comp, comp_ns, now, init_rate = 12500000;    /* 100 Mbits/sec */
	void *data = (void *)(unsigned long long)skb->data;
	struct iphdr *ip = data + sizeof(struct ethhdr);
	struct ethhdr *eth = data;
	u64 len = skb->len;
	long ret;
	u64 zero = 0;

	now = bpf_ktime_get_ns();

	if (data + sizeof(struct ethhdr) > data_end)
		return TC_ACT_OK;
	if (skb->protocol != bpf_htons(ETH_P_IP))
		return TC_ACT_OK;
	if (data + sizeof(struct ethhdr) + sizeof(struct iphdr) > data_end)
		return TC_ACT_OK;

	rate = bpf_map_lookup_elem(&rate_map, &ip->daddr);
	if (!rate) {
		bpf_map_update_elem(&rate_map, &ip->daddr, &init_rate, BPF_ANY);
		bpf_map_update_elem(&tstamp_map, &ip->daddr, &now, BPF_ANY);
		bpf_map_update_elem(&comp_map, &ip->daddr, &zero, BPF_ANY);
		return TC_ACT_OK;
	}

	delay_ns = skb->len * NS_PER_SEC / (*rate);

	tstamp = bpf_map_lookup_elem(&tstamp_map, &ip->daddr);
	if (!tstamp)	/* unlikely */
		return TC_ACT_OK;

	comp = bpf_map_lookup_elem(&comp_map, &ip->daddr);
	if (!comp)	/* unlikely */
		return TC_ACT_OK;

	// Reset comp and tstamp when idle
	if (now - last_ktime > 1000000000) {
		__sync_lock_test_and_set(comp, 0);
		__sync_lock_test_and_set(tstamp, now);
	}
	last_ktime = now;

	comp_ns = __sync_lock_test_and_set(comp, 0);
	tstamp_comp = *tstamp - comp_ns;
	if (tstamp_comp < now) {
		tstamp_new = tstamp_comp + delay_ns;
		if (tstamp_new < now) {
			__sync_fetch_and_add(comp, now - tstamp_new);
			__sync_lock_test_and_set(tstamp, now);
		} else {
			__sync_fetch_and_sub(tstamp, comp_ns);
			__sync_fetch_and_add(tstamp, delay_ns);
		}
		skb->tstamp = now;
		return TC_ACT_OK;
	}

	__sync_fetch_and_sub(tstamp, comp_ns);
	skb->tstamp = *tstamp;
	__sync_fetch_and_add(tstamp, delay_ns);

	return TC_ACT_OK;
}

char _license[] SEC("license") = "GPL";
