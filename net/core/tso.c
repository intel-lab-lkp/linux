// SPDX-License-Identifier: GPL-2.0
#include <linux/export.h>
#include <linux/if_vlan.h>
#include <net/ip.h>
#include <net/tso.h>
#include <linux/dma-mapping.h>
#include <linux/unaligned.h>

void tso_build_hdr(const struct sk_buff *skb, char *hdr, struct tso_t *tso,
		   int size, bool is_last)
{
	int hdr_len = skb_transport_offset(skb) + tso->tlen;
	int mac_hdr_len = skb_network_offset(skb);

	memcpy(hdr, skb->data, hdr_len);
	if (!tso->ipv6) {
		struct iphdr *iph = (void *)(hdr + mac_hdr_len);

		iph->id = htons(tso->ip_id);
		iph->tot_len = htons(size + hdr_len - mac_hdr_len);
		tso->ip_id++;
	} else {
		struct ipv6hdr *iph = (void *)(hdr + mac_hdr_len);

		iph->payload_len = htons(size + tso->tlen);
	}
	hdr += skb_transport_offset(skb);
	if (tso->tlen != sizeof(struct udphdr)) {
		struct tcphdr *tcph = (struct tcphdr *)hdr;

		put_unaligned_be32(tso->tcp_seq, &tcph->seq);

		if (!is_last) {
			/* Clear all special flags for not last packet */
			tcph->psh = 0;
			tcph->fin = 0;
			tcph->rst = 0;
		}
	} else {
		struct udphdr *uh = (struct udphdr *)hdr;

		uh->len = htons(sizeof(*uh) + size);
	}
}
EXPORT_SYMBOL(tso_build_hdr);

void tso_build_data(const struct sk_buff *skb, struct tso_t *tso, int size)
{
	tso->tcp_seq += size; /* not worth avoiding this operation for UDP */
	tso->size -= size;
	tso->data += size;

	if ((tso->size == 0) &&
	    (tso->next_frag_idx < skb_shinfo(skb)->nr_frags)) {
		skb_frag_t *frag = &skb_shinfo(skb)->frags[tso->next_frag_idx];

		/* Move to next segment */
		tso->size = skb_frag_size(frag);
		tso->data = skb_frag_address(frag);
		tso->next_frag_idx++;
	}
}
EXPORT_SYMBOL(tso_build_data);

int tso_start(struct sk_buff *skb, struct tso_t *tso)
{
	int tlen = skb_is_gso_tcp(skb) ? tcp_hdrlen(skb) : sizeof(struct udphdr);
	int hdr_len = skb_transport_offset(skb) + tlen;

	tso->tlen = tlen;
	tso->ip_id = ntohs(ip_hdr(skb)->id);
	tso->tcp_seq = (tlen != sizeof(struct udphdr)) ? ntohl(tcp_hdr(skb)->seq) : 0;
	tso->next_frag_idx = 0;
	tso->ipv6 = vlan_get_protocol(skb) == htons(ETH_P_IPV6);

	/* Build first data */
	tso->size = skb_headlen(skb) - hdr_len;
	tso->data = skb->data + hdr_len;
	if ((tso->size == 0) &&
	    (tso->next_frag_idx < skb_shinfo(skb)->nr_frags)) {
		skb_frag_t *frag = &skb_shinfo(skb)->frags[tso->next_frag_idx];

		/* Move to next segment */
		tso->size = skb_frag_size(frag);
		tso->data = skb_frag_address(frag);
		tso->next_frag_idx++;
	}
	return hdr_len;
}
EXPORT_SYMBOL(tso_start);

/**
 * tso_dma_map_init - DMA-map GSO payload regions
 * @map: map struct to initialize
 * @dev: device for DMA mapping
 * @skb: the GSO skb
 * @hdr_len: per-segment header length in bytes
 *
 * DMA-maps the linear payload (after headers) and all frags.
 * Positions the iterator at byte 0 of the payload.
 *
 * Returns 0 on success, -ENOMEM on DMA mapping failure (partial mappings
 * are cleaned up internally).
 */
int tso_dma_map_init(struct tso_dma_map *map, struct device *dev,
		     const struct sk_buff *skb, unsigned int hdr_len)
{
	unsigned int linear_len = skb_headlen(skb) - hdr_len;
	unsigned int nr_frags = skb_shinfo(skb)->nr_frags;
	int i;

	map->dev = dev;
	map->skb = skb;
	map->hdr_len = hdr_len;
	map->frag_idx = -1;
	map->offset = 0;
	map->linear_len = 0;
	map->nr_frags = 0;

	if (linear_len > 0) {
		map->linear_dma = dma_map_single(dev, skb->data + hdr_len,
						 linear_len, DMA_TO_DEVICE);
		if (dma_mapping_error(dev, map->linear_dma))
			return -ENOMEM;
		map->linear_len = linear_len;
	}

	for (i = 0; i < nr_frags; i++) {
		skb_frag_t *frag = &skb_shinfo(skb)->frags[i];

		map->frags[i].len = skb_frag_size(frag);
		map->frags[i].dma = skb_frag_dma_map(dev, frag, 0,
						     map->frags[i].len,
						     DMA_TO_DEVICE);
		if (dma_mapping_error(dev, map->frags[i].dma)) {
			tso_dma_map_cleanup(map);
			return -ENOMEM;
		}
		map->nr_frags = i + 1;
	}

	if (linear_len == 0 && nr_frags > 0)
		map->frag_idx = 0;

	return 0;
}
EXPORT_SYMBOL(tso_dma_map_init);

/**
 * tso_dma_map_cleanup - unmap all DMA regions in a tso_dma_map
 * @map: the map to clean up
 *
 * Unmaps linear payload and all mapped frags. Used on error paths.
 * Success paths use the driver's completion path to handle unmapping.
 */
void tso_dma_map_cleanup(struct tso_dma_map *map)
{
	int i;

	if (map->linear_len)
		dma_unmap_single(map->dev, map->linear_dma, map->linear_len,
				 DMA_TO_DEVICE);

	for (i = 0; i < map->nr_frags; i++)
		dma_unmap_page(map->dev, map->frags[i].dma, map->frags[i].len,
			       DMA_TO_DEVICE);

	map->linear_len = 0;
	map->nr_frags = 0;
}
EXPORT_SYMBOL(tso_dma_map_cleanup);

/**
 * tso_dma_map_count - count descriptors for a payload range
 * @map: the payload map
 * @len: number of payload bytes in this segment
 *
 * Counts how many contiguous DMA region chunks the next @len bytes
 * will span, without advancing the iterator. Uses region sizes from
 * the current position.
 *
 * Returns the number of descriptors needed for @len bytes of payload.
 */
unsigned int tso_dma_map_count(const struct tso_dma_map *map, unsigned int len)
{
	unsigned int offset = map->offset;
	int idx = map->frag_idx;
	unsigned int count = 0;

	while (len > 0) {
		unsigned int region_len, chunk;

		if (idx == -1)
			region_len = map->linear_len;
		else
			region_len = map->frags[idx].len;

		chunk = min(len, region_len - offset);
		len -= chunk;
		count++;
		offset = 0;
		idx++;
	}

	return count;
}
EXPORT_SYMBOL(tso_dma_map_count);

/**
 * tso_dma_map_next - yield the next DMA address range
 * @map: the payload map
 * @addr: output DMA address
 * @chunk_len: output chunk length
 * @mapping_len: full DMA mapping length when this chunk starts a new
 *               mapping region, or 0 when continuing a previous one.
 *               Driver can assign this to the last descriptor.
 * @seg_remaining: bytes left in current segment
 *
 * Yields the next (dma_addr, chunk_len) pair and advances the iterator.
 *
 * Returns true if a chunk was yielded, false when @seg_remaining is 0.
 */
bool tso_dma_map_next(struct tso_dma_map *map, dma_addr_t *addr,
		      unsigned int *chunk_len, unsigned int *mapping_len,
		      unsigned int seg_remaining)
{
	unsigned int region_len, chunk;

	if (!seg_remaining)
		return false;

	if (map->frag_idx == -1) {
		region_len = map->linear_len;
		chunk = min(seg_remaining, region_len - map->offset);
		*addr = map->linear_dma + map->offset;
		*mapping_len = (map->offset == 0) ? region_len : 0;
	} else {
		region_len = map->frags[map->frag_idx].len;
		chunk = min(seg_remaining, region_len - map->offset);
		*addr = map->frags[map->frag_idx].dma + map->offset;
		*mapping_len = (map->offset == 0) ? region_len : 0;
	}

	*chunk_len = chunk;
	map->offset += chunk;

	if (map->offset >= region_len) {
		map->frag_idx++;
		map->offset = 0;
	}

	return true;
}
EXPORT_SYMBOL(tso_dma_map_next);
