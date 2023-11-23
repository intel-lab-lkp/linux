// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 *  rtase is the Linux device driver released for Realtek Automotive Switch
 *  controllers with PCI-Express interface.
 *
 *  Copyright(c) 2023 Realtek Semiconductor Corp.
 *
 *  Below is a simplified block diagram of the chip and its relevant interfaces.
 *
 *               *************************
 *               *                       *
 *               *  CPU network device   *
 *               *                       *
 *               *   +-------------+     *
 *               *   |  PCIE Host  |     *
 *               ***********++************
 *                          ||
 *                         PCIE
 *                          ||
 *      ********************++**********************
 *      *            | PCIE Endpoint |             *
 *      *            +---------------+             *
 *      *                | GMAC |                  *
 *      *                +--++--+  Realtek         *
 *      *                   ||     RTL90xx Series  *
 *      *                   ||                     *
 *      *     +-------------++----------------+    *
 *      *     |           | MAC |             |    *
 *      *     |           +-----+             |    *
 *      *     |                               |    *
 *      *     |     Ethernet Switch Core      |    *
 *      *     |                               |    *
 *      *     |   +-----+           +-----+   |    *
 *      *     |   | MAC |...........| MAC |   |    *
 *      *     +---+-----+-----------+-----+---+    *
 *      *         | PHY |...........| PHY |        *
 *      *         +--++-+           +--++-+        *
 *      *************||****************||***********
 *
 *  The block of the Realtek RTL90xx series is our entire chip architecture,
 *  the GMAC is connected to the switch core, and there is no PHY in between.
 *  In addition, this driver is mainly used to control GMAC, but does not
 *  control the switch core, so it is not the same as DSA.
 */

#include <linux/crc32.h>
#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/if_vlan.h>
#include <linux/in.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/mdio.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/netdevice.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <linux/prefetch.h>
#include <linux/rtnetlink.h>
#include <linux/tcp.h>
#include <asm/irq.h>
#include <net/ip6_checksum.h>
#include <net/page_pool/helpers.h>
#include <net/pkt_cls.h>

#include "rtase.h"

#define RTK_OPTS1_DEBUG_VALUE 0x0BADBEEF
#define RTK_MAGIC_NUMBER      0x0BADBADBADBADBAD

static const struct pci_device_id rtase_pci_tbl[] = {
	{PCI_VDEVICE(REALTEK, 0x906A)},
	{}
};

MODULE_DEVICE_TABLE(pci, rtase_pci_tbl);

MODULE_AUTHOR("Realtek ARD Software Team");
MODULE_DESCRIPTION("Network Driver for the PCIe interface of Realtek Automotive Ethernet Switch");
MODULE_LICENSE("Dual BSD/GPL");

struct rtase_counters {
	__le64 tx_packets;
	__le64 rx_packets;
	__le64 tx_errors;
	__le32 rx_errors;
	__le16 rx_missed;
	__le16 align_errors;
	__le32 tx_one_collision;
	__le32 tx_multi_collision;
	__le64 rx_unicast;
	__le64 rx_broadcast;
	__le32 rx_multicast;
	__le16 tx_aborted;
	__le16 tx_underun;
} __packed;

static void rtase_w8(const struct rtase_private *tp, u16 reg, u8 val8)
{
	writeb(val8, tp->mmio_addr + reg);
}

static void rtase_w16(const struct rtase_private *tp, u16 reg, u16 val16)
{
	writew(val16, tp->mmio_addr + reg);
}

static void rtase_w32(const struct rtase_private *tp, u16 reg, u32 val32)
{
	writel(val32, tp->mmio_addr + reg);
}

static u8 rtase_r8(const struct rtase_private *tp, u16 reg)
{
	return readb(tp->mmio_addr + reg);
}

static u16 rtase_r16(const struct rtase_private *tp, u16 reg)
{
	return readw(tp->mmio_addr + reg);
}

static u32 rtase_r32(const struct rtase_private *tp, u16 reg)
{
	return readl(tp->mmio_addr + reg);
}

static void rtase_set_rxbufsize(struct rtase_private *tp)
{
	tp->rx_buf_sz = RX_BUF_SIZE;
}

static int rtase_alloc_desc(struct rtase_private *tp)
{
	struct pci_dev *pdev = tp->pdev;
	u32 i;

	/* rx and tx descriptors needs 256 bytes alignment.
	 * dma_alloc_coherent provides more.
	 */
	for (i = 0; i < tp->func_tx_queue_num; i++) {
		tp->tx_ring[i].desc = dma_alloc_coherent(&pdev->dev,
							 RTASE_TX_RING_DESC_SIZE,
							 &tp->tx_ring[i].phy_addr,
							 GFP_KERNEL);
		if (!tp->tx_ring[i].desc)
			return -ENOMEM;
	}

	for (i = 0; i < tp->func_rx_queue_num; i++) {
		tp->rx_ring[i].desc =
			dma_alloc_coherent(&pdev->dev, RTASE_RX_RING_DESC_SIZE,
					   &tp->rx_ring[i].phy_addr,
					   GFP_KERNEL);
		if (!tp->rx_ring[i].desc)
			return -ENOMEM;
	}

	return 0;
}

static void rtase_free_desc(struct rtase_private *tp)
{
	struct pci_dev *pdev = tp->pdev;
	u32 i;

	for (i = 0; i < tp->func_tx_queue_num; i++) {
		if (!tp->tx_ring[i].desc)
			continue;

		dma_free_coherent(&pdev->dev, RTASE_TX_RING_DESC_SIZE,
				  tp->tx_ring[i].desc,
				  tp->tx_ring[i].phy_addr);
		tp->tx_ring[i].desc = NULL;
	}

	for (i = 0; i < tp->func_rx_queue_num; i++) {
		if (!tp->rx_ring[i].desc)
			continue;

		dma_free_coherent(&pdev->dev, RTASE_RX_RING_DESC_SIZE,
				  tp->rx_ring[i].desc,
				  tp->rx_ring[i].phy_addr);
		tp->rx_ring[i].desc = NULL;
	}
}

static void rtase_unmap_tx_skb(struct pci_dev *pdev, u32 len,
			       struct tx_desc *desc)
{
	dma_unmap_single(&pdev->dev, le64_to_cpu(desc->addr), len,
			 DMA_TO_DEVICE);
	desc->opts1 = cpu_to_le32(RTK_OPTS1_DEBUG_VALUE);
	desc->opts2 = 0x00;
	desc->addr = cpu_to_le64(RTK_MAGIC_NUMBER);
}

static void rtase_tx_clear_range(struct rtase_ring *ring, u32 start, u32 n)
{
	const struct rtase_private *tp = ring->ivec->tp;
	struct tx_desc *desc_base = ring->desc;
	struct net_device *dev = tp->dev;
	u32 i;

	for (i = 0; i < n; i++) {
		u32 entry = (start + i) % NUM_DESC;
		struct tx_desc *desc = desc_base + entry;
		u32 len = ring->mis.len[entry];
		struct sk_buff *skb;

		if (len == 0)
			continue;

		rtase_unmap_tx_skb(tp->pdev, len, desc);
		ring->mis.len[entry] = 0;
		skb = ring->skbuff[entry];
		if (!skb)
			continue;

		dev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		ring->skbuff[entry] = NULL;
	}
}

static void rtase_tx_clear(struct rtase_private *tp)
{
	struct rtase_ring *ring;
	u16 i;

	for (i = 0; i < tp->func_tx_queue_num; i++) {
		ring = &tp->tx_ring[i];
		rtase_tx_clear_range(ring, ring->dirty_idx, NUM_DESC);
		ring->cur_idx = 0;
		ring->dirty_idx = 0;
	}
}

static void rtase_mark_to_asic(union rx_desc *desc, u32 rx_buf_sz)
{
	u32 eor = le32_to_cpu(desc->desc_cmd.opts1) & RING_END;

	desc->desc_status.opts2 = 0;
	/* force memory writes to complete before releasing descriptor */
	dma_wmb();
	WRITE_ONCE(desc->desc_cmd.opts1,
		   cpu_to_le32(DESC_OWN | eor | rx_buf_sz));
}

static void rtase_tx_desc_init(struct rtase_private *tp, u16 idx)
{
	struct rtase_ring *ring = &tp->tx_ring[idx];
	struct tx_desc *desc;
	u32 i;

	memset(ring->desc, 0x0, RTASE_TX_RING_DESC_SIZE);
	memset(ring->skbuff, 0x0, sizeof(ring->skbuff));
	ring->cur_idx = 0;
	ring->dirty_idx = 0;
	ring->index = idx;

	for (i = 0; i < NUM_DESC; i++) {
		ring->mis.len[i] = 0;
		if ((NUM_DESC - 1) == i) {
			desc = ring->desc + sizeof(struct tx_desc) * i;
			desc->opts1 = cpu_to_le32(RING_END);
		}
	}

	ring->ring_handler = tx_handler;
	if (idx < 4) {
		ring->ivec = &tp->int_vector[idx];
		list_add_tail(&ring->ring_entry,
			      &tp->int_vector[idx].ring_list);
	} else {
		ring->ivec = &tp->int_vector[0];
		list_add_tail(&ring->ring_entry, &tp->int_vector[0].ring_list);
	}
}

static void rtase_map_to_asic(union rx_desc *desc, dma_addr_t mapping,
			      u32 rx_buf_sz)
{
	desc->desc_cmd.addr = cpu_to_le64(mapping);
	/* make sure the physical address has been updated */
	wmb();
	rtase_mark_to_asic(desc, rx_buf_sz);
}

static void rtase_make_unusable_by_asic(union rx_desc *desc)
{
	desc->desc_cmd.addr = cpu_to_le64(RTK_MAGIC_NUMBER);
	desc->desc_cmd.opts1 &= ~cpu_to_le32(DESC_OWN | RSVD_MASK);
}

static int rtase_alloc_rx_skb(const struct rtase_ring *ring,
			      struct sk_buff **p_sk_buff, union rx_desc *desc,
			      dma_addr_t *rx_phy_addr, u8 in_intr)
{
	struct rtase_int_vector *ivec = ring->ivec;
	const struct rtase_private *tp = ivec->tp;
	struct sk_buff *skb = NULL;
	dma_addr_t mapping;
	struct page *page;
	void *buf_addr;
	int ret = 0;

	page = page_pool_dev_alloc_pages(tp->page_pool);
	if (!page) {
		netdev_err(tp->dev, "failed to alloc page\n");
		goto err_out;
	}

	buf_addr = page_address(page);
	mapping = page_pool_get_dma_addr(page);

	skb = build_skb(buf_addr, PAGE_SIZE);
	if (!skb) {
		page_pool_put_full_page(tp->page_pool, page, true);
		netdev_err(tp->dev, "failed to build skb\n");
		goto err_out;
	}

	*p_sk_buff = skb;
	*rx_phy_addr = mapping;
	rtase_map_to_asic(desc, mapping, tp->rx_buf_sz);

	return ret;

err_out:
	if (skb)
		dev_kfree_skb(skb);

	ret = -ENOMEM;
	rtase_make_unusable_by_asic(desc);

	return ret;
}

static u32 rtase_rx_ring_fill(struct rtase_ring *ring, u32 ring_start,
			      u32 ring_end, u8 in_intr)
{
	union rx_desc *desc_base = ring->desc;
	u32 cur;

	for (cur = ring_start; ring_end - cur > 0; cur++) {
		u32 i = cur % NUM_DESC;
		union rx_desc *desc = desc_base + i;
		int ret;

		if (ring->skbuff[i])
			continue;

		ret = rtase_alloc_rx_skb(ring, &ring->skbuff[i], desc,
					 &ring->mis.data_phy_addr[i],
					 in_intr);
		if (ret)
			break;
	}

	return cur - ring_start;
}

static void rtase_mark_as_last_descriptor(union rx_desc *desc)
{
	desc->desc_cmd.opts1 |= cpu_to_le32(RING_END);
}

static void rtase_rx_ring_clear(struct rtase_ring *ring)
{
	union rx_desc *desc;
	u32 i;

	for (i = 0; i < NUM_DESC; i++) {
		desc = ring->desc + sizeof(union rx_desc) * i;

		if (!ring->skbuff[i])
			continue;

		dev_kfree_skb(ring->skbuff[i]);

		ring->skbuff[i] = NULL;

		rtase_make_unusable_by_asic(desc);
	}
}

static void rtase_rx_desc_init(struct rtase_private *tp, u16 idx)
{
	struct rtase_ring *ring = &tp->rx_ring[idx];
	u16 i;

	memset(ring->desc, 0x0, RTASE_RX_RING_DESC_SIZE);
	memset(ring->skbuff, 0x0, sizeof(ring->skbuff));
	ring->cur_idx = 0;
	ring->dirty_idx = 0;
	ring->index = idx;

	for (i = 0; i < NUM_DESC; i++)
		ring->mis.data_phy_addr[i] = 0;

	ring->ring_handler = rx_handler;
	ring->ivec = &tp->int_vector[idx];
	list_add_tail(&ring->ring_entry, &tp->int_vector[idx].ring_list);
}

static void rtase_rx_clear(struct rtase_private *tp)
{
	u32 i;

	for (i = 0; i < tp->func_rx_queue_num; i++)
		rtase_rx_ring_clear(&tp->rx_ring[i]);

	page_pool_destroy(tp->page_pool);
	tp->page_pool = NULL;
}

static int rtase_init_ring(const struct net_device *dev)
{
	struct rtase_private *tp = netdev_priv(dev);
	struct page_pool_params pp_params = {
		.flags = PP_FLAG_DMA_MAP | PP_FLAG_DMA_SYNC_DEV,
		.order = 0,
		.pool_size = NUM_DESC * tp->func_rx_queue_num,
		.nid = dev_to_node(&tp->pdev->dev),
		.dev = &tp->pdev->dev,
		.dma_dir = DMA_FROM_DEVICE,
		.max_len = PAGE_SIZE,
		.offset = 0,
	};
	struct page_pool *page_pool;
	u32 num;
	u16 i;

	page_pool = page_pool_create(&pp_params);
	if (IS_ERR(page_pool)) {
		netdev_err(tp->dev, "failed to create page pool\n");
		return -ENOMEM;
	}

	tp->page_pool = page_pool;

	for (i = 0; i < tp->func_tx_queue_num; i++)
		rtase_tx_desc_init(tp, i);

	for (i = 0; i < tp->func_rx_queue_num; i++) {
		rtase_rx_desc_init(tp, i);
		num = rtase_rx_ring_fill(&tp->rx_ring[i], 0, NUM_DESC, 0);
		if (num != NUM_DESC)
			goto err_out;

		rtase_mark_as_last_descriptor(tp->rx_ring[i].desc +
					      sizeof(union rx_desc) *
					      (NUM_DESC - 1));
	}

	return 0;

err_out:
	rtase_rx_clear(tp);
	return -ENOMEM;
}

static void rtase_interrupt_mitigation(const struct rtase_private *tp)
{
	u32 i;

	for (i = 0; i < tp->func_tx_queue_num; i++)
		rtase_w16(tp, RTASE_INT_MITI_TX + i * 2, tp->tx_int_mit);

	for (i = 0; i < tp->func_rx_queue_num; i++)
		rtase_w16(tp, RTASE_INT_MITI_RX + i * 2, tp->rx_int_mit);
}

static void rtase_tally_counter_addr_fill(const struct rtase_private *tp)
{
	rtase_w32(tp, RTASE_DTCCR4, upper_32_bits(tp->tally_paddr));
	rtase_w32(tp, RTASE_DTCCR0, lower_32_bits(tp->tally_paddr));
}

static void rtase_tally_counter_clear(const struct rtase_private *tp)
{
	u32 cmd = lower_32_bits(tp->tally_paddr);

	rtase_w32(tp, RTASE_DTCCR4, upper_32_bits(tp->tally_paddr));
	rtase_w32(tp, RTASE_DTCCR0, cmd | COUNTER_RESET);
}

static void rtase_desc_addr_fill(const struct rtase_private *tp)
{
	const struct rtase_ring *ring;
	u16 i, cmd, val;
	int err;

	for (i = 0; i < tp->func_tx_queue_num; i++) {
		ring = &tp->tx_ring[i];

		rtase_w32(tp, RTASE_TX_DESC_ADDR0,
			  lower_32_bits(ring->phy_addr));
		rtase_w32(tp, RTASE_TX_DESC_ADDR4,
			  upper_32_bits(ring->phy_addr));

		cmd = i | TX_DESC_CMD_WE | TX_DESC_CMD_CS;
		rtase_w16(tp, RTASE_TX_DESC_COMMAND, cmd);

		err = read_poll_timeout(rtase_r16, val, !(val & TX_DESC_CMD_CS),
					10, 1000, false, tp, RTASE_TX_DESC_COMMAND);

		if (err == -ETIMEDOUT)
			netdev_err(tp->dev, "error occurred in fill tx descriptor\n");
	}

	for (i = 0; i < tp->func_rx_queue_num; i++) {
		ring = &tp->rx_ring[i];

		if (i == 0) {
			rtase_w32(tp, RTASE_Q0_RX_DESC_ADDR0,
				  lower_32_bits(ring->phy_addr));
			rtase_w32(tp, RTASE_Q0_RX_DESC_ADDR4,
				  upper_32_bits(ring->phy_addr));
		} else {
			rtase_w32(tp, (RTASE_Q1_RX_DESC_ADDR0 + ((i - 1) * 8)),
				  lower_32_bits(ring->phy_addr));
			rtase_w32(tp, (RTASE_Q1_RX_DESC_ADDR4 + ((i - 1) * 8)),
				  upper_32_bits(ring->phy_addr));
		}
	}
}

static void rtase_hw_set_features(const struct net_device *dev,
				  netdev_features_t features)
{
	const struct rtase_private *tp = netdev_priv(dev);
	u16 rx_config, val;

	rx_config = rtase_r16(tp, RTASE_RX_CONFIG_0);
	if (features & NETIF_F_RXALL)
		rx_config |= (ACCEPT_ERR | ACCEPT_RUNT);
	else
		rx_config &= ~(ACCEPT_ERR | ACCEPT_RUNT);

	rtase_w16(tp, RTASE_RX_CONFIG_0, rx_config);

	val = rtase_r16(tp, RTASE_CPLUS_CMD);
	if (features & NETIF_F_RXCSUM)
		rtase_w16(tp, RTASE_CPLUS_CMD, val | RX_CHKSUM);
	else
		rtase_w16(tp, RTASE_CPLUS_CMD, val & ~RX_CHKSUM);

	rx_config = rtase_r16(tp, RTASE_RX_CONFIG_1);
	if (dev->features & NETIF_F_HW_VLAN_CTAG_RX)
		rx_config |= (INNER_VLAN_DETAG_EN | OUTER_VLAN_DETAG_EN);
	else
		rx_config &= ~(INNER_VLAN_DETAG_EN | OUTER_VLAN_DETAG_EN);

	rtase_w16(tp, RTASE_RX_CONFIG_1, rx_config);
}

static void rtase_hw_set_rx_packet_filter(struct net_device *dev)
{
	u32 mc_filter[2] = { 0xFFFFFFFF, 0xFFFFFFFF };
	struct rtase_private *tp = netdev_priv(dev);
	u16 rx_mode;

	rx_mode = rtase_r16(tp, RTASE_RX_CONFIG_0) & ~ACCEPT_MASK;
	rx_mode |= ACCEPT_BROADCAST | ACCEPT_MYPHYS;

	if (dev->flags & IFF_PROMISC) {
		rx_mode |= ACCEPT_MULTICAST | ACCEPT_ALLPHYS;
	} else if (dev->flags & IFF_ALLMULTI) {
		rx_mode |= ACCEPT_MULTICAST;
	} else {
		struct netdev_hw_addr *hw_addr;

		mc_filter[0] = 0;
		mc_filter[1] = 0;

		netdev_for_each_mc_addr(hw_addr, dev) {
			u32 bit_nr = eth_hw_addr_crc(hw_addr);
			u32 idx = u32_get_bits(bit_nr, BIT(31));
			u32 bit = u32_get_bits(bit_nr, MULTICAST_FILTER_MASK);

			mc_filter[idx] |= BIT(bit);
			rx_mode |= ACCEPT_MULTICAST;
		}
	}

	if (dev->features & NETIF_F_RXALL)
		rx_mode |= ACCEPT_ERR | ACCEPT_RUNT;

	rtase_w32(tp, RTASE_MAR0, swab32(mc_filter[1]));
	rtase_w32(tp, RTASE_MAR1, swab32(mc_filter[0]));
	rtase_w16(tp, RTASE_RX_CONFIG_0, rx_mode);
}

static void rtase_irq_dis_and_clear(const struct rtase_private *tp)
{
	const struct rtase_int_vector *ivec = &tp->int_vector[0];
	u32 val1;
	u16 val2;
	u8 i;

	rtase_w32(tp, ivec->imr_addr, 0);
	val1 = rtase_r32(tp, ivec->isr_addr);
	rtase_w32(tp, ivec->isr_addr, val1);

	for (i = 1; i < tp->int_nums; i++) {
		ivec = &tp->int_vector[i];
		rtase_w16(tp, ivec->imr_addr, 0);
		val2 = rtase_r16(tp, ivec->isr_addr);
		rtase_w16(tp, ivec->isr_addr, val2);
	}
}

static void rtase_poll_timeout(const struct rtase_private *tp, u32 cond,
			       u32 sleep_us, u64 timeout_us, u16 reg)
{
	int err;
	u8 val;

	err = read_poll_timeout(rtase_r8, val, val & cond, sleep_us, timeout_us,
				false, tp, reg);

	if (err == -ETIMEDOUT)
		netdev_err(tp->dev, "poll reg 0x00%x timeout\n", reg);
}

static void rtase_nic_reset(const struct net_device *dev)
{
	const struct rtase_private *tp = netdev_priv(dev);
	u16 rx_config;
	u8 val;

	rx_config = rtase_r16(tp, RTASE_RX_CONFIG_0);
	rtase_w16(tp, RTASE_RX_CONFIG_0, rx_config & ~ACCEPT_MASK);

	val = rtase_r8(tp, RTASE_MISC);
	rtase_w8(tp, RTASE_MISC, val | RX_DV_GATE_EN);

	val = rtase_r8(tp, RTASE_CHIP_CMD);
	rtase_w8(tp, RTASE_CHIP_CMD, val | STOP_REQ);
	mdelay(2);

	rtase_poll_timeout(tp, STOP_REQ_DONE, 100, 150000, RTASE_CHIP_CMD);

	rtase_poll_timeout(tp, TX_FIFO_EMPTY, 100, 100000, RTASE_FIFOR);

	rtase_poll_timeout(tp, RX_FIFO_EMPTY, 100, 100000, RTASE_FIFOR);

	val = rtase_r8(tp, RTASE_CHIP_CMD);
	rtase_w8(tp, RTASE_CHIP_CMD, val & ~(TE | RE));
	val = rtase_r8(tp, RTASE_CHIP_CMD);
	rtase_w8(tp, RTASE_CHIP_CMD, val & ~STOP_REQ);

	rtase_w16(tp, RTASE_RX_CONFIG_0, rx_config);
}

static void rtase_hw_reset(const struct net_device *dev)
{
	const struct rtase_private *tp = netdev_priv(dev);

	rtase_irq_dis_and_clear(tp);

	rtase_nic_reset(dev);
}

static void rtase_set_rx_queue(const struct rtase_private *tp)
{
	u16 reg_data;

	reg_data = rtase_r16(tp, RTASE_FCR);
	switch (tp->func_rx_queue_num) {
	case 1:
		u16p_replace_bits(&reg_data, 0x1, FCR_RXQ_MASK);
		break;
	case 2:
		u16p_replace_bits(&reg_data, 0x2, FCR_RXQ_MASK);
		break;
	case 4:
		u16p_replace_bits(&reg_data, 0x3, FCR_RXQ_MASK);
		break;
	}
	rtase_w16(tp, RTASE_FCR, reg_data);
}

static void rtase_set_tx_queue(const struct rtase_private *tp)
{
	u16 reg_data;

	reg_data = rtase_r16(tp, RTASE_TX_CONFIG_1);
	switch (tp->tx_queue_ctrl) {
	case 1:
		u16p_replace_bits(&reg_data, 0x0, TC_MODE_MASK);
		break;
	case 2:
		u16p_replace_bits(&reg_data, 0x1, TC_MODE_MASK);
		break;
	case 3:
	case 4:
		u16p_replace_bits(&reg_data, 0x2, TC_MODE_MASK);
		break;
	default:
		u16p_replace_bits(&reg_data, 0x3, TC_MODE_MASK);
		break;
	}
	rtase_w16(tp, RTASE_TX_CONFIG_1, reg_data);
}

static void rtase_hw_config(struct net_device *dev)
{
	const struct rtase_private *tp = netdev_priv(dev);
	u32 reg_data32;
	u16 reg_data16;

	rtase_hw_reset(dev);

	/* set rx dma burst */
	reg_data16 = rtase_r16(tp, RTASE_RX_CONFIG_0);
	reg_data16 &= ~(RX_SINGLE_TAG | RX_SINGLE_FETCH);
	u16p_replace_bits(&reg_data16, RX_DMA_BURST_256, RX_MX_DMA_MASK);
	rtase_w16(tp, RTASE_RX_CONFIG_0, reg_data16);

	/* new rx descritpor */
	reg_data16 = rtase_r16(tp, RTASE_RX_CONFIG_1);
	reg_data16 |= RX_NEW_DESC_FORMAT_EN | PCIE_NEW_FLOW;
	u16p_replace_bits(&reg_data16, 0xF, RX_MAX_FETCH_DESC_MASK);
	rtase_w16(tp, RTASE_RX_CONFIG_1, reg_data16);

	rtase_set_rx_queue(tp);

	rtase_interrupt_mitigation(tp);

	/* set tx dma burst size and interframe gap time */
	reg_data32 = rtase_r32(tp, RTASE_TX_CONFIG_0);
	u32p_replace_bits(&reg_data32, TX_DMA_BURST_UNLIMITED, TX_DMA_MASK);
	u32p_replace_bits(&reg_data32, INTERFRAMEGAP, TX_INTER_FRAME_GAP_MASK);
	rtase_w32(tp, RTASE_TX_CONFIG_0, reg_data32);

	/* new tx descriptor */
	reg_data16 = rtase_r16(tp, RTASE_TFUN_CTRL);
	rtase_w16(tp, RTASE_TFUN_CTRL, reg_data16 | TX_NEW_DESC_FORMAT_EN);

	/* tx fetch desc number */
	rtase_w8(tp, RTASE_TDFNR, 0x10);

	/* tag num select */
	reg_data16 = rtase_r16(tp, RTASE_MTPS);
	u16p_replace_bits(&reg_data16, 0x4, TAG_NUM_SEL_MASK);
	rtase_w16(tp, RTASE_MTPS, reg_data16);

	rtase_set_tx_queue(tp);

	rtase_w16(tp, RTASE_TOKSEL, 0x5555);

	rtase_tally_counter_addr_fill(tp);
	rtase_desc_addr_fill(tp);
	rtase_hw_set_features(dev, dev->features);

	/* enable flow control */
	reg_data16 = rtase_r16(tp, RTASE_CPLUS_CMD);
	reg_data16 |= (FORCE_TXFLOW_EN | FORCE_RXFLOW_EN);
	rtase_w16(tp, RTASE_CPLUS_CMD, reg_data16);
	/* set near fifo threshold - rx missed issue. */
	rtase_w16(tp, RTASE_RFIFONFULL, 0x190);

	rtase_w16(tp, RTASE_RMS, tp->rx_buf_sz);

	rtase_hw_set_rx_packet_filter(dev);
}

static void rtase_nic_enable(const struct net_device *dev)
{
	const struct rtase_private *tp = netdev_priv(dev);
	u16 rcr = rtase_r16(tp, RTASE_RX_CONFIG_1);
	u8 val;

	rtase_w16(tp, RTASE_RX_CONFIG_1, rcr & ~PCIE_RELOAD_En);
	rtase_w16(tp, RTASE_RX_CONFIG_1, rcr | PCIE_RELOAD_En);

	val = rtase_r8(tp, RTASE_CHIP_CMD);
	rtase_w8(tp, RTASE_CHIP_CMD, val | TE | RE);

	val = rtase_r8(tp, RTASE_MISC);
	rtase_w8(tp, RTASE_MISC, val & ~RX_DV_GATE_EN);
}

static void rtase_enable_hw_interrupt(const struct rtase_private *tp)
{
	const struct rtase_int_vector *ivec = &tp->int_vector[0];
	u32 i;

	rtase_w32(tp, ivec->imr_addr, ivec->imr);

	for (i = 1; i < tp->int_nums; i++) {
		ivec = &tp->int_vector[i];
		rtase_w16(tp, ivec->imr_addr, ivec->imr);
	}
}

static void rtase_hw_start(const struct net_device *dev)
{
	const struct rtase_private *tp = netdev_priv(dev);

	rtase_nic_enable(dev);
	rtase_enable_hw_interrupt(tp);
}

/*  the interrupt handler does RXQ0 and TXQ0, TXQ4~7 interrutp status
 */
static irqreturn_t rtase_interrupt(int irq, void *dev_instance)
{
	const struct rtase_private *tp;
	struct rtase_int_vector *ivec;
	u32 status;

	ivec = dev_instance;
	tp = ivec->tp;
	status = rtase_r32(tp, ivec->isr_addr);

	rtase_w32(tp, ivec->imr_addr, 0x0);
	rtase_w32(tp, ivec->isr_addr, status & ~FOVW);

	if (napi_schedule_prep(&ivec->napi))
		__napi_schedule(&ivec->napi);

	return IRQ_HANDLED;
}

/*  the interrupt handler does RXQ1&TXQ1 or RXQ2&TXQ2 or RXQ3&TXQ3 interrupt
 *  status according to interrupt vector
 */
static irqreturn_t rtase_q_interrupt(int irq, void *dev_instance)
{
	const struct rtase_private *tp;
	struct rtase_int_vector *ivec;
	u16 status;

	ivec = dev_instance;
	tp = ivec->tp;
	status = rtase_r16(tp, ivec->isr_addr);

	rtase_w16(tp, ivec->imr_addr, 0x0);
	rtase_w16(tp, ivec->isr_addr, status);

	if (napi_schedule_prep(&ivec->napi))
		__napi_schedule(&ivec->napi);

	return IRQ_HANDLED;
}

static int rtase_poll(struct napi_struct *napi, int budget)
{
	const struct rtase_int_vector *ivec;
	const struct rtase_private *tp;
	struct rtase_ring *ring;
	int total_workdone = 0;

	ivec = container_of(napi, struct rtase_int_vector, napi);
	tp = ivec->tp;

	list_for_each_entry(ring, &ivec->ring_list, ring_entry) {
		total_workdone += ring->ring_handler(ring, budget);
	}

	if (total_workdone >= budget)
		return budget;

	if (napi_complete_done(napi, total_workdone)) {
		if (!ivec->index)
			rtase_w32(tp, ivec->imr_addr, ivec->imr);
		else
			rtase_w16(tp, ivec->imr_addr, ivec->imr);
	}

	return total_workdone;
}

static int rtase_open(struct net_device *dev)
{
	struct rtase_private *tp = netdev_priv(dev);
	struct rtase_int_vector *ivec = &tp->int_vector[0];
	const struct pci_dev *pdev = tp->pdev;
	int ret;
	u16 i;

	rtase_set_rxbufsize(tp);

	ret = rtase_alloc_desc(tp);
	if (ret)
		goto err_free_all_allocated_mem;

	ret = rtase_init_ring(dev);
	if (ret)
		goto err_free_all_allocated_mem;

	rtase_hw_config(dev);

	if (tp->sw_flag & SWF_MSIX_ENABLED) {
		ret = request_irq(ivec->irq, rtase_interrupt, 0,
				  dev->name, ivec);

		/* request other interrupts to handle multiqueue */
		for (i = 1; i < tp->int_nums; i++) {
			if (ret)
				continue;

			ivec = &tp->int_vector[i];
			if (ivec->status != 1)
				continue;

			snprintf(ivec->name, sizeof(ivec->name), "%s_int%i", tp->dev->name, i);
			ret = request_irq(ivec->irq, rtase_q_interrupt, 0,
					  ivec->name, ivec);
		}
	} else if (tp->sw_flag & SWF_MSI_ENABLED) {
		ret = request_irq(pdev->irq, rtase_interrupt, 0, dev->name,
				  ivec);
	} else {
		ret = request_irq(pdev->irq, rtase_interrupt, IRQF_SHARED,
				  dev->name, ivec);
	}

	if (ret != 0) {
		netdev_err(dev, "can't request MSIX interrupt. Error: %d\n", ret);
		goto err_free_all_allocated_mem;
	}

	rtase_hw_start(dev);

	netif_carrier_on(dev);
	netif_wake_queue(dev);

	goto out;

err_free_all_allocated_mem:
	rtase_free_desc(tp);

out:
	return ret;
}

static void rtase_down(struct net_device *dev)
{
	struct rtase_private *tp = netdev_priv(dev);
	u32 i;

	netif_stop_queue(dev);

	/* give a racing hard_start_xmit a few cycles to complete */
	synchronize_rcu();

	netif_carrier_off(dev);

	rtase_hw_reset(dev);

	for (i = 0; i < tp->int_nums; i++)
		synchronize_irq(tp->int_vector[i].irq);

	rtase_tx_clear(tp);

	rtase_rx_clear(tp);
}

static int rtase_close(struct net_device *dev)
{
	struct rtase_private *tp = netdev_priv(dev);
	const struct pci_dev *pdev = tp->pdev;
	u32 i;

	rtase_down(dev);

	if (tp->sw_flag & SWF_MSIX_ENABLED) {
		for (i = 0; i < tp->int_nums; i++)
			free_irq(tp->int_vector[i].irq, &tp->int_vector[i]);

	} else {
		free_irq(pdev->irq, &tp->int_vector[0]);
	}

	rtase_free_desc(tp);

	return 0;
}

static void rtase_enable_eem_write(const struct rtase_private *tp)
{
	u8 val;

	val = rtase_r8(tp, RTASE_EEM);
	rtase_w8(tp, RTASE_EEM, val | EEM_UNLOCK);
}

static void rtase_disable_eem_write(const struct rtase_private *tp)
{
	u8 val;

	val = rtase_r8(tp, RTASE_EEM);
	rtase_w8(tp, RTASE_EEM, val & ~EEM_UNLOCK);
}

static void rtase_rar_set(const struct rtase_private *tp, const u8 *addr)
{
	u32 rar_low, rar_high;

	rar_low = (u32)addr[0] | ((u32)addr[1] << 8) |
		  ((u32)addr[2] << 16) | ((u32)addr[3] << 24);

	rar_high = (u32)addr[4] | ((u32)addr[5] << 8);

	rtase_enable_eem_write(tp);
	rtase_w32(tp, RTASE_MAC0, rar_low);
	rtase_w32(tp, RTASE_MAC4, rar_high);
	rtase_disable_eem_write(tp);
	rtase_w16(tp, RTASE_LBK_CTRL, LBK_ATLD | LBK_CLR);
}

#ifdef CONFIG_NET_POLL_CONTROLLER
/* Polling 'interrupt' - used by things like netconsole to send skbs
 * without having to re-enable interrupts. It's not called while
 * the interrupt routine is executing.
 */
static void rtase_netpoll(struct net_device *dev)
{
	const struct rtase_private *tp = netdev_priv(dev);
	const struct pci_dev *pdev = tp->pdev;

	disable_irq(pdev->irq);
	rtase_interrupt(pdev->irq, dev);
	enable_irq(pdev->irq);
}
#endif

static const struct net_device_ops rtase_netdev_ops = {
	.ndo_open = rtase_open,
	.ndo_stop = rtase_close,
#ifdef CONFIG_NET_POLL_CONTROLLER
	.ndo_poll_controller = rtase_netpoll,
#endif
};

static void rtase_get_mac_address(struct net_device *dev)
{
	struct rtase_private *tp = netdev_priv(dev);
	u8 mac_addr[ETH_ALEN] __aligned(2) = {};
	u32 i;

	for (i = 0; i < ETH_ALEN; i++)
		mac_addr[i] = rtase_r8(tp, RTASE_MAC0 + i);

	if (!is_valid_ether_addr(mac_addr)) {
		eth_random_addr(mac_addr);
		dev->addr_assign_type = NET_ADDR_RANDOM;
		netdev_warn(dev, "Random ether addr %pM\n", mac_addr);
	}

	eth_hw_addr_set(dev, mac_addr);
	rtase_rar_set(tp, mac_addr);

	ether_addr_copy(dev->perm_addr, dev->dev_addr);
}

static void rtase_init_netdev_ops(struct net_device *dev)
{
	dev->netdev_ops = &rtase_netdev_ops;
}

static void rtase_reset_interrupt(struct pci_dev *pdev,
				  const struct rtase_private *tp)
{
	if (tp->sw_flag & SWF_MSIX_ENABLED)
		pci_disable_msix(pdev);
	else
		pci_disable_msi(pdev);
}

static int rtase_alloc_msix(struct pci_dev *pdev, struct rtase_private *tp)
{
	int ret;
	u16 i;

	memset(tp->msix_entry, 0x0, RTASE_NUM_MSIX * sizeof(struct msix_entry));

	for (i = 0; i < RTASE_NUM_MSIX; i++)
		tp->msix_entry[i].entry = i;

	ret = pci_enable_msix_range(pdev, tp->msix_entry, tp->int_nums,
				    tp->int_nums);

	if (ret == tp->int_nums) {
		for (i = 0; i < tp->int_nums; i++) {
			tp->int_vector[i].irq = pci_irq_vector(pdev, i);
			tp->int_vector[i].status = 1;
		}
	}

	return ret;
}

static int rtase_alloc_interrupt(struct pci_dev *pdev,
				 struct rtase_private *tp)
{
	int ret;

	ret = rtase_alloc_msix(pdev, tp);
	if (ret != tp->int_nums) {
		ret = pci_enable_msi(pdev);
		if (ret)
			dev_err(&pdev->dev,
				"unable to alloc interrupt.(MSI)\n");
		else
			tp->sw_flag |= SWF_MSI_ENABLED;
	} else {
		tp->sw_flag |= SWF_MSIX_ENABLED;
	}

	return ret;
}

static void rtase_init_hardware(const struct rtase_private *tp)
{
	u16 i;

	for (i = 0; i < RTASE_VLAN_FILTER_ENTRY_NUM; i++)
		rtase_w32(tp, RTASE_VLAN_ENTRY_0 + i * 4, 0);
}

static void rtase_init_int_vector(struct rtase_private *tp)
{
	u16 i;

	/* interrupt vector 0 */
	tp->int_vector[0].tp = tp;
	tp->int_vector[0].index = 0;
	tp->int_vector[0].imr_addr = RTASE_IMR0;
	tp->int_vector[0].isr_addr = RTASE_ISR0;
	tp->int_vector[0].imr = ROK | RDU | TOK | TOK4 | TOK5 | TOK6 | TOK7;
	tp->int_vector[0].poll = rtase_poll;

	memset(tp->int_vector[0].name, 0x0, sizeof(tp->int_vector[0].name));
	INIT_LIST_HEAD(&tp->int_vector[0].ring_list);

	netif_napi_add(tp->dev, &tp->int_vector[0].napi,
		       tp->int_vector[0].poll);
	napi_enable(&tp->int_vector[0].napi);

	/* interrupt vector 1 ~ 3 */
	for (i = 1; i < tp->int_nums; i++) {
		tp->int_vector[i].tp = tp;
		tp->int_vector[i].index = i;
		tp->int_vector[i].imr_addr = RTASE_IMR1 + (i - 1) * 4;
		tp->int_vector[i].isr_addr = RTASE_ISR1 + (i - 1) * 4;
		tp->int_vector[i].imr = Q_ROK | Q_RDU | Q_TOK;
		tp->int_vector[i].poll = rtase_poll;

		memset(tp->int_vector[i].name, 0x0, sizeof(tp->int_vector[0].name));
		INIT_LIST_HEAD(&tp->int_vector[i].ring_list);

		netif_napi_add(tp->dev, &tp->int_vector[i].napi,
			       tp->int_vector[i].poll);
		napi_enable(&tp->int_vector[i].napi);
	}
}

static u16 rtase_calc_time_mitigation(u32 time_us)
{
	u8 msb, time_count, time_unit;
	u16 int_miti;

	time_us = min_t(int, time_us, MITI_MAX_TIME);

	msb = fls(time_us);
	if (msb >= MITI_COUNT_BIT_NUM) {
		time_unit = msb - MITI_COUNT_BIT_NUM;
		time_count = time_us >> (msb - MITI_COUNT_BIT_NUM);
	} else {
		time_unit = 0;
		time_count = time_us;
	}

	int_miti = u16_encode_bits(time_count, MITI_TIME_COUNT_MASK) |
		   u16_encode_bits(time_unit, MITI_TIME_UNIT_MASK);

	return int_miti;
}

static u16 rtase_calc_packet_num_mitigation(u16 pkt_num)
{
	u8 msb, pkt_num_count, pkt_num_unit;
	u16 int_miti;

	pkt_num = min_t(int, pkt_num, MITI_MAX_PKT_NUM);

	if (pkt_num > 60) {
		pkt_num_unit = MITI_MAX_PKT_NUM_IDX;
		pkt_num_count = pkt_num / MITI_MAX_PKT_NUM_UNIT;
	} else {
		msb = fls(pkt_num);
		if (msb >= MITI_COUNT_BIT_NUM) {
			pkt_num_unit = msb - MITI_COUNT_BIT_NUM;
			pkt_num_count = pkt_num >> (msb - MITI_COUNT_BIT_NUM);
		} else {
			pkt_num_unit = 0;
			pkt_num_count = pkt_num;
		}
	}

	int_miti = u16_encode_bits(pkt_num_count, MITI_PKT_NUM_COUNT_MASK) |
		   u16_encode_bits(pkt_num_unit, MITI_PKT_NUM_UNIT_MASK);

	return int_miti;
}

static void rtase_init_software_variable(struct pci_dev *pdev,
					 struct rtase_private *tp)
{
	u16 int_miti;

	tp->tx_queue_ctrl = RTASE_TXQ_CTRL;
	tp->func_tx_queue_num = RTASE_FUNC_TXQ_NUM;
	tp->func_rx_queue_num = RTASE_FUNC_RXQ_NUM;
	tp->int_nums = RTASE_INTERRUPT_NUM;

	int_miti = rtase_calc_time_mitigation(MITI_DEFAULT_TIME) |
		   rtase_calc_packet_num_mitigation(MITI_DEFAULT_PKT_NUM);
	tp->tx_int_mit = int_miti;
	tp->rx_int_mit = int_miti;

	tp->sw_flag = 0;

	rtase_init_int_vector(tp);

	/* MTU range: 60 - hw-specific max */
	tp->dev->min_mtu = ETH_ZLEN;
	tp->dev->max_mtu = MAX_JUMBO_SIZE;
}

static bool rtase_check_mac_version_valid(struct rtase_private *tp)
{
	u32 hw_ver = rtase_r32(tp, RTASE_TX_CONFIG_0) & HW_VER_MASK;
	bool known_ver = false;

	switch (hw_ver) {
	case 0x00800000:
	case 0x04000000:
	case 0x04800000:
		known_ver = true;
		break;
	}

	return known_ver;
}

static int rtase_init_board(struct pci_dev *pdev, struct net_device **dev_out,
			    void __iomem **ioaddr_out)
{
	struct net_device *dev;
	void __iomem *ioaddr;
	int ret = -ENOMEM;

	/* dev zeroed in alloc_etherdev */
	dev = alloc_etherdev_mq(sizeof(struct rtase_private),
				RTASE_FUNC_TXQ_NUM);
	if (!dev)
		goto err_out;

	SET_NETDEV_DEV(dev, &pdev->dev);

	ret = pci_enable_device(pdev);
	if (ret < 0)
		goto err_out_free_dev;

	/* make sure PCI base addr 1 is MMIO */
	if (!(pci_resource_flags(pdev, 2) & IORESOURCE_MEM)) {
		ret = -ENODEV;
		goto err_out_disable;
	}

	/* check for weird/broken PCI region reporting */
	if (pci_resource_len(pdev, 2) < RTASE_REGS_SIZE) {
		ret = -ENODEV;
		goto err_out_disable;
	}

	ret = pci_request_regions(pdev, KBUILD_MODNAME);
	if (ret < 0)
		goto err_out_disable;

	if (!dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64)))
		dev->features |= NETIF_F_HIGHDMA;
	else if (dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32)))
		goto err_out_free_res;
	else
		dev_info(&pdev->dev, "DMA_BIT_MASK: 32\n");

	pci_set_master(pdev);

	/* ioremap MMIO region */
	ioaddr = ioremap(pci_resource_start(pdev, 2),
			 pci_resource_len(pdev, 2));
	if (!ioaddr) {
		ret = -EIO;
		goto err_out_free_res;
	}

	*ioaddr_out = ioaddr;
	*dev_out = dev;

	return ret;

err_out_free_res:
	pci_release_regions(pdev);

err_out_disable:
	pci_disable_device(pdev);

err_out_free_dev:
	free_netdev(dev);

err_out:
	*ioaddr_out = NULL;
	*dev_out = NULL;

	return ret;
}

static void rtase_release_board(struct pci_dev *pdev, struct net_device *dev,
				void __iomem *ioaddr)
{
	const struct rtase_private *tp = netdev_priv(dev);

	rtase_rar_set(tp, tp->dev->perm_addr);
	iounmap(ioaddr);

	if ((tp->sw_flag & SWF_MSIX_ENABLED))
		pci_disable_msix(pdev);
	else
		pci_disable_msi(pdev);

	pci_release_regions(pdev);
	pci_disable_device(pdev);
	free_netdev(dev);
}

static int rtase_init_one(struct pci_dev *pdev,
			  const struct pci_device_id *ent)
{
	struct net_device *dev = NULL;
	void __iomem *ioaddr = NULL;
	struct rtase_private *tp;
	int ret;

	if (!pdev->is_physfn && pdev->is_virtfn) {
		dev_err(&pdev->dev, "This module does not support a virtual function.");
		return -EINVAL;
	}

	dev_dbg(&pdev->dev, "Automotive Switch Ethernet driver loaded\n");

	ret = rtase_init_board(pdev, &dev, &ioaddr);
	if (ret != 0)
		return ret;

	tp = netdev_priv(dev);
	tp->mmio_addr = ioaddr;
	tp->dev = dev;
	tp->pdev = pdev;

	/* identify chip attached to board */
	if (!rtase_check_mac_version_valid(tp)) {
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "unknown chip version, contact rtase maintainers (see MAINTAINERS file)\n");
	}

	dev->tstats = netdev_alloc_pcpu_stats(struct pcpu_sw_netstats);
	if (!dev->tstats)
		goto err_out_1;

	rtase_init_software_variable(pdev, tp);
	rtase_init_hardware(tp);

	ret = rtase_alloc_interrupt(pdev, tp);
	if (ret < 0) {
		dev_err(&pdev->dev, "unable to alloc MSIX/MSI\n");
		goto err_out_1;
	}

	rtase_init_netdev_ops(dev);

	dev->features |= NETIF_F_HW_VLAN_CTAG_TX | NETIF_F_HW_VLAN_CTAG_RX;

	dev->features |= NETIF_F_IP_CSUM;
	dev->features |= NETIF_F_RXCSUM | NETIF_F_SG | NETIF_F_TSO;
	dev->features |= NETIF_F_IPV6_CSUM | NETIF_F_TSO6;
	dev->hw_features = NETIF_F_SG | NETIF_F_IP_CSUM | NETIF_F_TSO |
			   NETIF_F_RXCSUM | NETIF_F_HW_VLAN_CTAG_TX |
			   NETIF_F_HW_VLAN_CTAG_RX;
	dev->hw_features |= NETIF_F_RXALL;
	dev->hw_features |= NETIF_F_RXFCS;
	dev->hw_features |= NETIF_F_IPV6_CSUM | NETIF_F_TSO6;
	dev->vlan_features = NETIF_F_SG | NETIF_F_IP_CSUM | NETIF_F_TSO |
			     NETIF_F_HIGHDMA;
	dev->priv_flags |= IFF_LIVE_ADDR_CHANGE;
	netif_set_tso_max_size(dev, LSO_64K);
	netif_set_tso_max_segs(dev, NIC_MAX_PHYS_BUF_COUNT_LSO2);

	rtase_get_mac_address(dev);

	tp->tally_vaddr = dma_alloc_coherent(&pdev->dev,
					     sizeof(*tp->tally_vaddr),
					     &tp->tally_paddr,
					     GFP_KERNEL);
	if (!tp->tally_vaddr) {
		ret = -ENOMEM;
		goto err_out;
	}

	rtase_tally_counter_clear(tp);

	pci_set_drvdata(pdev, dev);

	ret = register_netdev(dev);
	if (ret != 0)
		goto err_out;

	netdev_dbg(dev, "%pM, IRQ %d\n", dev->dev_addr, dev->irq);

	netif_carrier_off(dev);

	goto out;

err_out:
	if (tp->tally_vaddr) {
		dma_free_coherent(&pdev->dev,
				  sizeof(*tp->tally_vaddr),
				  tp->tally_vaddr,
				  tp->tally_paddr);

		tp->tally_vaddr = NULL;
	}

err_out_1:
	rtase_release_board(pdev, dev, ioaddr);

out:
	return ret;
}

static void rtase_remove_one(struct pci_dev *pdev)
{
	struct net_device *dev = pci_get_drvdata(pdev);
	struct rtase_private *tp = netdev_priv(dev);
	struct rtase_int_vector *ivec;
	u32 i;

	for (i = 0; i < tp->int_nums; i++) {
		ivec = &tp->int_vector[i];
		netif_napi_del(&ivec->napi);
	}

	unregister_netdev(dev);
	rtase_reset_interrupt(pdev, tp);
	if (tp->tally_vaddr) {
		dma_free_coherent(&pdev->dev,
				  sizeof(*tp->tally_vaddr),
				  tp->tally_vaddr,
				  tp->tally_paddr);
		tp->tally_vaddr = NULL;
	}

	rtase_release_board(pdev, dev, tp->mmio_addr);
	pci_set_drvdata(pdev, NULL);
}

static void rtase_shutdown(struct pci_dev *pdev)
{
	struct net_device *dev = pci_get_drvdata(pdev);
	const struct rtase_private *tp = netdev_priv(dev);

	if (netif_running(dev))
		rtase_close(dev);

	rtase_reset_interrupt(pdev, tp);
}

static struct pci_driver rtase_pci_driver = {
	.name = KBUILD_MODNAME,
	.id_table = rtase_pci_tbl,
	.probe = rtase_init_one,
	.remove = rtase_remove_one,
	.shutdown = rtase_shutdown,
};

module_pci_driver(rtase_pci_driver);
