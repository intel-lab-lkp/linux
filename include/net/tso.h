/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _TSO_H
#define _TSO_H

#include <linux/skbuff.h>
#include <net/ip.h>

#define TSO_HEADER_SIZE		256

struct tso_t {
	int	next_frag_idx;
	int	size;
	void	*data;
	u16	ip_id;
	u8	tlen; /* transport header len */
	bool	ipv6;
	u32	tcp_seq;
};

/* Calculate the worst case buffer count */
static inline int tso_count_descs(const struct sk_buff *skb)
{
	return skb_shinfo(skb)->gso_segs * 2 + skb_shinfo(skb)->nr_frags;
}

void tso_build_hdr(const struct sk_buff *skb, char *hdr, struct tso_t *tso,
		   int size, bool is_last);
void tso_build_data(const struct sk_buff *skb, struct tso_t *tso, int size);
int tso_start(struct sk_buff *skb, struct tso_t *tso);

static inline int tso_compute_hdr_len(const struct sk_buff *skb)
{
	int tlen = skb_is_gso_tcp(skb) ? tcp_hdrlen(skb) : sizeof(struct udphdr);
	int hdr_len = skb_transport_offset(skb) + tlen;

	return hdr_len;
}

netdev_features_t tso_features_check(struct sk_buff *skb,
				     struct net_device *dev,
				     netdev_features_t features);
#endif	/* _TSO_H */
