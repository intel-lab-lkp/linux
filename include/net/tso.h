/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _TSO_H
#define _TSO_H

#include <linux/skbuff.h>
#include <linux/dma-mapping.h>
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

/**
 * struct tso_dma_map - DMA mapping state for GSO payload
 * @dev: device used for DMA mapping
 * @skb: the GSO skb being mapped
 * @hdr_len: per-segment header length
 * @iova_state: DMA IOVA state (when IOMMU available)
 * @iova_offset: global byte offset into IOVA range (IOVA path only)
 * @total_len: total payload length
 * @frag_idx: current region (-1 = linear, 0..nr_frags-1 = frag)
 * @offset: byte offset within current region
 * @linear_dma: DMA address of the linear payload
 * @linear_len: length of the linear payload
 * @nr_frags: number of frags successfully DMA-mapped
 * @frags: per-frag DMA address and length
 *
 * DMA-maps the payload regions of a GSO skb (linear data + frags).
 * Prefers the DMA IOVA API for a single contiguous mapping with one
 * IOTLB sync; falls back to per-region dma_map_phys() otherwise.
 */
struct tso_dma_map {
	struct device		*dev;
	const struct sk_buff	*skb;
	unsigned int		hdr_len;
	/* IOVA path */
	struct dma_iova_state	iova_state;
	size_t			iova_offset;
	size_t			total_len;
	/* Fallback path if IOVA path fails */
	int			frag_idx;
	unsigned int		offset;
	dma_addr_t		linear_dma;
	unsigned int		linear_len;
	unsigned int		nr_frags;
	struct {
		dma_addr_t	dma;
		unsigned int	len;
	} frags[MAX_SKB_FRAGS];
};

#endif	/* _TSO_H */
